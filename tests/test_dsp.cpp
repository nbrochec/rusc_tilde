// Minimal self-contained tests for the model-independent parts of rusc~.
// No test framework: each check prints PASS/FAIL and the process exit code
// reports the overall result (used by ctest and CI).

#include "circular_buffer.h"
#include "energy_threshold.h"
#include "leaky_integrator.h"
#include "mel_frontend.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) std::printf("  PASS  %s\n", msg);                            \
        else { std::printf("  FAIL  %s  (%s:%d)\n", msg, __FILE__, __LINE__);  \
               ++g_failures; }                                                 \
    } while (0)

static bool approx(double a, double b, double tol) { return std::fabs(a - b) <= tol; }


// ── CircularBuffer ─────────────────────────────────────────────────────────────
static void test_circular_buffer() {
    std::printf("CircularBuffer\n");
    CircularBuffer<double> buf(4);
    CHECK(!buf.is_fully_allocated(), "starts not fully allocated");

    buf.add_samples(std::vector<double>{1, 2, 3});
    CHECK(!buf.is_fully_allocated(), "3 of 4 samples: still filling");

    buf.add_samples(std::vector<double>{4, 5});
    CHECK(buf.is_fully_allocated(), "wrapped once: fully allocated");

    auto s = buf.get_samples();
    CHECK(s.size() == 4, "get_samples returns capacity");
    CHECK(s[0] == 2 && s[1] == 3 && s[2] == 4 && s[3] == 5,
          "get_samples is chronological (oldest first)");

    buf.resize(8);
    CHECK(buf.size() == 8 && !buf.is_fully_allocated(), "growing resets fully-allocated");
}


// ── EnergyThreshold ────────────────────────────────────────────────────────────
static void test_energy_threshold() {
    std::printf("EnergyThreshold\n");
    std::vector<double> silence(480, 0.0);
    std::vector<double> sine(480);
    for (std::size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5 * std::sin(2.0 * 3.141592653589793 * 1000.0 * static_cast<double>(i) / 48000.0);   // 10 full periods

    EnergyThreshold gate_off(EnergyThreshold::MINIMUM_THRESHOLD);
    CHECK(gate_off.is_above_threshold(silence), "minimum threshold passes silence");

    EnergyThreshold gate(-40.0);
    CHECK(!gate.is_above_threshold(silence), "-40 dB gate blocks silence");
    CHECK(gate.is_above_threshold(sine), "-40 dB gate passes a 0.5-amplitude sine");

    // RMS of 0.5 sine ≈ 0.3536 → −9.03 dB
    CHECK(approx(EnergyThreshold::atodb(EnergyThreshold::rms(sine)), -9.03, 0.05),
          "rms/atodb of a 0.5 sine is about -9 dB");
    CHECK(EnergyThreshold::atodb(0.0) == -120.0, "atodb(0) is -120 dB");
}


// ── LeakyIntegrator ────────────────────────────────────────────────────────────
static void test_leaky_integrator() {
    std::printf("LeakyIntegrator\n");
    LeakyIntegrator li;
    std::vector<float> a{1.f, 0.f}, b{0.f, 1.f};

    li.set_tau(0.0);
    li.process(a);
    auto out = li.process(b);
    CHECK(out == b, "tau = 0 passes input through");

    LeakyIntegrator li2;
    li2.set_tau(1000.0);
    li2.process(a);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    out = li2.process(b);
    CHECK(out[0] > 0.5f && out[0] < 1.f && out[1] > 0.f && out[1] < 0.5f,
          "tau = 1 s after 20 ms stays close to the previous value");
    CHECK(approx(out[0] + out[1], 1.0, 1e-5), "smoothing preserves the total mass");
}


// ── MelFrontend ────────────────────────────────────────────────────────────────
static void test_mel_frontend() {
    std::printf("MelFrontend\n");
    const int n_fft = 1024, hop = 480, n_bins = n_fft / 2 + 1;

    // Identity-ish filterbank: mel j sums bins [8j, 8j+8)
    std::vector<float> filters(static_cast<std::size_t>(n_bins) * MelFrontend::N_MELS, 0.f);
    for (int k = 0; k < n_bins; ++k) {
        int j = std::min(k / 8, MelFrontend::N_MELS - 1);
        filters[static_cast<std::size_t>(k) * MelFrontend::N_MELS + static_cast<std::size_t>(j)] = 1.f;
    }
    MelFrontend fe(n_fft, hop, filters);

    // 1. Power spectrum vs. a naive DFT of the windowed frame
    std::mt19937 rng(7);
    std::normal_distribution<float> nd;
    std::vector<float> frame(static_cast<std::size_t>(n_fft));
    for (auto& x : frame) x = nd(rng);

    auto power = fe.power_spectrum(frame.data());

    double max_rel = 0.0;
    for (int k = 0; k < n_bins; ++k) {
        std::complex<double> acc{0.0, 0.0};
        for (int n = 0; n < n_fft; ++n) {
            double w = 0.5 * (1.0 - std::cos(2.0 * 3.141592653589793 * n / (n_fft - 1)));
            double ang = -2.0 * 3.141592653589793 * k * n / n_fft;
            acc += w * static_cast<double>(frame[static_cast<std::size_t>(n)])
                 * std::complex<double>(std::cos(ang), std::sin(ang));
        }
        double ref = std::norm(acc);
        double rel = std::fabs(ref - power[static_cast<std::size_t>(k)]) / std::max(ref, 1e-6);
        max_rel = std::max(max_rel, rel);
    }
    std::printf("        max relative error vs naive DFT: %.2e\n", max_rel);
    CHECK(max_rel < 1e-3, "power spectrum matches a naive DFT");

    // 2. Sine at bin 100 → energy lands in mel 12 (bins 96..103)
    std::vector<float> sine(48000);
    for (std::size_t i = 0; i < sine.size(); ++i)
        sine[i] = 0.5f * std::sin(2.f * 3.14159265f * 100.f * static_cast<float>(i) / static_cast<float>(n_fft));
    const int64_t nb_frames = 1001;
    auto feats = fe.compute(sine, 48000, nb_frames);
    CHECK(feats.size() == static_cast<std::size_t>(4 * nb_frames * MelFrontend::N_MELS),
          "output has [4, nb_frames, 64] elements");

    const std::size_t frame_sz = static_cast<std::size_t>(nb_frames) * MelFrontend::N_MELS;
    bool tiled = true;
    for (int c = 1; c < 4 && tiled; ++c)
        for (std::size_t i = 0; i < frame_sz; ++i)
            if (feats[i] != feats[static_cast<std::size_t>(c) * frame_sz + i]) { tiled = false; break; }
    CHECK(tiled, "the 4 channels are identical copies");

    // frame 10, mel 12 should dominate its neighbours
    const std::size_t row = 10 * MelFrontend::N_MELS;
    float peak = feats[row + 12], left = feats[row + 10], right = feats[row + 14];
    CHECK(peak > left + 20.f && peak > right + 20.f, "sine energy lands in the expected mel band");

    // 3. Frames beyond the audio stay at the zero padding value
    const int64_t T = (48000 + n_fft - n_fft) / hop + 1;   // 101 frames for 1 s
    CHECK(feats[static_cast<std::size_t>(T) * MelFrontend::N_MELS] == 0.f
          && feats[static_cast<std::size_t>(T - 1) * MelFrontend::N_MELS] != 0.f,
          "exactly 101 frames are filled for 1 s at hop 480");

    // 4. Silence → floor of 10*log10(1e-10) = -100 dB
    std::vector<float> zeros(48000, 0.f);
    auto z = fe.compute(zeros, 48000, nb_frames);
    CHECK(approx(z[0], -100.0, 1e-3), "silence gives the -100 dB floor");
}


// ── ResamplingBuffer ───────────────────────────────────────────────────────────
static void test_resampling_buffer() {
    std::printf("ResamplingBuffer\n");
    const int in_sr = 44100, out_sr = 48000, vec = 64;
    ResamplingBuffer rb(4800, vec, in_sr, out_sr);

    // Feed 1 s of a 1 kHz sine in vector-sized chunks
    std::vector<double> chunk(vec);
    std::size_t n = 0;
    for (int v = 0; v < in_sr / vec + 1; ++v) {
        for (auto& x : chunk)
            x = std::sin(2.0 * 3.141592653589793 * 1000.0 * static_cast<double>(n++) / in_sr);
        rb.add_samples(chunk);
    }
    CHECK(rb.is_fully_allocated(), "buffer fills after 1 s of input");

    auto s = rb.get_samples();
    CHECK(s.size() == 4800, "get_samples returns the buffer size at the output rate");

    // Output should still be a 1 kHz sine at 48 kHz: 48 samples per period
    double rms = 0.0;
    for (auto x : s) rms += x * x;
    rms = std::sqrt(rms / static_cast<double>(s.size()));
    CHECK(approx(rms, 0.7071, 0.02), "resampled sine keeps its RMS");
}


int main() {
    test_circular_buffer();
    test_energy_threshold();
    test_leaky_integrator();
    test_mel_frontend();
    test_resampling_buffer();

    if (g_failures == 0) { std::printf("\nAll tests passed.\n"); return 0; }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
