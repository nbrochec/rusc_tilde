#ifndef CLAP_TILDE_CLAP_MODEL_H
#define CLAP_TILDE_CLAP_MODEL_H

#include <torch/torch.h>
#include <vector>
#include <string>

struct ClassificationResult {
    std::vector<float>       distribution;
    double                   inference_latency_ms;
    std::vector<std::string> class_names;
};

struct IClapModel {
    virtual ~IClapModel() = default;
    virtual at::Tensor encode_text(const std::vector<std::string>& class_names) = 0;
    virtual at::Tensor encode_audio(std::vector<float> audio) = 0;
    virtual ClassificationResult classify(std::vector<float> audio,
                                          const at::Tensor& text_embs) = 0;
    virtual int  get_sample_rate()    const = 0;
    virtual int  get_segment_length() const = 0;   // samples at model sr for current context
    virtual void set_context_ms(int ms) = 0;
};

#endif //CLAP_TILDE_CLAP_MODEL_H
