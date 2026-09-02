#ifndef RUSC_TILDE_CLAP_MODEL_ONNX_H
#define RUSC_TILDE_CLAP_MODEL_ONNX_H

// ONNX Runtime backend for rusc_tilde.
//
// Mel preprocessing (STFT → power → mel filterbank → log10 dB → tile 4×) is
// implemented in MelFrontend with pocketfft. No LibTorch / Essentia dependency.

#include <onnxruntime_cxx_api.h>
#ifdef __APPLE__
#include <coreml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "bpe_tokenizer.h"
#include "clap_model.h"
#include "mel_frontend.h"


class ClapModelONNX : public IClapModel {
public:
    static constexpr int EMB_DIM = 512;

    ClapModelONNX(const std::string& audio_onnx_path,
                  const std::string& text_onnx_path,
                  const std::string& meta_json_path,
                  const std::string& tokenizer_dir,
                  bool use_ane = false,
                  int intra_op_threads = 1)
        : m_env(ORT_LOGGING_LEVEL_WARNING, "rusc_tilde")
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

        // Mel filterbank [n_fft/2+1, 64] from the binary sidecar next to the meta file
        auto mel_path = (std::filesystem::u8path(meta_json_path).parent_path()
                         / "clap_mel_filters.bin").u8string();
        const std::size_t n_bins = static_cast<std::size_t>(m_n_fft / 2 + 1);
        m_frontend = std::make_unique<MelFrontend>(
            m_n_fft, m_hop_length, load_mel_filters(mel_path, n_bins * MelFrontend::N_MELS));

        // ONNX sessions
        // intra-op threads parallelise the operators inside the encoder graph;
        // on an M4 Pro the audio encoder goes from ~53 ms (1 thread) to ~32 ms (4).
        Ort::SessionOptions audio_opts;
        audio_opts.SetIntraOpNumThreads(std::max(1, intra_op_threads));
        audio_opts.SetInterOpNumThreads(1);
        audio_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // The text encoder always stays on the CPU provider. It runs on the
        // encoder thread, concurrently with audio inference; running two CoreML
        // models at once inside the host (which uses Metal for its own UI) is
        // not worth the risk, and CoreML brings nothing to a one-off encoding.
        Ort::SessionOptions text_opts;
        text_opts.SetIntraOpNumThreads(std::max(1, intra_op_threads));
        text_opts.SetInterOpNumThreads(1);
        text_opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // Apple Neural Engine via the CoreML execution provider, audio session
        // only. MLProgram is required — the older NeuralNetwork format has no ANE
        // path for the encoder's attention blocks. Elsewhere the flag is a no-op
        // and the encoder stays on the ORT CPU provider.
#ifdef __APPLE__
        if (use_ane) {
            uint32_t flags = COREML_FLAG_CREATE_MLPROGRAM;
            auto status = OrtSessionOptionsAppendExecutionProvider_CoreML(audio_opts, flags);
            if (status != nullptr)
                Ort::GetApi().ReleaseStatus(status);
        }
#else
        (void)use_ane;
#endif

        m_audio_session = Ort::Session(m_env, ort_path(audio_onnx_path).c_str(), audio_opts);
        m_text_session  = Ort::Session(m_env, ort_path(text_onnx_path).c_str(),  text_opts);

        // Cache input/output names
        Ort::AllocatorWithDefaultOptions alloc;
        m_audio_input_name     = m_audio_session.GetInputNameAllocated (0, alloc).get();
        m_audio_output_name    = m_audio_session.GetOutputNameAllocated(0, alloc).get();
        m_text_input_ids_name  = m_text_session.GetInputNameAllocated  (0, alloc).get();
        m_text_attn_mask_name  = m_text_session.GetInputNameAllocated  (1, alloc).get();
        m_text_output_name     = m_text_session.GetOutputNameAllocated (0, alloc).get();

        m_tokenizer = std::make_unique<BPETokenizer>(tokenizer_dir, m_max_text_length);
    }

    ~ClapModelONNX() override = default;

    void set_context_ms(int ms) override {
        ms = std::max(100, std::min(ms, m_default_context_ms));
        m_context_ms = ms;
    }

    int get_sample_rate()    const override { return m_sample_rate; }
    int get_context_ms()     const override { return m_context_ms; }
    int get_max_context_ms() const override { return m_default_context_ms; }
    int get_segment_length() const override {
        return static_cast<int>(std::round(
            static_cast<double>(m_context_ms) * m_sample_rate / 1000.0));
    }

    // Returns [512] float32 L2-normalised audio embedding.
    std::vector<float> encode_audio(const std::vector<float>& audio) override {
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
        return std::vector<float>(ptr, ptr + static_cast<std::size_t>(N * EMB_DIM));
    }

    // text_embs: [num_classes * 512] row-major
    ClassificationResult classify(const std::vector<float>& audio,
                                  const std::vector<float>& text_embs,
                                  int num_classes) override {
        auto t1        = std::chrono::steady_clock::now();
        auto audio_emb = run_audio_encoder(audio);  // [512]
        auto t2        = std::chrono::steady_clock::now();

        // logits[i] = exp(scale) * dot(audio_emb, text_embs[i*512..])
        float scale = std::exp(m_logit_scale_a);
        std::vector<float> logits(static_cast<std::size_t>(num_classes));
        for (int i = 0; i < num_classes; ++i) {
            float dot = 0.f;
            for (int j = 0; j < EMB_DIM; ++j)
                dot += audio_emb[static_cast<std::size_t>(j)]
                     * text_embs [static_cast<std::size_t>(i * EMB_DIM + j)];
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
    // ONNX Runtime takes wide-char paths on Windows and UTF-8 elsewhere.
#ifdef _WIN32
    static std::wstring ort_path(const std::string& utf8) {
        return std::filesystem::u8path(utf8).wstring();
    }
#else
    static const std::string& ort_path(const std::string& utf8) { return utf8; }
#endif

    // Shared audio encoder: waveform → features → ONNX → [512].
    // Serialised: classify() (inference thread) and encode_audio() (few-shot
    // encoder thread) share the front-end buffers. Text encoding is not
    // affected and runs concurrently.
    std::vector<float> run_audio_encoder(const std::vector<float>& audio) {
        std::lock_guard<std::mutex> lock{m_audio_mutex};
        const int64_t nf = static_cast<int64_t>(m_nb_max_frames);
        // Features live in a buffer owned by the front-end; the tensor only wraps it.
        auto& feats = m_frontend->compute(
            audio, static_cast<std::size_t>(get_segment_length()), nf);   // [4 * nf * 64]

        const std::vector<int64_t> feat_shape = {1, 4, nf, MelFrontend::N_MELS};
        auto audio_val = Ort::Value::CreateTensor<float>(
            m_memory_info, feats.data(), feats.size(),
            feat_shape.data(), feat_shape.size());

        const char* in_names[]  = {m_audio_input_name.c_str()};
        const char* out_names[] = {m_audio_output_name.c_str()};

        auto outputs = m_audio_session.Run(
            Ort::RunOptions{nullptr}, in_names, &audio_val, 1, out_names, 1);

        auto* ptr = outputs[0].GetTensorMutableData<float>();
        return std::vector<float>(ptr, ptr + EMB_DIM);
    }


    static std::vector<float> load_mel_filters(const std::string& path, std::size_t count) {
        std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
        if (!f) throw std::runtime_error("Cannot open mel filters: " + path);
        std::vector<float> data(count);
        f.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(count * sizeof(float)));
        if (!f) throw std::runtime_error("Failed to read mel filters from: " + path);
        return data;
    }

    static std::string read_file(const std::string& path) {
        std::ifstream f(std::filesystem::u8path(path));
        if (!f) throw std::runtime_error("Cannot open: " + path);
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // Minimal flat-JSON lookup: returns the raw text following `"key":`.
    // Only matches a quoted key that is followed by ':' (skipping whitespace),
    // so a string value that happens to contain the key is not mistaken for it.
    static std::string find_value(const std::string& json, const std::string& key) {
        const std::string quoted = "\"" + key + "\"";
        std::size_t pos = 0;
        while ((pos = json.find(quoted, pos)) != std::string::npos) {
            std::size_t after = pos + quoted.size();
            while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) ++after;
            if (after < json.size() && json[after] == ':') {
                ++after;
                while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) ++after;
                return json.substr(after);
            }
            pos = after;
        }
        throw std::runtime_error("Key not found in meta JSON: " + key);
    }

    static int parse_int(const std::string& json, const std::string& key) {
        try { return std::stoi(find_value(json, key)); }
        catch (const std::invalid_argument&) {
            throw std::runtime_error("Invalid integer for key in meta JSON: " + key);
        }
    }

    static float parse_float(const std::string& json, const std::string& key) {
        try { return std::stof(find_value(json, key)); }
        catch (const std::invalid_argument&) {
            throw std::runtime_error("Invalid float for key in meta JSON: " + key);
        }
    }

    // ONNX
    Ort::Env        m_env;
    Ort::Session    m_audio_session;
    Ort::Session    m_text_session;
    Ort::MemoryInfo m_memory_info;

    std::unique_ptr<BPETokenizer> m_tokenizer;
    std::unique_ptr<MelFrontend>  m_frontend;
    std::mutex                    m_audio_mutex;

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


#endif // RUSC_TILDE_CLAP_MODEL_ONNX_H
