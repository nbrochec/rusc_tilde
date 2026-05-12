#ifndef CLAP_TILDE_CLAP_MODEL_H
#define CLAP_TILDE_CLAP_MODEL_H

#include <vector>
#include <string>

struct ClassificationResult {
    std::vector<float>       distribution;
    double                   inference_latency_ms;
    std::vector<std::string> class_names;
};

struct IClapModel {
    virtual ~IClapModel() = default;

    // Returns [N * 512] row-major float32 (N = class_names.size())
    virtual std::vector<float> encode_text(const std::vector<std::string>& class_names) = 0;

    // Returns [512] float32 L2-normalised audio embedding
    virtual std::vector<float> encode_audio(std::vector<float> audio) = 0;

    // text_embs: [num_classes * 512] row-major; num_classes must equal text_embs.size()/512
    virtual ClassificationResult classify(std::vector<float> audio,
                                          const std::vector<float>& text_embs,
                                          int num_classes) = 0;

    virtual int  get_sample_rate()    const = 0;
    virtual int  get_segment_length() const = 0;   // samples at model sr for current context
    virtual void set_context_ms(int ms) = 0;
};

#endif //CLAP_TILDE_CLAP_MODEL_H
