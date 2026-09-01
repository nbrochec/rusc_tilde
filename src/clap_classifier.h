
#ifndef CLAP_TILDE_CLAP_CLASSIFIER_H
#define CLAP_TILDE_CLAP_CLASSIFIER_H

#include <algorithm>
#include <cmath>
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
            , m_threshold_window_ms(threshold_window_ms)
            , m_energy_threshold(energy_threshold_db) {}


    void initialize_model() {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_model = m_model_factory();
        // Apply a context requested before the model existed (e.g. @context in the object box)
        if (m_requested_context_ms)
            m_model->set_context_ms(*m_requested_context_ms);
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

    // Returns the effective context in ms. Before the model is loaded the request is
    // stored and applied in initialize_model(); the model may clamp it to its exported maximum.
    int set_context_ms(int ms) {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_requested_context_ms = ms;
        if (!m_model) return ms;
        m_model->set_context_ms(ms);
        if (m_classification_buffer && m_input_sr && m_input_vector_length) {
            m_classification_buffer = std::make_unique<ResamplingBuffer>(
                static_cast<std::size_t>(m_model->get_segment_length()),
                *m_input_vector_length,
                *m_input_sr,
                m_model->get_sample_rate());
        }
        return m_model->get_context_ms();
    }

    // Effective context in ms (the requested value if the model is not loaded yet)
    int get_context_ms() {
        std::lock_guard<std::mutex> lock{m_mutex};
        if (m_model) return m_model->get_context_ms();
        return m_requested_context_ms.value_or(0);
    }

    bool is_model_loaded() {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_model != nullptr;
    }


    void set_classes(std::vector<std::string> class_names) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_classes = std::move(class_names);
        m_classes_pending = true;
        m_classes_additive = false;
    }

    void add_classes(std::vector<std::string> class_names) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_classes = std::move(class_names);
        m_classes_pending = true;
        m_classes_additive = true;
    }

    void queue_audio_example(const std::string& label, std::vector<float> audio_samples) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_audio.push_back({label, std::move(audio_samples)});
    }

    void queue_audio_examples_batch(const std::string& label, std::vector<std::vector<float>> batch) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_audio_batches.push_back({label, std::move(batch)});
    }

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

        if (m_combined_dirty) {
            auto [embs, names] = build_combined();
            m_cached_combined_embs  = std::move(embs);
            m_cached_combined_names = std::move(names);
            m_combined_dirty = false;
        }
        if (m_cached_combined_names.empty() || m_cached_combined_embs.empty()) return std::nullopt;

        const auto& combined_embs  = m_cached_combined_embs;
        const auto& combined_names = m_cached_combined_names;

        int num_classes = static_cast<int>(combined_names.size());

        m_threshold_buffer->add_samples(input);
        m_classification_buffer->add_samples(input);

        if (!m_classification_buffer->is_fully_allocated()) return std::nullopt;

        if (m_active) {
            auto samples = m_classification_buffer->get_samples();
            if (m_energy_threshold.is_above_threshold(samples)) {
                auto result = m_model->classify(util::to_floats(samples), combined_embs, num_classes);
                result.class_names = combined_names;
                return result;
            } else {
                m_active = false;
            }
        } else {
            if (m_energy_threshold.is_above_threshold(m_threshold_buffer->samples_unordered())) {
                m_active = true;
                auto samples = m_classification_buffer->get_samples();
                auto result = m_model->classify(util::to_floats(samples), combined_embs, num_classes);
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

    std::vector<std::string> get_class_names() {
        std::lock_guard<std::mutex> lock{m_mutex};
        return m_cached_combined_names;
    }


private:
    bool is_initialized() const {
        return m_model && m_classification_buffer && m_threshold_buffer && m_input_sr.has_value();
    }

    void apply_pending_classes() {
        bool pending = false;
        std::vector<std::string> names;
        bool additive = false;
        {
            std::lock_guard<std::mutex> class_lock{m_class_mutex};
            if (m_classes_pending) {
                pending = true;
                names = m_pending_classes;
                additive = m_classes_additive;
                m_classes_pending = false;
            }
        }
        if (pending && m_model) {
            if (additive) {
                for (const auto& n : names) {
                    if (std::find(m_text_class_names.begin(), m_text_class_names.end(), n)
                            == m_text_class_names.end())
                        m_text_class_names.push_back(n);
                }
                m_text_embeddings = m_model->encode_text(m_text_class_names);
            } else {
                m_text_embeddings = m_model->encode_text(names);
                m_text_class_names = std::move(names);
            }
            m_combined_dirty = true;
        }
    }

    void apply_pending_audio() {
        std::vector<PendingAudio> pending;
        std::vector<PendingAudioBatch> pending_batches;
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
            pending_batches = std::move(m_pending_audio_batches);
            m_pending_audio_batches.clear();
        }

        if (clear_all) {
            if (!m_audio_examples.empty()) {
                m_audio_examples.clear();
                m_combined_dirty = true;
            }
        } else {
            for (const auto& lbl : clear_labels) {
                if (m_audio_examples.erase(lbl) > 0)
                    m_combined_dirty = true;
            }
        }

        if (!m_model) return;

        for (auto& p : pending) {
            auto emb = m_model->encode_audio(std::move(p.audio));  // [512]
            if (!emb.empty()) {
                m_audio_examples[p.label] = std::move(emb);
                m_combined_dirty = true;
            }
        }

        for (auto& batch : pending_batches) {
            constexpr std::size_t EMB_DIM = 512;
            std::vector<float> avg(EMB_DIM, 0.0f);
            int count = 0;
            for (auto& audio : batch.audio_samples) {
                auto emb = m_model->encode_audio(std::move(audio));
                if (emb.size() == EMB_DIM) {
                    for (std::size_t i = 0; i < EMB_DIM; ++i) avg[i] += emb[i];
                    ++count;
                }
            }
            if (count > 0) {
                float inv = 1.0f / static_cast<float>(count);
                for (auto& v : avg) v *= inv;
                float norm = 0.0f;
                for (auto v : avg) norm += v * v;
                norm = std::sqrt(norm);
                if (norm > 1e-8f) for (auto& v : avg) v /= norm;
                m_audio_examples[batch.label] = std::move(avg);
                m_combined_dirty = true;
            }
        }
    }

    // Returns {combined_embs [M*512], names [M]}
    std::pair<std::vector<float>, std::vector<std::string>> build_combined() {
        std::vector<std::string> names;
        std::vector<float> combined;

        // Text classes: use audio embedding if recorded, else text embedding
        for (int i = 0; i < static_cast<int>(m_text_class_names.size()); ++i) {
            const auto& name = m_text_class_names[static_cast<std::size_t>(i)];
            names.push_back(name);
            auto it = m_audio_examples.find(name);
            if (it != m_audio_examples.end()) {
                combined.insert(combined.end(), it->second.begin(), it->second.end());
            } else {
                auto base = static_cast<std::size_t>(i * 512);
                combined.insert(combined.end(),
                    m_text_embeddings.begin() + static_cast<std::ptrdiff_t>(base),
                    m_text_embeddings.begin() + static_cast<std::ptrdiff_t>(base + 512));
            }
        }

        // Audio-only labels not present in text classes
        for (const auto& [label, emb] : m_audio_examples) {
            if (std::find(m_text_class_names.begin(), m_text_class_names.end(), label)
                    == m_text_class_names.end()) {
                names.push_back(label);
                combined.insert(combined.end(), emb.begin(), emb.end());
            }
        }

        return {std::move(combined), std::move(names)};
    }

    std::function<std::unique_ptr<IClapModel>()> m_model_factory;
    int                         m_threshold_window_ms;
    std::optional<int>          m_input_sr;
    std::optional<std::size_t>  m_input_vector_length;
    std::optional<int>          m_requested_context_ms;

    EnergyThreshold       m_energy_threshold;
    bool                  m_initialized = false;
    bool                  m_active = false;

    std::unique_ptr<IClapModel>               m_model;
    std::unique_ptr<ResamplingBuffer>         m_classification_buffer;
    std::unique_ptr<CircularBuffer<double>>   m_threshold_buffer;

    std::vector<float>                        m_text_embeddings;   // [N * 512] row-major
    std::vector<std::string>                  m_text_class_names;
    std::map<std::string, std::vector<float>> m_audio_examples;    // label → [512]
    std::vector<std::string>                  m_cached_combined_names;
    std::vector<float>                        m_cached_combined_embs;
    bool                                      m_combined_dirty = true;

    std::mutex               m_class_mutex;
    std::vector<std::string> m_pending_classes;
    bool                     m_classes_pending  = false;
    bool                     m_classes_additive = false;

    struct PendingAudio      { std::string label; std::vector<float> audio; };
    struct PendingAudioBatch { std::string label; std::vector<std::vector<float>> audio_samples; };
    std::vector<PendingAudio>      m_pending_audio;
    std::vector<PendingAudioBatch> m_pending_audio_batches;
    bool m_clear_all_examples = false;
    std::vector<std::string> m_clear_labels;

    std::mutex m_mutex;
};


#endif //CLAP_TILDE_CLAP_CLASSIFIER_H
