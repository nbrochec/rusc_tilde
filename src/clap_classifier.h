
#ifndef CLAP_TILDE_CLAP_CLASSIFIER_H
#define CLAP_TILDE_CLAP_CLASSIFIER_H

#include <torch/torch.h>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <string>

#include "clap_model.h"
#include "circular_buffer.h"
#include "energy_threshold.h"
#include "utility.h"


class ClapClassifier {
public:
    static const int DEFAULT_THRESHOLD_WINDOW_MS = 20;

    explicit ClapClassifier(std::function<std::unique_ptr<IClapModel>()> model_factory
                            , double energy_threshold_db = EnergyThreshold::MINIMUM_THRESHOLD
                            , int threshold_window_ms = DEFAULT_THRESHOLD_WINDOW_MS)
            : m_model_factory(std::move(model_factory))
            , m_energy_threshold(energy_threshold_db)
            , m_threshold_window_ms(threshold_window_ms) {}


    void initialize_model() {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_model = m_model_factory();
        m_initialized = is_initialized();
    }


    void initialize_buffers(int sr, int input_vector_length) {
        assert(m_model);
        std::lock_guard<std::mutex> lock{m_mutex};

        m_input_sr            = sr;
        m_input_vector_length = static_cast<std::size_t>(input_vector_length);
        m_threshold_buffer    = std::make_unique<CircularBuffer<double>>(m_threshold_window_ms, sr);
        m_classification_buffer = std::make_unique<ResamplingBuffer>(
            static_cast<std::size_t>(m_model->get_segment_length()),
            *m_input_vector_length,
            sr,
            m_model->get_sample_rate());

        m_initialized = is_initialized();
    }

    void set_context_ms(int ms) {
        std::lock_guard<std::mutex> lock{m_mutex};
        if (m_model) m_model->set_context_ms(ms);
        if (m_classification_buffer && m_input_sr && m_input_vector_length) {
            m_classification_buffer = std::make_unique<ResamplingBuffer>(
                static_cast<std::size_t>(m_model->get_segment_length()),
                *m_input_vector_length,
                *m_input_sr,
                m_model->get_sample_rate());
        }
    }


    void set_classes(std::vector<std::string> class_names) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_classes = std::move(class_names);
        m_classes_pending = true;
    }

    // Queue an audio example to be encoded on the inference thread.
    // audio_samples should be at the model's native sample rate (48 kHz).
    void queue_audio_example(const std::string& label, std::vector<float> audio_samples) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_audio.push_back({label, std::move(audio_samples)});
    }

    // Clear all recorded audio examples (or just one label if specified).
    void clear_audio_examples(const std::string& label = "") {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        if (label.empty()) {
            m_clear_all_examples = true;
        } else {
            m_clear_labels.push_back(label);
        }
    }


    std::optional<ClassificationResult> process(std::vector<double>&& input) {
        std::lock_guard<std::mutex> lock{m_mutex};

        if (!m_initialized) return std::nullopt;

        apply_pending_classes();
        apply_pending_audio();

        // Build combined embedding: text classes + audio examples
        auto [combined_embs, combined_names] = build_combined();
        if (combined_names.empty() || !combined_embs.defined()) return std::nullopt;

        m_threshold_buffer->add_samples(input);
        m_classification_buffer->add_samples(input);

        if (!m_classification_buffer->is_fully_allocated()) return std::nullopt;

        if (m_active) {
            auto samples = m_classification_buffer->get_samples();
            if (m_energy_threshold.is_above_threshold(samples)) {
                auto result = m_model->classify(util::to_floats(samples), combined_embs);
                result.class_names = combined_names;
                return result;
            } else {
                m_active = false;
            }
        } else {
            if (m_energy_threshold.is_above_threshold(m_threshold_buffer->samples_unordered())) {
                m_active = true;
                auto samples = m_classification_buffer->get_samples();
                auto result = m_model->classify(util::to_floats(samples), combined_embs);
                result.class_names = combined_names;
                return result;
            }
        }

        return std::nullopt;
    }


    void set_energy_threshold(double threshold_db) {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_energy_threshold.set_threshold_db(threshold_db);
    }


    void set_threshold_window(int duration_ms) {
        std::lock_guard<std::mutex> lock{m_mutex};
        duration_ms = std::max(0, duration_ms);
        m_threshold_window_ms = duration_ms;
        if (m_initialized && m_input_sr) {
            m_threshold_buffer->resize(duration_ms, *m_input_sr);
        }
    }


    int get_segment_length() {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_model ? m_model->get_segment_length() : 0;
    }

    // Returns the combined class names (text + audio examples) as of last inference cycle.
    std::vector<std::string> get_class_names() {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_cached_combined_names;
    }


private:
    bool is_initialized() const {
        return m_model && m_classification_buffer && m_threshold_buffer && m_input_sr.has_value();
    }

    // Must be called with m_mutex held.
    void apply_pending_classes() {
        bool pending = false;
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> class_lock{m_class_mutex};
            if (m_classes_pending) {
                pending = true;
                names = m_pending_classes;
                m_classes_pending = false;
            }
        }
        if (pending && m_model) {
            m_text_embeddings = m_model->encode_text(names);
            m_text_class_names = std::move(names);
        }
    }

    // Must be called with m_mutex held. Encodes queued audio examples.
    void apply_pending_audio() {
        std::vector<PendingAudio> pending;
        bool clear_all = false;
        std::vector<std::string> clear_labels;

        {
            std::lock_guard<std::mutex> class_lock{m_class_mutex};
            if (m_clear_all_examples) {
                clear_all = true;
                m_clear_all_examples = false;
            }
            clear_labels = std::move(m_clear_labels);
            m_clear_labels.clear();
            pending = std::move(m_pending_audio);
            m_pending_audio.clear();
        }

        if (clear_all) {
            m_audio_examples.clear();
        } else {
            for (const auto& lbl : clear_labels)
                m_audio_examples.erase(lbl);
        }

        if (!m_model) return;
        for (auto& p : pending) {
            auto emb = m_model->encode_audio(std::move(p.audio));
            if (emb.defined() && emb.numel() > 0)
                m_audio_examples[p.label] = emb;   // overwrite — last example wins per label
        }
    }

    // Build combined embeddings. Audio examples replace text embeddings for the
    // same label; audio-only labels (no matching text class) are appended.
    // Works with no text classes if audio examples cover all labels.
    std::pair<at::Tensor, std::vector<std::string>> build_combined() {
        std::vector<std::string> names;
        std::vector<at::Tensor> parts;

        // Text classes: use audio embedding if one was recorded, else text embedding.
        for (int i = 0; i < static_cast<int>(m_text_class_names.size()); ++i) {
            const auto& name = m_text_class_names[i];
            names.push_back(name);
            auto it = m_audio_examples.find(name);
            if (it != m_audio_examples.end())
                parts.push_back(it->second);                          // audio overrides text
            else
                parts.push_back(m_text_embeddings[i].unsqueeze(0));  // text row → [1, 512]
        }

        // Audio-only labels (label not present in text classes).
        for (const auto& [label, emb] : m_audio_examples) {
            if (std::find(m_text_class_names.begin(), m_text_class_names.end(), label)
                    == m_text_class_names.end()) {
                names.push_back(label);
                parts.push_back(emb);
            }
        }

        if (parts.empty()) {
            m_cached_combined_names = {};
            return {{}, {}};
        }

        m_cached_combined_names = names;
        return {torch::cat(parts, 0), std::move(names)};
    }

    std::function<std::unique_ptr<IClapModel>()> m_model_factory;
    int                         m_threshold_window_ms;
    std::optional<int>          m_input_sr;
    std::optional<std::size_t>  m_input_vector_length;

    EnergyThreshold       m_energy_threshold;
    bool                  m_initialized = false;
    bool                  m_active = false;

    std::unique_ptr<IClapModel>               m_model;
    std::unique_ptr<ResamplingBuffer>         m_classification_buffer;
    std::unique_ptr<CircularBuffer<double>>   m_threshold_buffer;

    at::Tensor               m_text_embeddings;
    std::vector<std::string> m_text_class_names;
    std::map<std::string, at::Tensor> m_audio_examples;   // label → [1, 512]
    std::vector<std::string> m_cached_combined_names;

    std::mutex               m_class_mutex;
    std::vector<std::string> m_pending_classes;
    bool                     m_classes_pending = false;

    struct PendingAudio { std::string label; std::vector<float> audio; };
    std::vector<PendingAudio> m_pending_audio;
    bool m_clear_all_examples = false;
    std::vector<std::string> m_clear_labels;

    std::mutex m_mutex;
};


#endif //CLAP_TILDE_CLAP_CLASSIFIER_H
