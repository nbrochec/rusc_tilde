// Minimal self-contained tests for the model-independent parts of rusc~.
// No test framework: each check prints PASS/FAIL and the process exit code
// reports the overall result (used by ctest and CI).

#include "circular_buffer.h"
#include "energy_threshold.h"
#include "leaky_integrator.h"
#include "mel_frontend.h"
#include "spsc_ring.h"
#include "clap_classifier.h"

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <thread>
#include <algorithm>
#include <memory>
#include <string>
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


// ── SpscRing ───────────────────────────────────────────────────────────────────
static void test_spsc_ring() {
    std::printf("SpscRing\n");
    SpscRing<double> ring(100);
    CHECK(ring.capacity() == 128, "capacity rounds up to a power of two");

    std::vector<double> block(64);
    for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<double>(i);
    CHECK(ring.write(block.data(), 64) == 64, "first block fits");
    CHECK(ring.write(block.data(), 64) == 64, "second block fits (ring full)");
    CHECK(ring.write(block.data(), 64) == 0, "third block is dropped, never blocks");
    CHECK(ring.available() == 128, "128 samples waiting");

    std::vector<double> out;
    CHECK(ring.read_all(out) == 128 && out.size() == 128, "read_all drains everything");
    CHECK(out[0] == 0 && out[63] == 63 && out[64] == 0 && out[127] == 63, "order preserved");
    CHECK(ring.read_all(out) == 0 && out.size() == 128, "empty ring appends nothing");

    // Wrap-around: write 100, read, write 64 (crosses the end), read
    out.clear();
    std::vector<double> hundred(100, 7.0);
    ring.write(hundred.data(), 100); ring.read_all(out);
    out.clear();
    ring.write(block.data(), 64); ring.read_all(out);
    bool ok = out.size() == 64;
    for (std::size_t i = 0; ok && i < 64; ++i) ok = out[i] == static_cast<double>(i);
    CHECK(ok, "wrap-around keeps sample order");

    // Producer / consumer threads: every sample arrives exactly once, in order
    SpscRing<double> ring2(4096);
    const std::size_t total = 200000;
    std::vector<double> received; received.reserve(total);
    std::thread consumer([&] {
        while (received.size() < total) {
            if (ring2.read_all(received) == 0) std::this_thread::yield();
        }
    });
    std::size_t sent = 0;
    while (sent < total) {
        double chunk[64];
        std::size_t n = std::min<std::size_t>(64, total - sent);
        for (std::size_t i = 0; i < n; ++i) chunk[i] = static_cast<double>(sent + i);
        sent += ring2.write(chunk, n);   // retries the remainder when full
    }
    consumer.join();
    ok = received.size() == total;
    for (std::size_t i = 0; ok && i < total; ++i) ok = received[i] == static_cast<double>(i);
    CHECK(ok, "two threads: 200k samples delivered once, in order");
}


// ── ClapClassifier (encoder thread, class bookkeeping) ─────────────────────────
struct MockModel : IClapModel {
    std::vector<float> encode_text(const std::vector<std::string>& n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));   // "slow" text encoder
        return std::vector<float>(n.size() * 512, 0.1f);
    }
    std::vector<float> encode_audio(const std::vector<float>&) override { return std::vector<float>(512, 0.2f); }
    ClassificationResult classify(const std::vector<float>&, const std::vector<float>&, int n) override {
        return ClassificationResult{std::vector<float>(static_cast<std::size_t>(n), 1.f / static_cast<float>(n)), 0.0, {}};
    }
    int get_sample_rate() const override { return 48000; }
    int get_segment_length() const override { return 4800; }
    int get_context_ms() const override { return 100; }
    int get_max_context_ms() const override { return 100; }
    void set_context_ms(int) override {}
};

// Feed audio for `max_ms` and return the class names of the last result.
static std::vector<std::string> pump(ClapClassifier& c, int max_ms) {
    std::vector<std::string> names;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() < max_ms) {
        std::vector<double> v(64, 0.5);
        auto r = c.process(std::move(v));
        if (r) names = r->class_names;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return names;
}

static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) { if (!s.empty()) s += ","; s += x; }
    return s.empty() ? "(none)" : s;
}
#define CHECK_NAMES(got, expected, msg)                                        \
    do {                                                                       \
        std::vector<std::string> e_ = expected;                                \
        if (got != e_) std::printf("        got: %s\n", join(got).c_str());    \
        CHECK(got == e_, msg);                                                 \
    } while (0)

static void test_classifier() {
    std::printf("ClapClassifier\n");
    ClapClassifier c([] { return std::make_unique<MockModel>(); }, -80.0, 20);
    c.initialize_model();
    c.initialize_buffers(48000, 64);

    for (int i = 0; i < 100; ++i) { std::vector<double> v(64, 0.5); c.process(std::move(v)); }
    CHECK(c.get_class_names().empty(), "no classes yet: nothing to output");

    c.set_classes({"kick", "snare"});
    auto names = pump(c, 400);   // buffer fill (75 vectors) + 30 ms mock encoding
    CHECK_NAMES(names, std::vector<std::string>({"kick", "snare"}), "set_classes installs the encoded list");

    c.add_classes({"hihat", "kick"});
    names = pump(c, 150);
    CHECK_NAMES(names, std::vector<std::string>({"kick", "snare", "hihat"}), "add_class appends without duplicates");

    // back-to-back requests compose even while the first is still encoding
    c.set_classes({"a"});
    c.add_classes({"b"});
    names = pump(c, 200);
    CHECK_NAMES(names, std::vector<std::string>({"a", "b"}), "set_classes followed by add_class before encoding finished");

    // few-shot: a record then an immediate clear must not resurrect the label
    c.queue_audio_example("voice", std::vector<float>(4800, 0.1f));
    names = pump(c, 150);
    CHECK_NAMES(names, std::vector<std::string>({"a", "b", "voice"}), "record adds an audio-only label");

    c.queue_audio_example("late", std::vector<float>(4800, 0.1f));
    c.clear_audio_examples("late");
    names = pump(c, 150);
    CHECK(std::find(names.begin(), names.end(), "late") == names.end(),
          "clear_example right after record drops the pending result");

    c.clear_audio_examples();
    names = pump(c, 150);
    CHECK_NAMES(names, std::vector<std::string>({"a", "b"}), "clear_examples removes audio-only labels");
}


int main() {
    test_circular_buffer();
    test_spsc_ring();
    test_classifier();
    test_energy_threshold();
    test_leaky_integrator();
    test_mel_frontend();
    test_resampling_buffer();

    if (g_failures == 0) { std::printf("\nAll tests passed.\n"); return 0; }
    std::printf("\n%d check(s) failed.\n", g_failures);
    return 1;
}
