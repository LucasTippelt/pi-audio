#include "stages/eq_chain_stage.h"
#include "dsp_core/block_info.h"
#include "control/config_loader.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <limits>

static bool g_passed = true;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

#define ASSERT_FLOAT_EQ(a, b, eps, msg)                                          \
    do {                                                                         \
        if (std::fabs((a) - (b)) > (eps)) {                                      \
            std::fprintf(stderr, "[FAIL] %s: got %.8f expected %.8f (line %d)\n", \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

/* Helper: generate a sine wave */
static void generateSine(float* buf, uint32_t frames, double freq, double sampleRate) {
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i] = static_cast<float>(std::sin(2.0 * M_PI * freq * i / sampleRate));
    }
}

/* Helper: measure RMS */
static float measureRms(const float* buf, uint32_t frames) {
    double sumSq = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        sumSq += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return static_cast<float>(std::sqrt(sumSq / frames));
}

/* Helper: check for NaN/Inf */
static bool hasNaNOrInf(const float* buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        if (std::isnan(buf[i]) || std::isinf(buf[i])) return true;
    }
    return false;
}

/* Helper: compute peaking EQ coefficients using ConfigLoader. */
static void peakingCoeffs(float fc, float fs, float Q, float gainDb,
                          float& b0, float& b1, float& b2, float& a1, float& a2) {
    b0 = b1 = b2 = a1 = a2 = 0.f;
    ConfigLoader::precomputeBiquadCoeffs(fc, fs, Q, gainDb, &b0, &b1, &b2, &a1, &a2);
}

/* Test 1: Pass-through when no bands configured */
static void testPassthroughNoBands() {
    std::printf("Test: passthrough_no_bands ... ");
    EqChainStage stage;

    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);
    generateSine(buf.data(), frames, 1000.0, 48000.0);
    std::vector<float> expected = buf;

    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    bool match = true;
    for (uint32_t i = 0; i < frames && match; ++i) {
        if (std::fabs(buf[i] - expected[i]) > 1e-6f) match = false;
    }
    ASSERT(match, "passthrough_no_bands: output == input with zero bands");
    std::printf("%s\n", match ? "PASS" : "FAIL");
}

/* Test 2: Single peaking band produces expected gain at center frequency */
static void testSinglePeakingBand() {
    std::printf("Test: single_peaking_band ... ");
    EqChainStage stage;

    float b0, b1, b2, a1, a2;
    peakingCoeffs(1000.f, 48000.f, 0.7071f, 6.0f, b0, b1, b2, a1, a2);

    float coeffs[5] = {b0, b1, b2, a1, a2};
    bool enabled[1] = {true};
    stage.configure(coeffs, 1, enabled);

    constexpr uint32_t frames = 4096;
    std::vector<float> buf(frames);

    /* Feed sine at center frequency (1kHz) — should get ~+6dB gain */
    generateSine(buf.data(), frames, 1000.0, 48000.0);
    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();

    /* Warmup: process a few blocks to let transient settle */
    for (int w = 0; w < 10; ++w) {
        generateSine(buf.data(), frames, 1000.0, 48000.0);
        ch = buf.data();
        stage.process(&ch, 1, frames, info);
    }

    float rmsOut = measureRms(buf.data(), frames);
    /* Input sine RMS ~0.707, +6dB => ~1.414 */
    ASSERT(rmsOut > 1.2f, "single_peaking_band: gain at center freq is near +6dB");
    ASSERT(!hasNaNOrInf(buf.data(), frames), "single_peaking_band: no NaN/Inf");

    std::printf("PASS (rms=%.4f)\n", static_cast<double>(rmsOut));
}

/* Test 3: Multiple bands cascade correctly */
static void testMultipleBandsCascade() {
    std::printf("Test: multiple_bands_cascade ... ");
    EqChainStage stage;

    constexpr uint8_t numBands = 4;
    float coeffs[numBands * 5];
    bool enabled[numBands];

    /* Band 1: +6dB at 500Hz */
    peakingCoeffs(500.f, 48000.f, 1.0f, 6.0f,
                  coeffs[0], coeffs[1], coeffs[2], coeffs[3], coeffs[4]);
    enabled[0] = true;

    /* Band 2: +6dB at 2000Hz */
    peakingCoeffs(2000.f, 48000.f, 1.0f, 6.0f,
                  coeffs[5], coeffs[6], coeffs[7], coeffs[8], coeffs[9]);
    enabled[1] = true;

    /* Band 3: -6dB at 500Hz (cancels band 1) */
    peakingCoeffs(500.f, 48000.f, 1.0f, -6.0f,
                  coeffs[10], coeffs[11], coeffs[12], coeffs[13], coeffs[14]);
    enabled[2] = true;

    /* Band 4: +6dB at 4000Hz */
    peakingCoeffs(4000.f, 48000.f, 1.0f, 6.0f,
                  coeffs[15], coeffs[16], coeffs[17], coeffs[18], coeffs[19]);
    enabled[3] = true;

    stage.configure(coeffs, numBands, enabled);

    constexpr uint32_t frames = 4096;
    std::vector<float> buf(frames);

    /* Test at 2kHz — should have +6dB from band 2 */
    generateSine(buf.data(), frames, 2000.0, 48000.0);
    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();

    for (int w = 0; w < 10; ++w) {
        generateSine(buf.data(), frames, 2000.0, 48000.0);
        ch = buf.data();
        stage.process(&ch, 1, frames, info);
    }

    float rms2k = measureRms(buf.data(), frames);
    ASSERT(rms2k > 1.0f, "multiple_bands: +6dB at 2kHz");

    /* Test at 500Hz — bands 1 and 3 cancel, should be near unity */
    generateSine(buf.data(), frames, 500.0, 48000.0);
    for (int w = 0; w < 10; ++w) {
        generateSine(buf.data(), frames, 500.0, 48000.0);
        ch = buf.data();
        stage.process(&ch, 1, frames, info);
    }

    float rms500 = measureRms(buf.data(), frames);
    ASSERT(rms500 > 0.3f && rms500 < 1.2f, "multiple_bands: ~0dB at 500Hz (cancel)");
    ASSERT(!hasNaNOrInf(buf.data(), frames), "multiple_bands: no NaN/Inf");

    std::printf("PASS (rms_2k=%.4f rms_500=%.4f)\n",
               static_cast<double>(rms2k), static_cast<double>(rms500));
}

/* Test 4: Disabled bands are skipped */
static void testDisabledBandsSkipped() {
    std::printf("Test: disabled_bands_skipped ... ");
    EqChainStage stage;

    constexpr uint8_t numBands = 2;
    float coeffs[numBands * 5];
    bool enabled[numBands];

    /* Band 1: +12dB at 1kHz (disabled) */
    peakingCoeffs(1000.f, 48000.f, 0.7071f, 12.0f,
                  coeffs[0], coeffs[1], coeffs[2], coeffs[3], coeffs[4]);
    enabled[0] = false;

    /* Band 2: +6dB at 1kHz (enabled) */
    peakingCoeffs(1000.f, 48000.f, 0.7071f, 6.0f,
                  coeffs[5], coeffs[6], coeffs[7], coeffs[8], coeffs[9]);
    enabled[1] = true;

    stage.configure(coeffs, numBands, enabled);

    constexpr uint32_t frames = 4096;
    std::vector<float> buf(frames);

    /* With only band 2 active (+6dB), RMS should be ~1.4 */
    generateSine(buf.data(), frames, 1000.0, 48000.0);
    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();

    for (int w = 0; w < 10; ++w) {
        generateSine(buf.data(), frames, 1000.0, 48000.0);
        ch = buf.data();
        stage.process(&ch, 1, frames, info);
    }

    float rmsOut = measureRms(buf.data(), frames);
    /* If band 1 were active too, we'd get +18dB (~3.5 RMS). With only band 2: ~+6dB (~1.4) */
    ASSERT(rmsOut > 1.0f && rmsOut < 2.0f, "disabled_bands: only enabled band contributes");

    /* Now enable both bands and compare */
    enabled[0] = true;
    stage.configure(coeffs, numBands, enabled);

    for (int w = 0; w < 10; ++w) {
        generateSine(buf.data(), frames, 1000.0, 48000.0);
        ch = buf.data();
        stage.process(&ch, 1, frames, info);
    }

    float rmsBoth = measureRms(buf.data(), frames);
    ASSERT(rmsBoth > rmsOut, "disabled_bands: both bands gives more gain");

    std::printf("PASS (rms_one=%.4f rms_both=%.4f)\n",
               static_cast<double>(rmsOut), static_cast<double>(rmsBoth));
}

/* Test 5: Coefficient update resets delay states cleanly */
static void testCoeffUpdateResetsState() {
    std::printf("Test: coeff_update_resets_state ... ");
    EqChainStage stage;

    float b0, b1, b2, a1, a2;
    peakingCoeffs(1000.f, 48000.f, 0.7071f, 12.0f, b0, b1, b2, a1, a2);

    float coeffs[5] = {b0, b1, b2, a1, a2};
    bool enabled[1] = {true};
    stage.configure(coeffs, 1, enabled);

    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);

    /* Pump signal to build up state */
    generateSine(buf.data(), frames, 1000.0, 48000.0);
    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();
    for (int i = 0; i < 20; ++i) {
        stage.process(&ch, 1, frames, info);
    }

    /* Reconfigure with pass-through coefficients */
    float passCoeffs[5] = {1.f, 0.f, 0.f, 0.f, 0.f};
    stage.configure(passCoeffs, 1, enabled);

    /* Feed silence — should get silence out (no residual from old state) */
    std::memset(buf.data(), 0, frames * sizeof(float));
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    bool allSilent = true;
    for (uint32_t i = 0; i < frames && allSilent; ++i) {
        if (std::fabs(buf[i]) > 1e-10f) allSilent = false;
    }
    ASSERT(allSilent, "coeff_update_resets_state: silence after reconfigure");

    std::printf("%s\n", allSilent ? "PASS" : "FAIL");
}

/* Test 6: Max 12 bands fit without overflow */
static void testMaxBands() {
    std::printf("Test: max_bands ... ");
    EqChainStage stage;

    constexpr uint8_t numBands = EqChainStage::kMaxBands;
    float coeffs[numBands * 5];
    bool enabled[numBands];

    for (uint8_t i = 0; i < numBands; ++i) {
        /* All pass-through bands */
        coeffs[i * 5 + 0] = 1.f;
        coeffs[i * 5 + 1] = 0.f;
        coeffs[i * 5 + 2] = 0.f;
        coeffs[i * 5 + 3] = 0.f;
        coeffs[i * 5 + 4] = 0.f;
        enabled[i] = true;
    }

    stage.configure(coeffs, numBands, enabled);

    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);
    generateSine(buf.data(), frames, 1000.0, 48000.0);
    std::vector<float> expected = buf;

    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    bool match = true;
    for (uint32_t i = 0; i < frames && match; ++i) {
        if (std::fabs(buf[i] - expected[i]) > 1e-4f) match = false;
    }
    ASSERT(match, "max_bands: 12 pass-through bands == identity");
    ASSERT(!hasNaNOrInf(buf.data(), frames), "max_bands: no NaN/Inf with max bands");

    std::printf("%s\n", match ? "PASS" : "FAIL");
}

/* Test 7: Multi-channel processing */
static void testMultiChannel() {
    std::printf("Test: multi_channel ... ");
    EqChainStage stage;

    float b0, b1, b2, a1, a2;
    peakingCoeffs(1000.f, 48000.f, 0.7071f, 6.0f, b0, b1, b2, a1, a2);

    float coeffs[5] = {b0, b1, b2, a1, a2};
    bool enabled[1] = {true};
    stage.configure(coeffs, 1, enabled);

    constexpr uint32_t frames = 4096;
    std::vector<float> ch0(frames), ch1(frames);

    generateSine(ch0.data(), frames, 1000.0, 48000.0);
    generateSine(ch1.data(), frames, 500.0, 48000.0);

    float* channels[] = {ch0.data(), ch1.data()};
    BlockInfo info{};
    info.frames = frames;

    for (int w = 0; w < 10; ++w) {
        generateSine(ch0.data(), frames, 1000.0, 48000.0);
        generateSine(ch1.data(), frames, 500.0, 48000.0);
        stage.process(channels, 2, frames, info);
    }

    float rms0 = measureRms(ch0.data(), frames);
    float rms1 = measureRms(ch1.data(), frames);

    ASSERT(rms0 > 1.0f, "multi_channel: ch0 (at center freq) has gain");
    ASSERT(rms1 > 0.5f && rms1 < 1.0f, "multi_channel: ch1 (off-center) near unity");
    ASSERT(!hasNaNOrInf(ch0.data(), frames), "multi_channel: no NaN/Inf ch0");
    ASSERT(!hasNaNOrInf(ch1.data(), frames), "multi_channel: no NaN/Inf ch1");

    std::printf("PASS (rms=[%.4f %.4f])\n",
               static_cast<double>(rms0), static_cast<double>(rms1));
}

/* Test 8: Latency reported */
static void testLatency() {
    std::printf("Test: latency_reported ... ");
    EqChainStage stage;

    ASSERT(stage.latencySamples() == 0, "latency: zero bands => zero latency");

    float coeffs[10] = {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f};
    bool enabled[2] = {true, true};
    stage.configure(coeffs, 2, enabled);

    ASSERT(stage.latencySamples() == 4, "latency: 2 bands => 4 samples latency");

    std::printf("%s\n", (stage.latencySamples() == 4) ? "PASS" : "FAIL");
}

int main() {
    std::printf("=== EqChainStage Unit Tests ===\n\n");

    testPassthroughNoBands();
    testSinglePeakingBand();
    testMultipleBandsCascade();
    testDisabledBandsSkipped();
    testCoeffUpdateResetsState();
    testMaxBands();
    testMultiChannel();
    testLatency();

    if (g_passed) {
        std::printf("\n[PASS] All EQ chain tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
