#include "stages/biquad_stage.h"
#include "dsp_core/block_info.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <limits>

/* ------------------------------------------------------------------ */
/*  BiquadStage unit tests                                             */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Helper: generate a sine wave                                       */
/* ------------------------------------------------------------------ */
static void generateSine(float* buf, uint32_t frames, double freq, double sampleRate) {
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i] = static_cast<float>(std::sin(2.0 * M_PI * freq * i / sampleRate));
    }
}

/* ------------------------------------------------------------------ */
/*  Helper: measure RMS                                                */
/* ------------------------------------------------------------------ */
static float measureRms(const float* buf, uint32_t frames) {
    double sumSq = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        sumSq += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return static_cast<float>(std::sqrt(sumSq / frames));
}

/* ------------------------------------------------------------------ */
/*  Helper: check for NaN/Inf                                          */
/* ------------------------------------------------------------------ */
static bool hasNaNOrInf(const float* buf, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        if (std::isnan(buf[i]) || std::isinf(buf[i])) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Helper: compute LPF coefficients (RBJ cookbook, LPF #2)            */
/*  Returns pre-normalized b0,b1,b2,a1,a2 for DFII-T                  */
/* ------------------------------------------------------------------ */
static void lpfCoeffs(float fc, float fs, float Q,
                      float& b0, float& b1, float& b2, float& a1, float& a2) {
    double wo = 2.0 * M_PI * fc / fs;
    double cosWo = std::cos(wo);
    double sinWo = std::sin(wo);
    double alpha = sinWo / (2.0 * Q);

    double b0r = (1.0 - cosWo) / 2.0;
    double b1r = 1.0 - cosWo;
    double b2r = (1.0 - cosWo) / 2.0;
    double a0r = 1.0 + alpha;
    double a1r = -2.0 * cosWo;
    double a2r = 1.0 - alpha;

    b0 = static_cast<float>(b0r / a0r);
    b1 = static_cast<float>(b1r / a0r);
    b2 = static_cast<float>(b2r / a0r);
    a1 = static_cast<float>(a1r / a0r);
    a2 = static_cast<float>(a2r / a0r);
}

/* ------------------------------------------------------------------ */
/*  Helper: compute HPF coefficients (RBJ cookbook, HPF #2)            */
/* ------------------------------------------------------------------ */
static void hpfCoeffs(float fc, float fs, float Q,
                      float& b0, float& b1, float& b2, float& a1, float& a2) {
    double wo = 2.0 * M_PI * fc / fs;
    double cosWo = std::cos(wo);
    double sinWo = std::sin(wo);
    double alpha = sinWo / (2.0 * Q);

    double b0r = (1.0 + cosWo) / 2.0;
    double b1r = -(1.0 + cosWo);
    double b2r = (1.0 + cosWo) / 2.0;
    double a0r = 1.0 + alpha;
    double a1r = -2.0 * cosWo;
    double a2r = 1.0 - alpha;

    b0 = static_cast<float>(b0r / a0r);
    b1 = static_cast<float>(b1r / a0r);
    b2 = static_cast<float>(b2r / a0r);
    a1 = static_cast<float>(a1r / a0r);
    a2 = static_cast<float>(a2r / a0r);
}

/* ------------------------------------------------------------------ */
/*  Test: allpass unity gain                                           */
/* ------------------------------------------------------------------ */
static void testAllpassUnityGain() {
    std::printf("Test: allpass_unity_gain ... ");
    BiquadStage stage;
    stage.setCoeffs(1.0f, 0.f, 0.f, 0.f, 0.f);

    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);
    generateSine(buf.data(), frames, 440.0, 48000.0);

    /* Save a copy of input for comparison */
    std::vector<float> expected = buf;

    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    bool match = true;
    for (uint32_t i = 0; i < frames && match; ++i) {
        if (std::fabs(buf[i] - expected[i]) > 1e-6f) match = false;
    }
    ASSERT(match, "allpass_unity_gain: output == input");
    std::printf("%s\n", match ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ */
/*  Test: lowpass attenuation                                          */
/* ------------------------------------------------------------------ */
static void testLowpassAttenuation() {
    std::printf("Test: lowpass_attenuation ... ");
    BiquadStage stage;
    float b0, b1, b2, a1, a2;
    lpfCoeffs(1000.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
    stage.setCoeffs(b0, b1, b2, a1, a2);

    constexpr uint32_t frames = 4096;
    std::vector<float> bufLow(frames), bufHigh(frames);

    /* Low frequency (100Hz) — should pass */
    generateSine(bufLow.data(), frames, 100.0, 48000.0);
    float* chLow = bufLow.data();
    BlockInfo info{};
    info.frames = frames;
    stage.process(&chLow, 1, frames, info);

    /* High frequency (5kHz) — should be attenuated */
    generateSine(bufHigh.data(), frames, 5000.0, 48000.0);
    float* chHigh = bufHigh.data();
    stage.process(&chHigh, 1, frames, info);

    float rmsLow = measureRms(bufLow.data(), frames);
    float rmsHigh = measureRms(bufHigh.data(), frames);

    /* Sine RMS at full scale is ~0.707 */
    constexpr double kSineRms = 0.7071;

    ASSERT(rmsLow > 0.5f, "lowpass: low freq (100Hz) passes with gain near unity");
    ASSERT(rmsHigh < 0.1f, "lowpass: high freq (5kHz) is attenuated below -16dB");

    std::printf("PASS (rms_low=%.4f rms_high=%.4f)\n",
               static_cast<double>(rmsLow), static_cast<double>(rmsHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: highpass attenuation                                         */
/* ------------------------------------------------------------------ */
static void testHighpassAttenuation() {
    std::printf("Test: highpass_attenuation ... ");
    BiquadStage stage;
    float b0, b1, b2, a1, a2;
    hpfCoeffs(1000.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
    stage.setCoeffs(b0, b1, b2, a1, a2);

    constexpr uint32_t frames = 4096;
    std::vector<float> bufLow(frames), bufHigh(frames);

    /* Low frequency (100Hz) — should be blocked */
    generateSine(bufLow.data(), frames, 100.0, 48000.0);
    float* chLow = bufLow.data();
    BlockInfo info{};
    info.frames = frames;
    stage.process(&chLow, 1, frames, info);

    /* High frequency (5kHz) — should pass */
    generateSine(bufHigh.data(), frames, 5000.0, 48000.0);
    float* chHigh = bufHigh.data();
    stage.process(&chHigh, 1, frames, info);

    float rmsLow = measureRms(bufLow.data(), frames);
    float rmsHigh = measureRms(bufHigh.data(), frames);

    ASSERT(rmsLow < 0.1f, "highpass: low freq (100Hz) is attenuated below -16dB");
    ASSERT(rmsHigh > 0.5f, "highpass: high freq (5kHz) passes with gain near unity");

    std::printf("PASS (rms_low=%.4f rms_high=%.4f)\n",
               static_cast<double>(rmsLow), static_cast<double>(rmsHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: stability check                                              */
/* ------------------------------------------------------------------ */
static void testStabilityCheck() {
    std::printf("Test: stability_check ... ");
    BiquadStage stage;
    float b0, b1, b2, a1, a2;
    lpfCoeffs(5000.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
    stage.setCoeffs(b0, b1, b2, a1, a2);

    constexpr uint32_t frames = 1024;
    std::vector<float> buf(frames);
    BlockInfo info{};
    info.frames = frames;

    /* Run many blocks of noise */
    for (int block = 0; block < 100; ++block) {
        for (uint32_t i = 0; i < frames; ++i) {
            buf[i] = static_cast<float>(std::sin(block + i * 7.3)) * 0.5f;
        }
        float* ch = buf.data();
        stage.process(&ch, 1, frames, info);

        if (hasNaNOrInf(buf.data(), frames)) {
            ASSERT(false, "stability_check: NaN/Inf detected during long run");
            std::printf("FAIL\n");
            return;
        }
    }

    ASSERT(!hasNaNOrInf(buf.data(), frames), "stability_check: no NaN/Inf after 10k frames");
    std::printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test: reset clears state                                           */
/* ------------------------------------------------------------------ */
static void testResetClearsState() {
    std::printf("Test: reset_clears_state ... ");
    BiquadStage stage;
    float b0, b1, b2, a1, a2;
    lpfCoeffs(1000.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
    stage.setCoeffs(b0, b1, b2, a1, a2);

    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);

    /* Pump some signal through to build up state */
    generateSine(buf.data(), frames, 440.0, 48000.0);
    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* Reset and feed silence */
    stage.reset();
    std::memset(buf.data(), 0, frames * sizeof(float));
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    bool allSilent = true;
    for (uint32_t i = 0; i < frames && allSilent; ++i) {
        if (std::fabs(buf[i]) > 1e-10f) allSilent = false;
    }
    ASSERT(allSilent, "reset_clears_state: silence in produces silence out after reset");
    std::printf("%s\n", allSilent ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ */
/*  Test: multi-channel independence                                   */
/* ------------------------------------------------------------------ */
static void testMultiChannelIndependence() {
    std::printf("Test: multi_channel_independence ... ");
    BiquadStage stage;
    float b0, b1, b2, a1, a2;
    lpfCoeffs(1000.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
    stage.setCoeffs(b0, b1, b2, a1, a2);

    constexpr uint32_t frames = 1024;
    std::vector<float> ch0(frames), ch1(frames), ch2(frames);

    /* Channel 0: low freq (should pass) */
    generateSine(ch0.data(), frames, 100.0, 48000.0);
    /* Channel 1: high freq (should be attenuated) */
    generateSine(ch1.data(), frames, 5000.0, 48000.0);
    /* Channel 2: silence */
    std::memset(ch2.data(), 0, frames * sizeof(float));

    float* channels[] = {ch0.data(), ch1.data(), ch2.data()};
    BlockInfo info{};
    info.frames = frames;
    stage.process(channels, 3, frames, info);

    float rms0 = measureRms(ch0.data(), frames);
    float rms1 = measureRms(ch1.data(), frames);
    float rms2 = measureRms(ch2.data(), frames);

    bool ch0Pass = rms0 > 0.5f;
    bool ch1Atten = rms1 < 0.1f;
    bool ch2Silent = rms2 < 1e-10f;

    ASSERT(ch0Pass, "multi_channel: ch0 (low freq) passes");
    ASSERT(ch1Atten, "multi_channel: ch1 (high freq) attenuated");
    ASSERT(ch2Silent, "multi_channel: ch2 (silence) remains silent");

    std::printf("%s (rms=[%.4f %.4f %.6e])\n",
               (ch0Pass && ch1Atten && ch2Silent) ? "PASS" : "FAIL",
               static_cast<double>(rms0), static_cast<double>(rms1),
               static_cast<double>(rms2));
}

/* ------------------------------------------------------------------ */
/*  Test: latency reported                                             */
/* ------------------------------------------------------------------ */
static void testLatencyReported() {
    std::printf("Test: latency_reported ... ");
    BiquadStage stage;
    ASSERT(stage.latencySamples() == 2, "latency_reported: biquad reports 2 samples latency");
    std::printf("%s\n", stage.latencySamples() == 2 ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ */
int main() {
    std::printf("=== BiquadStage Unit Tests ===\n\n");

    testAllpassUnityGain();
    testLowpassAttenuation();
    testHighpassAttenuation();
    testStabilityCheck();
    testResetClearsState();
    testMultiChannelIndependence();
    testLatencyReported();

    if (g_passed) {
        std::printf("\n[PASS] All biquad stage tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
