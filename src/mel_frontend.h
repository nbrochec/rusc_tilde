#ifndef RUSC_TILDE_MEL_FRONTEND_H
#define RUSC_TILDE_MEL_FRONTEND_H

// Log-mel front-end matching the Python CLAP feature extractor:
//   center reflect-pad → STFT (Hann, hop) → power → mel filterbank → 10*log10 → tile 4×
//
// The FFT is pocketfft (header-only, BSD-3), which keeps the front-end
// dependency-free and identical on every platform.

#include "pocketfft_hdronly.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>


class MelFrontend {
public:
    static constexpr int N_MELS = 64;

    // mel_filters: [(n_fft/2 + 1) * N_MELS] row-major
    MelFrontend(int n_fft, int hop_length, std::vector<float> mel_filters)
        : m_n_fft(n_fft)
        , m_hop_length(hop_length)
        , m_n_bins(n_fft / 2 + 1)
        , m_mel_filters(std::move(mel_filters))
        , m_plan(static_cast<std::size_t>(n_fft))
    {
        if (n_fft <= 0 || hop_length <= 0)
            throw std::invalid_argument("MelFrontend: n_fft and hop_length must be positive");
        if (m_mel_filters.size() != static_cast<std::size_t>(m_n_bins) * N_MELS)
            throw std::invalid_argument("MelFrontend: mel filterbank has the wrong size");

        // Symmetric Hann window (matches the original implementation)
        constexpr float TWO_PI = 6.283185307179586f;
        m_hann_window.resize(static_cast<std::size_t>(n_fft));
        for (int i = 0; i < n_fft; ++i)
            m_hann_window[static_cast<std::size_t>(i)] =
                0.5f * (1.f - std::cos(TWO_PI * static_cast<float>(i) / static_cast<float>(n_fft - 1)));

        m_frame.assign(static_cast<std::size_t>(n_fft), 0.f);
        m_power.assign(static_cast<std::size_t>(m_n_bins), 0.f);
    }

    int n_fft()      const { return m_n_fft; }
    int hop_length() const { return m_hop_length; }

    // Returns [4 * nb_frames * N_MELS] flattened, i.e. the [1, 4, nb_frames, 64] tensor.
    // Audio is trimmed / zero-padded to `segment_length`; mel rows beyond the
    // computed frames stay zero.
    std::vector<float> compute(const std::vector<float>& audio,
                               std::size_t segment_length,
                               int64_t nb_frames) {
        if (segment_length < 2)
            throw std::invalid_argument("MelFrontend: segment_length too small");

        // 1. Trim / zero-pad to the context window
        std::vector<float> wav(audio.begin(), audio.end());
        wav.resize(segment_length, 0.f);

        // 2. Reflect padding (matches torch.stft center=True)
        const std::size_t pad = static_cast<std::size_t>(m_n_fft / 2);
        const std::size_t N   = wav.size();
        std::vector<float> padded(pad + N + pad);
        for (std::size_t i = 0; i < pad; ++i)
            padded[pad - 1 - i] = wav[std::min(i + 1, N - 1)];
        std::copy(wav.begin(), wav.end(), padded.begin() + static_cast<std::ptrdiff_t>(pad));
        for (std::size_t i = 0; i < pad; ++i)
            padded[pad + N + i] = wav[N - 2 - std::min(i, N - 2)];

        // 3. STFT frame-by-frame → power → mel → dB
        int64_t T_actual = (static_cast<int64_t>(padded.size()) - m_n_fft) / m_hop_length + 1;
        int64_t T        = std::min(T_actual, nb_frames);

        const std::size_t frame_sz = static_cast<std::size_t>(nb_frames) * N_MELS;
        std::vector<float> log_mel(frame_sz, 0.f);

        for (int64_t t = 0; t < T; ++t) {
            std::size_t offset = static_cast<std::size_t>(t * m_hop_length);
            power_spectrum(padded.data() + offset);

            std::size_t row = static_cast<std::size_t>(t) * N_MELS;
            for (int j = 0; j < N_MELS; ++j) {
                float mel_val = 0.f;
                for (int k = 0; k < m_n_bins; ++k)
                    mel_val += m_mel_filters[static_cast<std::size_t>(k) * N_MELS + static_cast<std::size_t>(j)]
                             * m_power[static_cast<std::size_t>(k)];
                log_mel[row + static_cast<std::size_t>(j)] = 10.f * std::log10(std::max(mel_val, 1e-10f));
            }
        }

        // 4. Tile 4× → [1, 4, nb_frames, 64] flattened
        std::vector<float> output(4 * frame_sz);
        for (int c = 0; c < 4; ++c)
            std::copy(log_mel.begin(), log_mel.end(),
                      output.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(c) * frame_sz));
        return output;
    }

    // Windowed power spectrum of one n_fft-sample frame → [n_fft/2 + 1].
    // Exposed for testing.
    const std::vector<float>& power_spectrum(const float* samples) {
        const std::size_t n = static_cast<std::size_t>(m_n_fft);
        for (std::size_t i = 0; i < n; ++i)
            m_frame[i] = samples[i] * m_hann_window[i];

        // In-place real FFT, halfcomplex layout: r0, r1, i1, r2, i2, ..., r(n/2)  (n even)
        m_plan.exec(m_frame.data(), 1.f, true);

        m_power[0] = m_frame[0] * m_frame[0];
        for (std::size_t k = 1; k < n / 2; ++k) {
            float re = m_frame[2 * k - 1];
            float im = m_frame[2 * k];
            m_power[k] = re * re + im * im;
        }
        if (n % 2 == 0)
            m_power[n / 2] = m_frame[n - 1] * m_frame[n - 1];
        else
            m_power[n / 2] = m_frame[n - 2] * m_frame[n - 2] + m_frame[n - 1] * m_frame[n - 1];
        return m_power;
    }

private:
    int m_n_fft;
    int m_hop_length;
    int m_n_bins;
    std::vector<float> m_mel_filters;
    std::vector<float> m_hann_window;

    pocketfft::detail::pocketfft_r<float> m_plan;
    std::vector<float> m_frame;   // [n_fft], reused per frame
    std::vector<float> m_power;   // [n_fft/2 + 1]
};


#endif // RUSC_TILDE_MEL_FRONTEND_H
