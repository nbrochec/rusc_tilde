
#ifndef RUSC_TILDE_CLAP_CLASSIFIER_H
#define RUSC_TILDE_CLAP_CLASSIFIER_H

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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


    ~ClapClassifier() {
        stop_encoder_thread();
    }

    void initialize_model() {
        std::lock_guard<std::mutex> lock{m_mutex};
        m_model = m_model_factory();
        // Apply a context requested before the model existed (e.g. @context in the object box)
        if (m_requested_context_ms)
            m_model->set_context_ms(*m_requested_context_ms);
        m_initialized = is_initialized();
        start_encoder_thread();
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


    // Class requests are queued in order, so `set_classes a` immediately followed
    // by `add_class b` yields {a, b} instead of losing the first message.
    void set_classes(std::vector<std::string> class_names) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_class_requests.push_back({std::move(class_names), false});
    }

    void add_classes(std::vector<std::string> class_names) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_class_requests.push_back({std::move(class_names), true});
    }

    // Epochs are captured at queue time: a clear issued after this call (even
    // before the example has been encoded) invalidates the result.
    void queue_audio_example(const std::string& label, std::vector<float> audio_samples) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_audio.push_back({label, std::move(audio_samples), m_audio_epoch, m_label_epoch[label]});
    }

    void queue_audio_examples_batch(const std::string& label, std::vector<std::vector<float>> batch) {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        m_pending_audio_batches.push_back({label, std::move(batch), m_audio_epoch, m_label_epoch[label]});
    }

    void clear_audio_examples(const std::string& label = "") {
        std::lock_guard<std::mutex> lock{m_class_mutex};
        if (label.empty()) {
            m_clear_all_examples = true;
            ++m_audio_epoch;
        } else {
            m_clear_labels.push_back(label);
            ++m_label_epoch[label];
        }
    }


    std::optional<ClassificationResult> process(std::vector<double>&& input) {
        std::lock_guard<std::mutex> lock{m_mutex};

        if (!m_initialized) return std::nullopt;

        apply_pending_classes();
        apply_pending_audio();
        apply_encoder_results();

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

        // The energy gate looks at the short `window` buffer (input rate) both to
        // become active and to stay active, so entering and leaving are symmetric
        // and no 1 s copy is needed for the check.
        const bool above = m_energy_threshold.is_above_threshold(m_threshold_buffer->samples_unordered());
        m_active = above;
        if (!above) return std::nullopt;

        // Chronological float copy of the context window into a reused buffer
        m_classification_buffer->copy_ordered(m_audio_scratch);
        auto result = m_model->classify(m_audio_scratch, combined_embs, num_classes);
        result.class_names = combined_names;
        return result;
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

    // ── Encoder worker ──────────────────────────────────────────────────────
    // Text and few-shot audio encodings run on their own thread so that the
    // inference thread keeps classifying with the previous prototypes instead
    // of stalling for the duration of the text encoder.

    enum class JobType { Text, Audio, AudioBatch };
    struct EncodeJob {
        JobType                          type;
        std::vector<std::string>         names;         // Text: full class list
        std::string                      label;         // Audio / AudioBatch
        std::vector<float>               audio;         // Audio
        std::vector<std::vector<float>>  batch;         // AudioBatch
        std::uint64_t                    audio_epoch = 0;
        std::uint64_t                    label_epoch = 0;
    };
    struct EncodeResult {
        JobType                  type;
        std::vector<std::string> names;
        std::string              label;
        std::vector<float>       embedding;   // Text: [N*512], Audio: [512]
        std::uint64_t            audio_epoch = 0;
        std::uint64_t            label_epoch = 0;
    };

    void start_encoder_thread() {
        if (m_encoder_thread.joinable()) return;
        m_encoder_stop = false;
        m_encoder_thread = std::thread([this] { encoder_loop(); });
    }

    void stop_encoder_thread() {
        {
            std::lock_guard<std::mutex> lock{m_job_mutex};
            m_encoder_stop = true;
        }
        m_job_cv.notify_all();
        if (m_encoder_thread.joinable()) m_encoder_thread.join();
    }

    void post_job(EncodeJob job) {
        {
            std::lock_guard<std::mutex> lock{m_job_mutex};
            m_jobs.push_back(std::move(job));
        }
        m_job_cv.notify_one();
    }

    void encoder_loop() {
        for (;;) {
            EncodeJob job;
            {
                std::unique_lock<std::mutex> lock{m_job_mutex};
                m_job_cv.wait(lock, [this] { return m_encoder_stop || !m_jobs.empty(); });
                if (m_encoder_stop) return;
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }

            EncodeResult res;
            res.type        = job.type;
            res.label       = job.label;
            res.audio_epoch = job.audio_epoch;
            res.label_epoch = job.label_epoch;
            try {
                switch (job.type) {
                    case JobType::Text:
                        res.names     = job.names;
                        res.embedding = m_model->encode_text(job.names);
                        break;
                    case JobType::Audio:
                        res.embedding = m_model->encode_audio(job.audio);
                        break;
                    case JobType::AudioBatch:
                        res.embedding = average_embeddings(job.batch);
                        break;
                }
            } catch (...) {
                continue;   // a failed encoding leaves the current prototypes untouched
            }

            std::lock_guard<std::mutex> lock{m_class_mutex};
            m_results.push_back(std::move(res));
        }
    }

    // Mean of the L2-normalised embeddings of several takes, re-normalised.
    std::vector<float> average_embeddings(const std::vector<std::vector<float>>& batch) {
        constexpr std::size_t EMB_DIM = 512;
        std::vector<float> avg(EMB_DIM, 0.0f);
        int count = 0;
        for (const auto& audio : batch) {
            auto emb = m_model->encode_audio(audio);
            if (emb.size() == EMB_DIM) {
                for (std::size_t i = 0; i < EMB_DIM; ++i) avg[i] += emb[i];
                ++count;
            }
        }
        if (count == 0) return {};
        float inv = 1.0f / static_cast<float>(count);
        for (auto& v : avg) v *= inv;
        float norm = 0.0f;
        for (auto v : avg) norm += v * v;
        norm = std::sqrt(norm);
        if (norm > 1e-8f) for (auto& v : avg) v /= norm;
        return avg;
    }

    // Called from process() under m_mutex: turn pending class requests into jobs.
    void apply_pending_classes() {
        std::deque<ClassRequest> requests;
        {
            std::lock_guard<std::mutex> class_lock{m_class_mutex};
            requests.swap(m_class_requests);
        }
        if (requests.empty() || !m_model) return;

        // Fold all queued requests into one list, merging against the last *requested*
        // list so they compose even while a previous encoding is still running.
        // Only the final list is sent to the encoder.
        std::vector<std::string> full = m_text_names_requested;
        for (auto& req : requests) {
            if (!req.additive) full.clear();
            for (auto& n : req.names)
                if (std::find(full.begin(), full.end(), n) == full.end())
                    full.push_back(std::move(n));
        }
        m_text_names_requested = full;

        EncodeJob job;
        job.type  = JobType::Text;
        job.names = std::move(full);
        post_job(std::move(job));
    }

    // Called from process() under m_mutex: clears apply immediately, encodings become jobs.
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
            EncodeJob job;
            job.type        = JobType::Audio;
            job.label       = p.label;
            job.audio       = std::move(p.audio);
            job.audio_epoch = p.audio_epoch;
            job.label_epoch = p.label_epoch;
            post_job(std::move(job));
        }
        for (auto& b : pending_batches) {
            EncodeJob job;
            job.type        = JobType::AudioBatch;
            job.label       = b.label;
            job.batch       = std::move(b.audio_samples);
            job.audio_epoch = b.audio_epoch;
            job.label_epoch = b.label_epoch;
            post_job(std::move(job));
        }
    }

    // Called from process() under m_mutex: install finished encodings.
    void apply_encoder_results() {
        std::deque<EncodeResult> results;
        std::uint64_t audio_epoch = 0;
        std::map<std::string, std::uint64_t> label_epochs;
        {
            std::lock_guard<std::mutex> class_lock{m_class_mutex};
            if (m_results.empty()) return;
            results.swap(m_results);
            audio_epoch  = m_audio_epoch;
            label_epochs = m_label_epoch;
        }

        for (auto& r : results) {
            if (r.type == JobType::Text) {
                m_text_embeddings  = std::move(r.embedding);
                m_text_class_names = std::move(r.names);
                m_combined_dirty   = true;
                continue;
            }
            if (r.embedding.empty()) continue;
            // Drop the result if the label (or everything) was cleared after it was queued
            auto it = label_epochs.find(r.label);
            std::uint64_t current_label_epoch = it == label_epochs.end() ? 0 : it->second;
            if (r.audio_epoch != audio_epoch || r.label_epoch != current_label_epoch) continue;
            m_audio_examples[r.label] = std::move(r.embedding);
            m_combined_dirty = true;
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
    std::vector<float>                        m_audio_scratch;     // context window as float

    std::vector<float>                        m_text_embeddings;   // [N * 512] row-major
    std::vector<std::string>                  m_text_class_names;
    std::map<std::string, std::vector<float>> m_audio_examples;    // label → [512]
    std::vector<std::string>                  m_cached_combined_names;
    std::vector<float>                        m_cached_combined_embs;
    bool                                      m_combined_dirty = true;

    std::vector<std::string>                  m_text_names_requested;   // last list sent to the encoder

    struct ClassRequest { std::vector<std::string> names; bool additive; };

    std::mutex               m_class_mutex;
    std::deque<ClassRequest> m_class_requests;
    std::deque<EncodeResult> m_results;                    // filled by the encoder thread
    std::uint64_t                        m_audio_epoch = 0; // bumped by clear_examples
    std::map<std::string, std::uint64_t> m_label_epoch;     // bumped by clear_example <label>

    struct PendingAudio      { std::string label; std::vector<float> audio;
                               std::uint64_t audio_epoch; std::uint64_t label_epoch; };
    struct PendingAudioBatch { std::string label; std::vector<std::vector<float>> audio_samples;
                               std::uint64_t audio_epoch; std::uint64_t label_epoch; };
    std::vector<PendingAudio>      m_pending_audio;
    std::vector<PendingAudioBatch> m_pending_audio_batches;
    bool m_clear_all_examples = false;
    std::vector<std::string> m_clear_labels;

    std::thread             m_encoder_thread;
    std::mutex              m_job_mutex;
    std::condition_variable m_job_cv;
    std::deque<EncodeJob>   m_jobs;
    bool                    m_encoder_stop = false;

    std::mutex m_mutex;
};


#endif //RUSC_TILDE_CLAP_CLASSIFIER_H
