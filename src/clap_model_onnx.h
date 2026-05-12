#ifndef CLAP_TILDE_CLAP_MODEL_ONNX_H
#define CLAP_TILDE_CLAP_MODEL_ONNX_H

// ONNX Runtime backend for clap_tilde.
//
// Mel preprocessing pipeline (STFT → power → mel filterbank → log10 dB → tile 4×)
// is implemented in plain C++ using Essentia's FFTW-backed FFT algorithm.
// No LibTorch dependency.

#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>

#include <essentia/essentia.h>
#include <essentia/algorithmfactory.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bpe_tokenizer.h"
#include "clap_model.h"


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
        // Parse metadata
        auto meta = read_file(meta_json_path);
        m_sample_rate        = parse_int  (meta, "sr");
        m_n_fft              = parse_int  (meta, "n_fft");
        m_hop_length         = parse_int  (meta, "hop_length");
        m_max_text_length    = parse_int  (meta, "max_text_length");
        m_logit_scale_a      = parse_float(meta, "logit_scale_a");
        m_default_context_ms = parse_int  (meta, "default_context_ms");
        m_nb_max_frames      = parse_int  (meta, "nb_max_frames");
        m_context_ms         = m_default_context_ms;

        // Load mel filterbank [513, 64] from binary sidecar
        auto mel_path = meta_json_path.substr(0, meta_json_path.rfind('/') + 1)
                        + "clap_tilde_mel_filters.bin";
        m_mel_filters = load_mel_filters(mel_path);   // [513 * 64] row-major

        // Hann window [n_fft]
        m_hann_window.resize(static_cast<std::size_t>(m_n_fft));
        for (int i = 0; i < m_n_fft; ++i)
            m_hann_window[static_cast<std::size_t>(i)] =
                0.5f * (1.f - std::cos(2.f * static_cast<float>(M_PI) * i / (m_n_fft - 1)));

        // Essentia FFT (FFTW-backed)
        essentia::init();
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        m_fft = factory.create("FFT", "size", m_n_fft);
        m_fft_frame.assign(static_cast<std::size_t>(m_n_fft), 0.f);
        m_fft_spectrum.resize(static_cast<std::size_t>(m_n_fft / 2 + 1));
        m_fft->input ("frame").set(m_fft_frame);
        m_fft->output("fft")  .set(m_fft_spectrum);

        // ONNX sessions
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        if (use_coreml) {
            uint32_t flags = COREML_FLAG_CREATE_MLPROGRAM;
            auto status = OrtSessionOptionsAppendExecutionProvider_CoreML(opts, flags);
            if (status != nullptr)
                Ort::GetApi().ReleaseStatus(status);
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

    ~ClapModelONNX() override {
        delete m_fft;
    }

    void set_context_ms(int ms) override {
        ms = std::max(100, std::min(ms, m_default_context_ms));
        m_context_ms = ms;
    }

    int get_sample_rate()    const override { return m_sample_rate; }
    int get_segment_length() const override {
        return static_cast<int>(std::round(
            static_cast<double>(m_context_ms) * m_sample_rate / 1000.0));
    }

    // Returns [512] float32 L2-normalised audio embedding.
    std::vector<float> encode_audio(std::vector<float> audio) override {
        return run_audio_encoder(audio);
    }

    // Returns [N * 512] float32 row-major.
    std::vector<float> encode_text(const std::vector<std::string>& class_names) override {
        auto [input_ids, attn_mask] = m_tokenizer->encode(class_names);

        const auto N = static_cast<int64_t>(class_names.size());
        const auto L = static_cast<int64_t>(m_max_text_length);
        std::vector<int64_t> shape = {N, L};

        std::array<Ort::Value, 2> inputs = {
            Ort::Value::CreateTensor<int64_t>(m_memory_info,
                input_ids.data(), input_ids.size(), shape.data(), 2),
            Ort::Value::CreateTensor<int64_t>(m_memory_info,
                attn_mask.data(), attn_mask.size(), shape.data(), 2),
        };

        const char* in_names[]  = {m_text_input_ids_name.c_str(), m_text_attn_mask_name.c_str()};
        const char* out_names[] = {m_text_output_name.c_str()};

        auto outputs = m_text_session.Run(
            Ort::RunOptions{nullptr}, in_names, inputs.data(), 2, out_names, 1);

        auto* ptr = outputs[0].GetTensorMutableData<float>();
        return std::vector<float>(ptr, ptr + static_cast<std::size_t>(N * 512));
    }

    // text_embs: [num_classes * 512] row-major
    ClassificationResult classify(std::vector<float> audio,
                                  const std::vector<float>& text_embs,
                                  int num_classes) override {
        auto t1        = std::chrono::high_resolution_clock::now();
        auto audio_emb = run_audio_encoder(audio);  // [512]
        auto t2        = std::chrono::high_resolution_clock::now();

        // logits[i] = exp(scale) * dot(audio_emb, text_embs[i*512..])
        float scale = std::exp(m_logit_scale_a);
        std::vector<float> logits(static_cast<std::size_t>(num_classes));
        for (int i = 0; i < num_classes; ++i) {
            float dot = 0.f;
            for (int j = 0; j < 512; ++j)
                dot += audio_emb[static_cast<std::size_t>(j)]
                     * text_embs [static_cast<std::size_t>(i * 512 + j)];
            logits[static_cast<std::size_t>(i)] = scale * dot;
        }

        // Numerically stable softmax
        float max_l = *std::max_element(logits.begin(), logits.end());
        float sum   = 0.f;
        for (auto& l : logits) { l = std::exp(l - max_l); sum += l; }
        for (auto& l : logits) l /= sum;

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        return ClassificationResult{std::move(logits), static_cast<double>(ns) / 1e6, {}};
    }


private:
    // Shared audio encoder: waveform → features → ONNX → [512]
    std::vector<float> run_audio_encoder(const std::vector<float>& audio) {
        const int64_t nf = static_cast<int64_t>(m_nb_max_frames);
        auto feats = waveform_to_features(audio, nf);  // [4 * nf * 64]

        const std::vector<int64_t> feat_shape = {1, 4, nf, 64};
        auto audio_val = Ort::Value::CreateTensor<float>(
            m_memory_info, feats.data(), feats.size(),
            feat_shape.data(), feat_shape.size());

        const char* in_names[]  = {m_audio_input_name.c_str()};
        const char* out_names[] = {m_audio_output_name.c_str()};

        auto outputs = m_audio_session.Run(
            Ort::RunOptions{nullptr}, in_names, &audio_val, 1, out_names, 1);

        auto* ptr = outputs[0].GetTensorMutableData<float>();
        return std::vector<float>(ptr, ptr + 512);
    }

    // Compute mel features [4 * nb_frames * 64] matching Python CLAP preprocessing:
    //   center-reflect-pad → STFT (Hann, hop) → power → mel filterbank → log10 dB → tile 4×
    // Audio is trimmed to the current context window; mel frames are zero-padded to nb_frames.
    std::vector<float> waveform_to_features(const std::vector<float>& audio, int64_t nb_frames) {
        const std::size_t seg = static_cast<std::size_t>(get_segment_length());

        // 1. Trim / zero-pad to current context window
        std::vector<float> wav(audio.begin(), audio.end());
        if (wav.size() < seg)
            wav.resize(seg, 0.f);
        else if (wav.size() > seg)
            wav.resize(seg);

        // 2. Reflect padding (matches torch.stft center=True)
        const int pad    = m_n_fft / 2;
        const std::size_t N = wav.size();
        std::vector<float> padded(static_cast<std::size_t>(pad) + N + static_cast<std::size_t>(pad));
        for (int i = 0; i < pad; ++i)
            padded[static_cast<std::size_t>(pad - 1 - i)] = wav[static_cast<std::size_t>(i + 1)];
        std::copy(wav.begin(), wav.end(), padded.begin() + pad);
        for (int i = 0; i < pad; ++i)
            padded[static_cast<std::size_t>(pad) + N + static_cast<std::size_t>(i)] =
                wav[N - 2 - static_cast<std::size_t>(i)];

        // 3. STFT frame-by-frame → mel → log10 dB
        int64_t T_actual = (static_cast<int64_t>(padded.size()) - m_n_fft) / m_hop_length + 1;
        int64_t T        = std::min(T_actual, nb_frames);

        // log_mel: [nb_frames, 64] zero-initialised (silence padding for t >= T)
        std::vector<float> log_mel(static_cast<std::size_t>(nb_frames * 64), 0.f);

        for (int64_t t = 0; t < T; ++t) {
            // Windowed frame → FFT
            std::size_t offset = static_cast<std::size_t>(t * m_hop_length);
            for (int i = 0; i < m_n_fft; ++i)
                m_fft_frame[static_cast<std::size_t>(i)] =
                    padded[offset + static_cast<std::size_t>(i)]
                    * m_hann_window[static_cast<std::size_t>(i)];
            m_fft->compute();

            // Mel filterbank: power[k] * mel_filters[k, j]  →  mel[j]
            std::size_t row = static_cast<std::size_t>(t * 64);
            for (int j = 0; j < 64; ++j) {
                float mel_val = 0.f;
                for (int k = 0; k < 513; ++k) {
                    float re = m_fft_spectrum[static_cast<std::size_t>(k)].real();
                    float im = m_fft_spectrum[static_cast<std::size_t>(k)].imag();
                    mel_val += m_mel_filters[static_cast<std::size_t>(k * 64 + j)] * (re*re + im*im);
                }
                log_mel[row + static_cast<std::size_t>(j)] =
                    10.f * std::log10(std::max(mel_val, 1e-10f));
            }
        }

        // 4. Tile 4× → [1, 4, nb_frames, 64] flattened
        std::size_t frame_sz = static_cast<std::size_t>(nb_frames * 64);
        std::vector<float> output(4 * frame_sz);
        for (int c = 0; c < 4; ++c)
            std::copy(log_mel.begin(), log_mel.end(),
                      output.begin() + static_cast<std::ptrdiff_t>(c * frame_sz));
        return output;
    }


    static std::vector<float> load_mel_filters(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open mel filters: " + path);
        std::vector<float> data(513 * 64);
        f.read(reinterpret_cast<char*>(data.data()), 513 * 64 * sizeof(float));
        if (!f) throw std::runtime_error("Failed to read mel filters from: " + path);
        return data;
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

    // ONNX
    Ort::Env        m_env;
    Ort::Session    m_audio_session;
    Ort::Session    m_text_session;
    Ort::MemoryInfo m_memory_info;

    std::unique_ptr<BPETokenizer> m_tokenizer;

    // DSP (Essentia FFT + mel)
    essentia::standard::Algorithm*        m_fft = nullptr;
    std::vector<essentia::Real>           m_fft_frame;     // [n_fft]
    std::vector<std::complex<essentia::Real>> m_fft_spectrum; // [n_fft/2+1]
    std::vector<float>                    m_mel_filters;   // [513 * 64] row-major
    std::vector<float>                    m_hann_window;   // [n_fft]

    // Model parameters
    int   m_sample_rate;
    int   m_default_context_ms;
    int   m_context_ms;
    int   m_nb_max_frames;
    int   m_n_fft;
    int   m_hop_length;
    int   m_max_text_length;
    float m_logit_scale_a;

    // ONNX I/O names
    std::string m_audio_input_name;
    std::string m_audio_output_name;
    std::string m_text_input_ids_name;
    std::string m_text_attn_mask_name;
    std::string m_text_output_name;
};


#endif // CLAP_TILDE_CLAP_MODEL_ONNX_H
