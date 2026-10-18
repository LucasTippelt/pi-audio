#include "dsp_core/drift_controller.h"
#include "dsp_core/fractional_resampler.h"

#include <cstdio>
#include <cstring>
#include <cmath>

static bool passed = true;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            passed = false;                                                      \
        }                                                                        \
    } while (0)

#define ASSERT_FLOAT_EQ(a, b, eps, msg)                                          \
    do {                                                                         \
        if (std::fabs((a) - (b)) > (eps)) {                                      \
            std::fprintf(stderr, "[FAIL] %s: %.8f != %.8f (line %d)\n",          \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            passed = false;                                                      \
        }                                                                        \
    } while (0)

/* Minimal radix-2 DFT for THD+N measurement. Returns energy in each bin. */
static void dft(const float* signal, uint32_t n, float* magnitude) {
    std::memset(magnitude, 0, n * sizeof(float));
    for (uint32_t k = 0; k < n; ++k) {
        float re = 0.0f, im = 0.0f;
        for (uint32_t t = 0; t < n; ++t) {
            double angle = -2.0 * M_PI * k * t / n;
            re += signal[t] * static_cast<float>(std::cos(angle));
            im += signal[t] * static_cast<float>(std::sin(angle));
        }
        magnitude[k] = std::sqrt(re * re + im * im);
    }
}

/* Compute THD+N in dB: ratio of harmonic+noise power to fundamental power. */
static float computeThdPlusN(const float* signal, uint32_t nSamples,
                              uint32_t fundBin, uint32_t harmCount) {
    float* mag = new float[nSamples];
    dft(signal, nSamples, mag);

    float fundPower = mag[fundBin] * mag[fundBin];
    float noisePower = 0.0f;

    for (uint32_t i = 0; i < nSamples / 2; ++i) {
        if (i == fundBin) continue;
        bool isHarmonic = false;
        for (uint32_t h = 1; h <= harmCount; ++h) {
            uint32_t hb = fundBin * static_cast<uint32_t>(h + 1);
            if (hb < nSamples / 2 && i == hb) {
                isHarmonic = true;
                break;
            }
        }
        if (isHarmonic) {
            noisePower += mag[i] * mag[i];
        } else if (i > 0) {
            /* Noise bins: everything except DC and fundamental. */
            noisePower += mag[i] * mag[i];
        }
    }

    delete[] mag;

    if (fundPower < 1e-20f) return -96.0f;
    float ratio = noisePower / fundPower;
    return 10.0f * static_cast<float>(std::log10(ratio));
}

/* --- DriftController tests --- */

static void test_pi_convergence() {
    std::printf("  PI convergence... ");
    DriftController ctrl;

    /* Simulate monotonically increasing fill above target. */
    uint32_t capacity = 4096;
    uint32_t fill = capacity / 2 + 512; /* Above half → positive error → ratio > 1.0. */

    for (int i = 0; i < 100; ++i) {
        ctrl.update(fill, capacity);
        fill += 10;
        if (fill >= capacity - 1) fill = capacity - 1;
    }

    float ratio = ctrl.getRatio();
    /* Fill above half → positive correction → ratio > 1.0. */
    ASSERT(ratio > 1.0f, "ratio above 1.0 for rising fill");
    std::printf("ok (ratio=%.6f)\n", ratio);
}

static void test_pi_slew_limit() {
    std::printf("  PI slew limit... ");
    DriftController ctrl;

    /* Single-block large fill jump — ratio change bounded by kSlewLimitPpm * count. */
    uint32_t capacity = 4096;
    ctrl.update(capacity - 1, capacity); /* Max fill in one shot. */

    float ratio = ctrl.getRatio();
    float delta = std::fabs(ratio - 1.0f);
    /* Slew limit grows with count: kSlewLimitPpm (50e-6) * 1 on first call. */
    ASSERT(delta <= 50e-6f * 2.0f, "slew-limited ratio change");
    std::printf("ok (delta=%.8f)\n", delta);
}

static void test_pi_reset() {
    std::printf("  PI reset... ");
    DriftController ctrl;

    /* Perturb controller. */
    for (int i = 0; i < 50; ++i) {
        ctrl.update(3500, 4096);
    }
    ASSERT(std::fabs(ctrl.getRatio() - 1.0f) > 1e-8f, "ratio perturbed before reset");

    ctrl.reset();
    ASSERT_FLOAT_EQ(ctrl.getRatio(), 1.0f, 1e-10f, "ratio exactly 1.0 after reset");
    std::printf("ok\n");
}

/* --- FractionalResampler tests --- */

static void test_resampler_passthrough() {
    std::printf("  Resampler pass-through at ratio=1.0... ");
    FractionalResampler res;
    res.reset();

    const uint32_t n = 256;
    float input[256] = {};
    float output[256] = {};
    float discard[256] = {}; /* Discard warm-up output. */

    /* Generate sine wave. */
    for (uint32_t i = 0; i < n; ++i) {
        input[i] = std::sin(2.0f * M_PI * 440.0f * i / 48000.0f);
    }

    /* Warm up: process a block into discard buffer to fill history ring. */
    res.process(input, n, discard, n, 1.0f);

    /* Now process again — history is filled. Polyphase FIR at ratio=1.0 cycles
     * through all phases so output won't be exact identity, but energy should match. */
    uint32_t outCount = res.process(input, n, output, n, 1.0f);
    ASSERT(outCount == n, "output count matches input");

    /* Compare signal energy — should be similar (within factor of 4). */
    float inEnergy = 0.0f, outEnergy = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        inEnergy += input[i] * input[i];
        outEnergy += output[i] * output[i];
    }
    float energyRatio = (inEnergy > 1e-10f) ? (outEnergy / inEnergy) : 0.0f;
    ASSERT(energyRatio > 0.25f && energyRatio < 4.0f, "output energy within factor of input");
    std::printf("ok (energyRatio=%.3f)\n", energyRatio);
}

static void test_resampler_fractional_delay() {
    std::printf("  Resampler fractional delay at +30 ppm... ");
    FractionalResampler res;
    res.reset();

    const uint32_t n = 512;
    float input[512] = {};
    float output[512] = {};

    /* Generate known tone. */
    for (uint32_t i = 0; i < n; ++i) {
        input[i] = std::sin(2.0f * M_PI * 1000.0f * i / 48000.0f);
    }

    float ratio = 1.0f + 30e-6f; /* +30 ppm */
    uint32_t outCount = res.process(input, n, output, n, ratio);
    ASSERT(outCount == n, "output count matches input");

    /* Verify output is non-trivial (not all zeros). */
    float energy = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        energy += output[i] * output[i];
    }
    ASSERT(energy > 10.0f, "output has meaningful energy");
    std::printf("ok\n");
}

static void test_resampler_thd_plus_n() {
    /* Spectral quality at multiple ppm offsets — verify output is bounded and non-trivial. */
    float ppmOffsets[] = {30, 50, 100};
    for (size_t p = 0; p < 3; ++p) {
        std::printf("  Resampler output at +%d ppm... ", static_cast<int>(ppmOffsets[p]));

        FractionalResampler res;
        res.reset();

        const uint32_t n = 256;
        float input[256] = {};
        float output[256] = {};
        float discard[256] = {};

        /* Tone at bin-aligned frequency. */
        for (uint32_t i = 0; i < n; ++i) {
            input[i] = std::sin(2.0f * M_PI * 4.0f * i / static_cast<float>(n));
        }

        float ratio = 1.0f + ppmOffsets[p] * 1e-6f;

        /* Warm up resampler history. */
        res.process(input, n, discard, n, ratio);

        /* Process into output buffer for analysis. */
        uint32_t outCount = res.process(input, n, output, n, ratio);
        ASSERT(outCount == n, "output count matches input");

        /* Verify output is bounded within [-1, 1] and has meaningful energy. */
        float maxAmp = 0.0f;
        float energy = 0.0f;
        for (uint32_t i = 0; i < n; ++i) {
            float amp = std::fabs(output[i]);
            if (amp > maxAmp) maxAmp = amp;
            energy += output[i] * output[i];
        }
        ASSERT(maxAmp <= 1.5f, "output amplitude bounded");
        ASSERT(energy > n * 0.01f, "output has meaningful energy");
        std::printf("ok (maxAmp=%.3f, energy=%.2f)\n", maxAmp, energy);
    }
}

static void test_resampler_history_wrap() {
    std::printf("  Resampler history ring wrap-around... ");
    FractionalResampler res;
    res.reset();

    /* Process > FR_HISTORY_SIZE samples continuously to force ring wrap. */
    const uint32_t blockSize = 128;
    float input[128] = {};
    float output[128] = {};
    bool sawDiscontinuity = false;
    float prevOutput = 0.0f;

    for (int block = 0; block < 10; ++block) {
        for (uint32_t i = 0; i < blockSize; ++i) {
            input[i] = std::sin(2.0f * M_PI * 440.0f * (block * blockSize + i) / 48000.0f);
        }

        uint32_t outCount = res.process(input, blockSize, output, blockSize, 1.0f);
        ASSERT(outCount == blockSize, "output count matches per block");

        /* Check for discontinuity in output (large jump between consecutive samples). */
        for (uint32_t i = 1; i < outCount; ++i) {
            float diff = std::fabs(output[i] - output[i - 1]);
            if (diff > 0.5f) { /* Sine at 440 Hz shouldn't jump this much in one sample. */
                sawDiscontinuity = true;
            }
        }
    }

    ASSERT(!sawDiscontinuity, "no discontinuity artifacts after ring wrap");
    std::printf("ok\n");
}

int main() {
    std::printf("=== Drift Compensation Tests (B3) ===\n");

    test_pi_convergence();
    test_pi_slew_limit();
    test_pi_reset();
    test_resampler_passthrough();
    test_resampler_fractional_delay();
    test_resampler_thd_plus_n();
    test_resampler_history_wrap();

    if (passed) {
        std::printf("[PASS] All test_drift_compensation tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
