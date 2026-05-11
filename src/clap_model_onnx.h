#ifndef CLAP_TILDE_CLAP_MODEL_ONNX_H
#define CLAP_TILDE_CLAP_MODEL_ONNX_H

// ONNX Runtime backend for clap_tilde.
//
// Expected files (all in the same directory as the audio .onnx):
//   clap_tilde_audio_<N>ms.onnx      — audio encoder  (input: mel features [1,4,F,64])
//   clap_tilde_text.onnx              — text encoder
//   clap_tilde_meta.json              — sr, seglen, nb_max_frames, n_fft, hop_length,
//                                       max_text_length, logit_scale_a
//   clap_tilde_mel_filters.bin        — float32 [513 × 64] mel filterbank (row-major)
//   vocab.json + merges.txt           — BPE tokenizer files
//
// Mel preprocessing (STFT → power → filterbank → log10 dB → tile 4×) is performed
// in C++ using LibTorch before the ONNX audio session, matching export_clap.py exactly.

#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>
#include <torch/torch.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bpe_tokenizer.h"
#include "clap_model.h"   // ClassificationResult, IClapModel


class ClapModelONNX : public IClapModel {
public:
    ClapModelONNX(const std::string& audio_onnx_path,
                  const std::string& text_onnx_path,
                  const std::string& meta_json_path,
                  const std::string& tokenizer_dir,
                  bool use_coreml = false)
        : m_env(ORT_LOGGING_LEVEL_WARNING, "clap_tilde")
        , m_audio_session(nullptr)
        , m_text_session(nullptr)
        , m_memory_info(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        at::init_num_threads();

        // Parse metadata
        auto meta = read_file(meta_json_path);
        m_sample_rate     = parse_int  (meta, "sr");
        m_n_fft           = parse_int  (meta, "n_fft");
        m_hop_length      = parse_int  (meta, "hop_length");
        m_max_text_length = parse_int  (meta, "max_text_length");
        m_logit_scale_a   = parse_float(meta, "logit_scale_a");
        m_context_ms      = parse_int  (meta, "default_context_ms");

        // Load mel filterbank [513, 64] from binary sidecar
        auto mel_path = meta_json_path.substr(0, meta_json_path.rfind('/') + 1)
                        + "clap_tilde_mel_filters.bin";
        m_mel_filters = load_mel_filters(mel_path);          // [513, 64]
        m_hann_window = torch::hann_window(m_n_fft);         // [n_fft]

        // ONNX sessions
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        if (use_coreml) {
            uint32_t flags = COREML_FLAG_CREATE_MLPROGRAM;
            auto status = OrtSessionOptionsAppendExecutionProvider_CoreML(opts, flags);
            if (status != nullptr) {
                // Non-fatal: log and continue with CPU fallback
                Ort::GetApi().ReleaseStatus(status);
            }
        }

        m_audio_session = Ort::Session(m_env, audio_onnx_path.c_str(), opts);
        m_text_session  = Ort::Session(m_env, text_onnx_path.c_str(),  opts);

        // Cache input/output names
        Ort::AllocatorWithDefaultOptions alloc;
        m_audio_input_name     = m_audio_session.GetInputNameAllocated (0, alloc).get();
        m_audio_output_name    = m_audio_session.GetOutputNameAllocated(0, alloc).get();
        m_text_input_ids_name  = m_text_session.GetInputNameAllocated  (0, alloc).get();
        m_text_attn_mask_name  = m_text_session.GetInputNameAllocated  (1, alloc).get();
        m_text_output_name     = m_text_session.GetOutputNameAllocated (0, alloc).get();

        m_tokenizer = std::make_unique<BPETokenizer>(tokenizer_dir, m_max_text_length);
    }


    void set_context_ms(int ms) override { m_context_ms = std::max(100, ms); }

    int get_sample_rate()    const override { return m_sample_rate; }
    int get_segment_length() const override {
        return static_cast<int>(std::round(m_context_ms * m_sample_rate / 1000.0));
    }

    // Run audio encoder only → [1, 512] normalised embedding on CPU.
    at::Tensor encode_audio(std::vector<float> audio) override {
        const int64_t nf = compute_nb_frames();
        auto feats = waveform_to_features(audio, nf);

        const std::vector<int64_t> feat_shape = {1, 4, nf, 64};
        auto feat_ptr = feats.data_ptr<float>();

        auto audio_val = Ort::Value::CreateTensor<float>(
            m_memory_info, feat_ptr,
            static_cast<std::size_t>(feats.numel()),
            feat_shape.data(), feat_shape.size());

        const char* in_names[]  = {m_audio_input_name.c_str()};
        const char* out_names[] = {m_audio_output_name.c_str()};

        auto outputs = m_audio_session.Run(
            Ort::RunOptions{nullptr}, in_names, &audio_val, 1, out_names, 1);

        return torch::from_blob(
            outputs[0].GetTensorMutableData<float>(), {1, 512}, torch::kFloat32).clone();
    }

    // Encode N class names → [N, 512] float32 tensor on CPU.
    at::Tensor encode_text(const std::vector<std::string>& class_names) override {
        auto [input_ids, attn_mask] = m_tokenizer->encode(class_names);

        const auto N = static_cast<int64_t>(class_names.size());
        const auto L = static_cast<int64_t>(m_max_text_length);
        std::vector<int64_t> shape = {N, L};

        auto ids_ptr  = input_ids.contiguous().data_ptr<int64_t>();
        auto mask_ptr = attn_mask.contiguous().data_ptr<int64_t>();

        std::array<Ort::Value, 2> inputs = {
            Ort::Value::CreateTensor<int64_t>(m_memory_info, ids_ptr,  N * L, shape.data(), 2),
            Ort::Value::CreateTensor<int64_t>(m_memory_info, mask_ptr, N * L, shape.data(), 2),
        };

        const char* in_names[]  = {m_text_input_ids_name.c_str(), m_text_attn_mask_name.c_str()};
        const char* out_names[] = {m_text_output_name.c_str()};

        auto outputs = m_text_session.Run(
            Ort::RunOptions{nullptr}, in_names, inputs.data(), 2, out_names, 1);

        return torch::from_blob(
            outputs[0].GetTensorMutableData<float>(), {N, 512}, torch::kFloat32).clone();
    }


    // Run audio inference with pre-computed text embeddings.
    ClassificationResult classify(std::vector<float> audio,
                                  const at::Tensor& text_embs) override {
        const int64_t nf = compute_nb_frames();
        auto feats = waveform_to_features(audio, nf);

        // ── ONNX audio encoder ────────────────────────────────────────────────
        const std::vector<int64_t> feat_shape = {1, 4, nf, 64};
        auto feat_ptr = feats.data_ptr<float>();

        auto audio_val = Ort::Value::CreateTensor<float>(
            m_memory_info, feat_ptr,
            static_cast<std::size_t>(feats.numel()),
            feat_shape.data(), feat_shape.size());

        const char* in_names[]  = {m_audio_input_name.c_str()};
        const char* out_names[] = {m_audio_output_name.c_str()};

        auto t1 = std::chrono::high_resolution_clock::now();
        auto outputs = m_audio_session.Run(
            Ort::RunOptions{nullptr}, in_names, &audio_val, 1, out_names, 1);
        auto t2 = std::chrono::high_resolution_clock::now();

        // ── Similarity + softmax ──────────────────────────────────────────────
        auto audio_emb = torch::from_blob(
            outputs[0].GetTensorMutableData<float>(), {1, 512}, torch::kFloat32);

        auto logits = std::exp(m_logit_scale_a)
                      * torch::mm(audio_emb, text_embs.t());   // [1, M]
        auto probs  = torch::softmax(logits[0], 0);            // [M]

        auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();

        std::vector<float> dist(static_cast<std::size_t>(probs.numel()));
        std::copy(probs.data_ptr<float>(),
                  probs.data_ptr<float>() + probs.numel(), dist.begin());

        return ClassificationResult{std::move(dist),
                                    static_cast<double>(latency_ns) / 1e6};
    }


private:
    int64_t compute_nb_frames() const {
        return get_segment_length() / m_hop_length + 1;
    }

    // Compute CLAP mel features from raw float32 audio (at model sample rate).
    // nb_frames: exact time-axis size the caller will pass to ORT — audio is
    // padded / trimmed to produce exactly that many frames.
    at::Tensor waveform_to_features(const std::vector<float>& audio, int64_t nb_frames) {
        const int64_t seg = get_segment_length();
        auto wav = torch::from_blob(
            const_cast<float*>(audio.data()),
            {static_cast<long>(audio.size())}, torch::kFloat32).clone();

        // Pad / trim to segment_length samples
        auto n = wav.size(0);
        if (n < seg)
            wav = at::pad(wav, {0, seg - n});
        else if (n > seg)
            wav = wav.slice(0, 0, seg);

        // Center padding (reflect) — matches torch.stft(center=True)
        auto pad = m_n_fft / 2;
        auto padded = at::pad(wav.unsqueeze(0), {pad, pad}, "reflect").squeeze(0);

        // STFT → complex [513, T]
        auto stft_c = torch::stft(
            padded, m_n_fft, m_hop_length, m_n_fft,
            m_hann_window, /*normalized=*/false,
            /*onesided=*/true, /*return_complex=*/true);

        // Power spectrum [513, T]
        auto stft_r = torch::view_as_real(stft_c);
        auto power  = stft_r.select(-1, 0).pow(2) + stft_r.select(-1, 1).pow(2);

        // Mel filterbank + log10 dB  [64, T]
        auto mel     = torch::mm(m_mel_filters.t(), power);
        auto log_mel = 10.0f * torch::log10(torch::clamp(mel, /*min=*/1e-10f));

        // Trim / pad time axis to nb_frames
        log_mel = log_mel.t();
        auto T = log_mel.size(0);
        if (T > nb_frames)
            log_mel = log_mel.slice(0, 0, nb_frames);
        else if (T < nb_frames)
            log_mel = at::pad(log_mel, {0, 0, 0, nb_frames - T});

        // CLAP "fusion" format: tile 4×  →  [1, 4, nb_frames, 64]
        return log_mel.unsqueeze(0).expand({4, -1, -1}).unsqueeze(0).contiguous();
    }


    static at::Tensor load_mel_filters(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open mel filters: " + path);
        std::vector<float> data(513 * 64);
        f.read(reinterpret_cast<char*>(data.data()), 513 * 64 * sizeof(float));
        if (!f) throw std::runtime_error("Failed to read mel filters from: " + path);
        return torch::from_blob(data.data(), {513, 64}, torch::kFloat32).clone();
    }

    static std::string read_file(const std::string& path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("Cannot open: " + path);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static int parse_int(const std::string& json, const std::string& key) {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos)
            throw std::runtime_error("Key not found in meta JSON: " + key);
        pos = json.find(':', pos) + 1;
        return std::stoi(json.substr(pos));
    }

    static float parse_float(const std::string& json, const std::string& key) {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos)
            throw std::runtime_error("Key not found in meta JSON: " + key);
        pos = json.find(':', pos) + 1;
        return std::stof(json.substr(pos));
    }

    Ort::Env         m_env;
    Ort::Session     m_audio_session;
    Ort::Session     m_text_session;
    Ort::MemoryInfo  m_memory_info;

    std::unique_ptr<BPETokenizer> m_tokenizer;

    at::Tensor m_mel_filters;   // [513, 64]
    at::Tensor m_hann_window;   // [n_fft]

    int   m_sample_rate;
    int   m_context_ms;
    int   m_n_fft;
    int   m_hop_length;
    int   m_max_text_length;
    float m_logit_scale_a;

    std::string m_audio_input_name;
    std::string m_audio_output_name;
    std::string m_text_input_ids_name;
    std::string m_text_attn_mask_name;
    std::string m_text_output_name;
};


#endif // CLAP_TILDE_CLAP_MODEL_ONNX_H
