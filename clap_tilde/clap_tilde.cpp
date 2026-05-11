#include "c74_min.h"
#include <chrono>
#include <dirent.h>
#include <pthread.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>

#include "clap_classifier.h"
#include "clap_model_onnx.h"
#include "leaky_integrator.h"
#include "utility.h"

using namespace c74::min;

static bool path_is_dir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static bool path_exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}
static std::string path_join(const std::string& a, const std::string& b) {
    return a.back() == '/' ? a + b : a + "/" + b;
}
static std::string path_parent(const std::string& p) {
    auto pos = p.rfind('/');
    return pos == std::string::npos ? "." : p.substr(0, pos);
}
static std::string path_extension(const std::string& p) {
    auto pos = p.rfind('.');
    auto sl  = p.rfind('/');
    if (pos == std::string::npos || (sl != std::string::npos && pos < sl)) return "";
    return p.substr(pos);
}

struct Docs {
    static const inline title VERBOSE_TITLE           = "Verbose";
    static const inline title ENABLED_TITLE           = "Enabled";
    static const inline title SENSITIVITY_TITLE       = "Sensitivity";
    static const inline title SENSITIVITY_RANGE_TITLE = "Sensitivity Range";
    static const inline title THRESHOLD_TITLE         = "Threshold";
    static const inline title WINDOW_TITLE            = "Window";
    static const inline title CONFIDENCE_TITLE        = "Confidence";

    static const inline description VERBOSE_DESCRIPTION =
        "Enable or disable verbose logging.";
    static const inline description ENABLED_DESCRIPTION =
        "Enable or disable classification. When @enabled 1 the model is running.";
    static const inline description SENSITIVITY_DESCRIPTION =
        "Adjust sensitivity of classification output (0.–1.).";
    static const inline description SENSITIVITY_RANGE_DESCRIPTION =
        "Time window in ms for sensitivity scaling (0–2000).";
    static const inline description THRESHOLD_DESCRIPTION =
        "Minimum energy in dB required for classification (-80–0).";
    static const inline description WINDOW_DESCRIPTION =
        "Sliding window size in ms for energy thresholding.";
    static const inline description CONFIDENCE_DESCRIPTION =
        "Minimum confidence (0.–1.) to output a class; below threshold outputs 'no_confidence'.";
    static const inline description SET_CLASS_DESCRIPTION =
        "Set class names for zero-shot CLAP inference. "
        "Separate class names with a comma atom. "
        "Example (message box): set_class violin pizzicato , flute multiphonics";
};


class clap_tilde : public object<clap_tilde>, public vector_operator<> {
private:
    std::unique_ptr<ClapClassifier> m_classifier;

    std::thread                     m_processing_thread;
    c74::min::fifo<double>          m_audio_fifo{65536};
    c74::min::fifo<ClassificationResult> m_event_fifo{100};

    std::atomic<bool> m_running          = false;
    std::atomic<bool> m_enabled          = true;
    std::atomic<bool> m_model_initialized = false;

    LeakyIntegrator m_integrator;

public:
    MIN_DESCRIPTION{"Real-time zero-shot audio classification using laion/clap-htsat-fused."};
    MIN_TAGS{""};
    MIN_AUTHOR{"Nicolas Brochec"};
    MIN_RELATED{""};

    inlet<>  inlet_main  {this, "(signal) audio input", ""};
    inlet<>  inlet_record{this, "(list) record label buffer_name — register a few-shot audio example", "list"};

    outlet<> outlet_main        {this, "(int) recognized class index"};
    outlet<> outlet_classname   {this, "(symbol) recognized class name"};
    outlet<> outlet_distribution{this, "(list) class probability distribution"};
    outlet<> dumpout            {this, "(any) dumpout"};

    // First arg: path to clap_tilde.ts (or the directory containing it + vocab.json + merges.txt)
    // Second arg (optional): device — "cpu" or "mps"  (default: cpu)
    argument<symbol> model_arg {this, "model",
        "Path to clap_tilde.ts, or the directory containing it. "
        "vocab.json and merges.txt must be in the same directory."};
    argument<symbol> device_arg{this, "device",
        "Inference device: 'cpu' or 'mps'. Optional, defaults to 'cpu'."};

    explicit clap_tilde(const atoms& args = {}) {
        try {
            auto paths = parse_paths(args);
            bool use_coreml = parse_use_coreml(args);
            cout << "[clap~] model: " << paths.model_path << endl;
            cout << "[clap~] ONNX backend" << (use_coreml ? " — CoreML EP (MPS)" : " — CPU") << endl;

            m_classifier = std::make_unique<ClapClassifier>(
                [p = paths, use_coreml]() {
                    return std::make_unique<ClapModelONNX>(
                        p.model_path, p.text_onnx_path, p.meta_json_path,
                        p.tokenizer_dir, use_coreml);
                });
            cout << "[clap~] classifier created, loading model on background thread..." << endl;
        } catch (std::exception& e) {
            error(e.what());
        }
    }

    ~clap_tilde() override {
        if (m_processing_thread.joinable()) {
            m_running = false;
            m_processing_thread.join();
        }
    }

    message<> maxclass_setup{this, "maxclass_setup",
        [this](const c74::min::atoms&, const int) -> c74::min::atoms {
            cout << " clap~ v1.0.0 (2026) by Nicolas Brochec" << endl;
            cout << " Zero-shot real-time audio classification — laion/clap-htsat-fused" << endl;
            cout << " IRCAM, RepMus REACH team" << endl;
            return {};
        }
    };


    // Dequeue inference results on the main thread and send to outlets
    timer<> deliverer{this, MIN_FUNCTION {
        ClassificationResult result;
        while (m_event_fifo.try_dequeue(result)) {
            assert(m_model_initialized);
            assert(m_classifier);

            auto smoothed = m_integrator.process(result.distribution);

            auto index = static_cast<long>(util::argmax(smoothed));
            float top  = smoothed[static_cast<std::size_t>(index)];

            if (top >= static_cast<float>(confidence.get())) {
                // outlet_main: winning class index
                outlet_main.send(index);

                // outlet_classname: winning class name (from result, matches distribution length)
                if (!result.class_names.empty()
                    && static_cast<std::size_t>(index) < result.class_names.size())
                    outlet_classname.send(result.class_names.at(static_cast<std::size_t>(index)));

                // outlet_distribution: smoothed probability list
                atoms dist;
                dist.reserve(smoothed.size());
                for (auto v : smoothed) dist.emplace_back(static_cast<double>(v));
                outlet_distribution.send(dist);
            }

            atoms latency{"latency"};
            latency.emplace_back(result.inference_latency_ms);
            dumpout.send(latency);
        }
        return {};
    }};


    void operator()(audio_bundle in, audio_bundle) override {
        if (in.channel_count() > 0 && m_running && m_enabled) {
            auto* s = in.samples(0);
            for (auto i = 0; i < in.frame_count(); ++i)
                m_audio_fifo.try_enqueue(static_cast<double>(s[i]));
        }
    }


    // ── Attributes ──────────────────────────────────────────────────────────

    attribute<bool> verbose{this, "verbose", false,
        Docs::VERBOSE_TITLE, Docs::VERBOSE_DESCRIPTION};

    attribute<bool> enabled{this, "enabled", true,
        Docs::ENABLED_TITLE, Docs::ENABLED_DESCRIPTION,
        setter{MIN_FUNCTION {
            if (args[0].type() == c74::min::message_type::int_argument) {
                m_enabled = static_cast<bool>(args[0]);
                return args;
            }
            cerr << "bad argument for message \"enabled\"" << endl;
            return enabled;
        }}
    };

    attribute<double> sensitivity{this, "sensitivity", 1.0,
        Docs::SENSITIVITY_TITLE, Docs::SENSITIVITY_DESCRIPTION,
        setter{MIN_FUNCTION {
            if (args.size() == 1 && args[0].type() == c74::min::message_type::float_argument) {
                auto tau = std::min(1.0, std::max(0.0, static_cast<double>(args[0])));
                m_integrator.set_tau((1.0 - tau) * static_cast<double>(sensitivityrange.get()));
                return {tau};
            }
            cerr << "bad argument for message \"sensitivity\"" << endl;
            return sensitivity;
        }}
    };

    attribute<int> sensitivityrange{this, "sensitivityrange", 2000,
        Docs::SENSITIVITY_RANGE_TITLE, Docs::SENSITIVITY_RANGE_DESCRIPTION,
        setter{MIN_FUNCTION {
            if (args.size() == 1
                && args[0].type() == c74::min::message_type::int_argument
                && static_cast<int>(args[0]) > 0)
            {
                double cur = std::clamp(static_cast<double>(sensitivity.get()), 0.0, 1.0);
                double new_tau = std::max((1.0 - cur) * static_cast<double>(args[0]), 1e-6);
                m_integrator.set_tau(new_tau);
                return args;
            }
            cerr << "bad argument for message \"sensitivityrange\"" << endl;
            return sensitivityrange;
        }}
    };

    attribute<double> threshold{this, "threshold", EnergyThreshold::MINIMUM_THRESHOLD,
        Docs::THRESHOLD_TITLE, Docs::THRESHOLD_DESCRIPTION,
        setter{MIN_FUNCTION {
            if (args.size() == 1
                && (args[0].type() == c74::min::message_type::float_argument
                    || args[0].type() == c74::min::message_type::int_argument))
            {
                if (m_classifier) m_classifier->set_energy_threshold(static_cast<float>(args[0]));
                return args;
            }
            cerr << "bad argument for message \"threshold\"" << endl;
            return threshold;
        }}
    };

    attribute<int> window{this, "window", ClapClassifier::DEFAULT_THRESHOLD_WINDOW_MS,
        Docs::WINDOW_TITLE, Docs::WINDOW_DESCRIPTION,
        setter{MIN_FUNCTION {
            if (args.size() == 1
                && (args[0].type() == c74::min::message_type::int_argument
                    || args[0].type() == c74::min::message_type::float_argument))
            {
                if (m_classifier) m_classifier->set_threshold_window(static_cast<int>(args[0]));
                return args;
            }
            cerr << "bad argument for message \"window\"" << endl;
            return window;
        }}
    };

    attribute<int> context{this, "context", 1000,
        title {"Context Length"},
        description {"Audio context window for inference in milliseconds. "
                     "Longer windows capture more temporal structure but increase latency. "
                     "Re-exports the model are not required — context is set at runtime."},
        setter{MIN_FUNCTION {
            if (args.size() == 1
                && (args[0].type() == c74::min::message_type::int_argument
                    || args[0].type() == c74::min::message_type::float_argument))
            {
                int ms = std::max(100, static_cast<int>(args[0]));
                if (m_classifier) m_classifier->set_context_ms(ms);
                return {ms};
            }
            cerr << "bad argument for message \"context\"" << endl;
            return context;
        }}
    };

    attribute<double> confidence{this, "confidence", 0.0,
        Docs::CONFIDENCE_TITLE, Docs::CONFIDENCE_DESCRIPTION,
        setter{MIN_FUNCTION {
            if (args.size() == 1 && args[0].type() == c74::min::message_type::float_argument) {
                return {std::min(1.0, std::max(0.0, static_cast<double>(args[0])))};
            }
            cerr << "bad argument for message \"confidence\"" << endl;
            return confidence;
        }}
    };


    // ── Messages ─────────────────────────────────────────────────────────────

    // set_classes "drums" "hi-hat" "voice" "bass drum"
    // Each atom (quoted or not) is one class name.
    message<> set_classes{this, "set_classes", Docs::SET_CLASS_DESCRIPTION,
        MIN_FUNCTION {
            if (inlet != 0) {
                cerr << "invalid message \"set_classes\" for inlet " << inlet << endl;
                return {};
            }
            if (!m_running) {
                cerr << "cannot set classes: model is not loaded yet" << endl;
                return {};
            }

            std::vector<std::string> class_names;
            for (const auto& a : args)
                class_names.push_back(std::string(a));

            if (class_names.empty()) {
                cerr << "set_classes: no class names provided" << endl;
                return {};
            }

            cout << "[clap~] set_classes: " << class_names.size() << " classes" << endl;
            for (const auto& n : class_names) cout << "  - " << n << endl;

            m_classifier->set_classes(std::move(class_names));
            return {};
        }
    };

    // Output current class names to dumpout
    message<> classnames{this, "classnames",
        "Output current class names via dumpout.",
        MIN_FUNCTION {
            if (inlet != 0) {
                cerr << "invalid message \"classnames\" for inlet " << inlet << endl;
                return {};
            }
            if (!m_running) {
                cerr << "cannot get classnames: model is not loaded" << endl;
                return {};
            }
            auto names = m_classifier->get_class_names();
            atoms out{"classnames"};
            for (const auto& n : names) out.emplace_back(n);
            dumpout.send(out);
            return {};
        }
    };

    // ── Few-shot audio example registration ──────────────────────────────────

    // record <label> <buffer_name>  (send to inlet 2)
    // Reads a buffer~, encodes its audio embedding, stores it as a custom class.
    // Multiple calls with the same label overwrite the previous example.
    // Buffer must be mono or stereo (only channel 0 is used); any sample rate accepted
    // (linear-interpolation resample to 48 kHz before encoding).
    message<> record_msg{this, "record",
        "record <label> <buffer_name> — register a few-shot audio example from a buffer~.",
        MIN_FUNCTION {
            if (inlet != 1) {
                cerr << "[clap~] record: send to inlet 2" << endl;
                return {};
            }
            if (args.size() < 2) {
                cerr << "[clap~] record: usage: record <label> <buffer_name>" << endl;
                return {};
            }
            if (!m_running) {
                cerr << "[clap~] record: model not loaded yet" << endl;
                return {};
            }

            auto label    = std::string(args[0]);
            auto buf_name = std::string(args[1]);

            m_record_buf.set(buf_name);
            {
                buffer_lock<> lock{m_record_buf};
                if (!lock.valid()) {
                    cerr << "[clap~] record: buffer not found: " << buf_name << endl;
                    return {};
                }

                auto frames   = static_cast<int>(lock.frame_count());
                auto channels = static_cast<int>(lock.channel_count());
                auto buf_sr   = lock.samplerate();

                // Read channel 0 into a float vector
                std::vector<float> raw(static_cast<std::size_t>(frames));
                for (int f = 0; f < frames; ++f)
                    raw[static_cast<std::size_t>(f)] =
                        static_cast<float>(lock[static_cast<size_t>(f * channels)]);

                // Linear resample to 48 kHz if needed
                std::vector<float> samples;
                constexpr int MODEL_SR = 48000;
                if (static_cast<int>(buf_sr) != MODEL_SR && buf_sr > 0) {
                    int target = static_cast<int>(std::round(frames * MODEL_SR / buf_sr));
                    auto wav   = torch::from_blob(raw.data(), {1, 1, frames}, torch::kFloat32).clone();
                    wav = torch::nn::functional::interpolate(
                        wav,
                        torch::nn::functional::InterpolateFuncOptions()
                            .size(std::vector<int64_t>{target})
                            .mode(torch::kLinear)
                            .align_corners(false));
                    auto ptr = wav.data_ptr<float>();
                    samples.assign(ptr, ptr + wav.numel());
                } else {
                    samples = std::move(raw);
                }

                m_classifier->queue_audio_example(label, std::move(samples));
            }

            cout << "[clap~] record: queued example for label \"" << label << "\"" << endl;
            return {};
        }
    };

    // clear_examples  — remove all recorded audio examples (send to either inlet)
    message<> clear_examples{this, "clear_examples",
        "Remove all recorded few-shot audio examples.",
        MIN_FUNCTION {
            if (!m_running) return {};
            m_classifier->clear_audio_examples();
            cout << "[clap~] clear_examples: all audio examples cleared" << endl;
            return {};
        }
    };

    // clear_example <label>  — remove a single label
    message<> clear_example{this, "clear_example",
        "clear_example <label> — remove the few-shot example for a specific label.",
        MIN_FUNCTION {
            if (args.empty()) {
                cerr << "[clap~] clear_example: provide a label" << endl;
                return {};
            }
            if (!m_running) return {};
            m_classifier->clear_audio_examples(std::string(args[0]));
            cout << "[clap~] clear_example: cleared \"" << std::string(args[0]) << "\"" << endl;
            return {};
        }
    };

    // Called after ctor + attributes are set up
    message<> setup{this, "setup", MIN_FUNCTION {
        if (!m_classifier) return {};
        m_classifier->set_energy_threshold(threshold.get());
        m_classifier->set_threshold_window(window.get());
        m_processing_thread = std::thread(&clap_tilde::main_loop, this);
        return {};
    }};

    // Called when DSP is enabled/restarted
    message<> dspsetup{this, "dspsetup", MIN_FUNCTION {
        int sr            = args[0];
        int vector_length = args[1];

        while (!m_model_initialized) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (m_running) {
            m_classifier->initialize_buffers(sr, vector_length);
            cout << "[clap~] buffers ready — sr: " << sr
                 << " vec: " << vector_length << endl;
        } else {
            cerr << "[clap~] dspsetup: model not running (load failed?)" << endl;
        }
        return {};
    }};


    buffer_reference m_record_buf{this, MIN_FUNCTION { return {}; }};

private:
    void main_loop() {
        // Lower OS priority so patch editing / Max UI stays responsive
        setpriority(PRIO_PROCESS, 0, 10);

        try {
            m_classifier->initialize_model();
            m_running = true;
            cout << "[clap~] model loaded — segment length: "
                 << m_classifier->get_segment_length() << " samples" << endl;
        } catch (const std::exception& e) {
            cerr << "[clap~] model load error: " << e.what() << endl;
        } catch (...) {
            cerr << "[clap~] unknown error during model loading" << endl;
        }

        m_model_initialized = true;

        try {
            std::vector<double> buffered;
            buffered.reserve(65536);
            while (m_running) {
                double sample;
                bool got_data = false;
                while (m_audio_fifo.try_dequeue(sample)) {
                    buffered.push_back(sample);
                    got_data = true;
                }
                if (got_data && m_enabled) {
                    auto result = m_classifier->process(std::move(buffered));
                    buffered.clear();
                    buffered.reserve(65536);
                    if (result) {
                        m_event_fifo.try_enqueue(*result);
                        deliverer.delay(0.0);
                    }
                }
                // Always yield — prevents pinning a CPU core between inference calls
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (const std::exception& e) {
            cerr << (verbose.get() ? e.what() : "model architecture is not compatible") << endl;
        } catch (...) {
            cerr << "unknown error in inference loop" << endl;
        }
    }


    struct ModelPaths {
        std::string model_path;
        std::string tokenizer_dir;
        std::string text_onnx_path;
        std::string meta_json_path;
    };

    // Accepts:
    //   - path to clap_tilde_audio_<N>ms.onnx
    //   - path to a directory containing clap_tilde_audio_*.onnx
    static ModelPaths parse_paths(const atoms& args) {
        if (args.empty()) throw std::runtime_error("Missing argument: model path or directory");
        if (args[0].type() != c74::min::message_type::symbol_argument)
            throw std::runtime_error("first argument must be a path");

        auto raw = std::string(args[0]);
        std::string resolved = (!raw.empty() && raw[0] == '/')
            ? raw : static_cast<std::string>(c74::min::path(raw));

        ModelPaths p;
        if (path_is_dir(resolved)) {
            p.model_path    = find_audio_onnx_in_dir(resolved);
            p.tokenizer_dir = resolved;
            if (p.model_path.empty())
                throw std::runtime_error("No clap_tilde_audio_*.onnx found in: " + resolved);
        } else {
            if (path_extension(resolved) != ".onnx")
                throw std::runtime_error("Expected an .onnx file, got: " + resolved);
            p.model_path    = resolved;
            p.tokenizer_dir = path_parent(resolved);
        }

        p.text_onnx_path = path_join(p.tokenizer_dir, "clap_tilde_text.onnx");
        p.meta_json_path = path_join(p.tokenizer_dir, "clap_tilde_meta.json");

        if (!path_exists(p.model_path))
            throw std::runtime_error("Audio ONNX not found: " + p.model_path);
        if (!path_exists(p.text_onnx_path))
            throw std::runtime_error("Text ONNX not found: " + p.text_onnx_path);
        if (!path_exists(p.meta_json_path))
            throw std::runtime_error("Meta JSON not found: " + p.meta_json_path);
        if (!path_exists(path_join(p.tokenizer_dir, "vocab.json")))
            throw std::runtime_error("vocab.json not found in: " + p.tokenizer_dir);
        if (!path_exists(path_join(p.tokenizer_dir, "merges.txt")))
            throw std::runtime_error("merges.txt not found in: " + p.tokenizer_dir);

        return p;
    }

    // Find the first clap_tilde_audio_*.onnx in a directory using POSIX readdir.
    static std::string find_audio_onnx_in_dir(const std::string& dir) {
        DIR* d = opendir(dir.c_str());
        if (!d) return {};
        std::string result;
        struct dirent* entry;
        const std::string prefix = "clap_tilde_audio";
        const std::string suffix = ".onnx";
        while ((entry = readdir(d)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > prefix.size() + suffix.size()
                && name.substr(0, prefix.size()) == prefix
                && name.substr(name.size() - suffix.size()) == suffix)
            {
                result = path_join(dir, name);
                break;
            }
        }
        closedir(d);
        return result;
    }


    static bool parse_use_coreml(const atoms& args) {
        if (args.size() < 2) return false;
        if (args[1].type() == c74::min::message_type::symbol_argument) {
            auto s = std::string(args[1]);
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (s == "MPS") return true;
            if (s == "CPU") return false;
            std::cerr << "[clap~] unknown device \"" << s << "\", defaulting to CPU" << std::endl;
        }
        return false;
    }


};


MIN_EXTERNAL(clap_tilde);
