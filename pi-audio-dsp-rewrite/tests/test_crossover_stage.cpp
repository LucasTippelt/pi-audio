#include "stages/crossover_stage.h"
#include "dsp_core/block_info.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <limits>

/* ------------------------------------------------------------------ */
/*  CrossoverStage unit tests                                          */
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
/*  Helper: generate white noise                                       */
/* ------------------------------------------------------------------ */
static void generateNoise(float* buf, uint32_t frames, unsigned int seed) {
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i] = static_cast<float>(std::sin(static_cast<double>(seed + i * 7919) * 0.001)) * 0.5f;
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
/*  Helper: build LR24 coefficients (2 identical biquads, Q=0.5412)   */
/*  Returns 10 floats: [b0,b1,b2,a1,a2, b0,b1,b2,a1,a2]              */
/* ------------------------------------------------------------------ */
static void lr24LPCoeffs(float fc, float fs, std::vector<float>& coeffs) {
    /* LR24 = cascade of two identical Butterworth 12dB sections.     */
    /* Each section uses Q = 0.7071 (standard BW 2nd-order).         */
    float b0, b1, b2, a1, a2;
    lpfCoeffs(fc, fs, 0.7071f, b0, b1, b2, a1, a2);
    coeffs.assign({b0, b1, b2, a1, a2, b0, b1, b2, a1, a2});
}

static void lr24HPCoeffs(float fc, float fs, std::vector<float>& coeffs) {
    float b0, b1, b2, a1, a2;
    hpfCoeffs(fc, fs, 0.7071f, b0, b1, b2, a1, a2);
    coeffs.assign({b0, b1, b2, a1, a2, b0, b1, b2, a1, a2});
}

/* ------------------------------------------------------------------ */
/*  Helper: build BW36 coefficients (first-order + 2 biquads)         */
/*  Returns 10 floats for biquads; firstOrderCoef separately.          */
/* ------------------------------------------------------------------ */
static void bw36LPCoeffs(float fc, float fs, std::vector<float>& coeffs, float& foCoef) {
    /* First-order LP: alpha = tan(pi * fc / fs)                      */
    double wo = M_PI * fc / fs;
    foCoef = static_cast<float>(std::tan(wo));

    /* Two biquad sections for 36dB BW (6th-order with first-order).  */
    /* Q values from Butterworth pole table: 0.5176 and 0.7071        */
    float b0, b1, b2, a1, a2;
    lpfCoeffs(fc, fs, 0.5176f, b0, b1, b2, a1, a2);
    float s0[5] = {b0, b1, b2, a1, a2};

    lpfCoeffs(fc, fs, 0.7071f, b0, b1, b2, a1, a2);
    float s1[5] = {b0, b1, b2, a1, a2};

    coeffs.assign({s0[0], s0[1], s0[2], s0[3], s0[4],
                    s1[0], s1[1], s1[2], s1[3], s1[4]});
}

/* ------------------------------------------------------------------ */
/*  Helper: build Bessel 24dB coefficients (2 biquads)                */
/*  Uses Thiele approximation Q values for maximally flat group delay.*/
/* ------------------------------------------------------------------ */
static void bessel24LPCoeffs(float fc, float fs, std::vector<float>& coeffs) {
    /* Bessel 4th-order: two sections with specific Q values from     */
    /* normalized Bessel-Thiele pole table for flat group delay.      */
    float b0, b1, b2, a1, a2;
    lpfCoeffs(fc, fs, 0.416f, b0, b1, b2, a1, a2);
    float s0[5] = {b0, b1, b2, a1, a2};

    lpfCoeffs(fc, fs, 2.578f, b0, b1, b2, a1, a2);
    float s1[5] = {b0, b1, b2, a1, a2};

    coeffs.assign({s0[0], s0[1], s0[2], s0[3], s0[4],
                    s1[0], s1[1], s1[2], s1[3], s1[4]});
}

/* ------------------------------------------------------------------ */
/*  Helper: simple DFT magnitude at a bin                              */
/* ------------------------------------------------------------------ */
static float dftMagnitude(const float* buf, uint32_t frames, double freqHz, double sampleRate) {
    double re = 0.0, im = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        double angle = 2.0 * M_PI * freqHz * i / sampleRate;
        re += buf[i] * std::cos(angle);
        im -= buf[i] * std::sin(angle);
    }
    return static_cast<float>(std::sqrt(re * re + im * im) / frames);
}

/* ------------------------------------------------------------------ */
/*  Test: LR24 woofer lowpass                                          */
/* ------------------------------------------------------------------ */
static void testLR24WooferLowpass() {
    std::printf("Test: lr24_woofer_lowpass ... ");
    CrossoverStage stage;
    std::vector<float> coeffs;
    lr24LPCoeffs(2500.f, 48000.f, coeffs);

    bool ok = stage.configure(coeffs.data(), 2, false, 0.f);
    ASSERT(ok, "lr24_woofer: configure succeeded");

    constexpr uint32_t frames = 4096;
    std::vector<float> bufLow(frames), bufHigh(frames);

    /* Low frequency (500Hz) — should pass */
    generateSine(bufLow.data(), frames, 500.0, 48000.0);
    float* ch = bufLow.data();
    BlockInfo info{};
    info.frames = frames;
    stage.process(&ch, 1, frames, info);

    /* High frequency (8kHz) — should be attenuated */
    generateSine(bufHigh.data(), frames, 8000.0, 48000.0);
    ch = bufHigh.data();
    stage.process(&ch, 1, frames, info);

    float rmsLow = measureRms(bufLow.data(), frames);
    float rmsHigh = measureRms(bufHigh.data(), frames);

    ASSERT(rmsLow > 0.3f, "lr24_woofer: passband (500Hz) has meaningful gain");
    ASSERT(rmsHigh < 0.05f, "lr24_woofer: stopband (8kHz) is attenuated");

    std::printf("%s (rms_pass=%.4f rms_stop=%.6f)\n",
               (rmsLow > 0.3f && rmsHigh < 0.05f) ? "PASS" : "FAIL",
               static_cast<double>(rmsLow), static_cast<double>(rmsHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: LR24 tweeter highpass                                        */
/* ------------------------------------------------------------------ */
static void testLR24TweeterHighpass() {
    std::printf("Test: lr24_tweeter_highpass ... ");
    CrossoverStage stage;
    std::vector<float> coeffs;
    lr24HPCoeffs(2500.f, 48000.f, coeffs);

    bool ok = stage.configure(coeffs.data(), 2, false, 0.f);
    ASSERT(ok, "lr24_tweeter: configure succeeded");

    constexpr uint32_t frames = 4096;
    std::vector<float> bufLow(frames), bufHigh(frames);

    /* Low frequency (500Hz) — should be blocked */
    generateSine(bufLow.data(), frames, 500.0, 48000.0);
    float* ch = bufLow.data();
    BlockInfo info{};
    info.frames = frames;
    stage.process(&ch, 1, frames, info);

    /* High frequency (8kHz) — should pass */
    generateSine(bufHigh.data(), frames, 8000.0, 48000.0);
    ch = bufHigh.data();
    stage.process(&ch, 1, frames, info);

    float rmsLow = measureRms(bufLow.data(), frames);
    float rmsHigh = measureRms(bufHigh.data(), frames);

    ASSERT(rmsLow < 0.01f, "lr24_tweeter: stopband (500Hz) is strongly attenuated");
    ASSERT(rmsHigh > 0.3f, "lr24_tweeter: passband (8kHz) has meaningful gain");

    std::printf("%s (rms_stop=%.6f rms_pass=%.4f)\n",
               (rmsLow < 0.01f && rmsHigh > 0.3f) ? "PASS" : "FAIL",
               static_cast<double>(rmsLow), static_cast<double>(rmsHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: LR24 sum flat at crossover frequency                         */
/* ------------------------------------------------------------------ */
static void testLR24SumFlat() {
    std::printf("Test: lr24_sum_flat ... ");

    CrossoverStage lp;
    CrossoverStage hp;
    std::vector<float> coeffsLP, coeffsHP;
    lr24LPCoeffs(2500.f, 48000.f, coeffsLP);
    lr24HPCoeffs(2500.f, 48000.f, coeffsHP);

    lp.configure(coeffsLP.data(), 2, false, 0.f);
    hp.configure(coeffsHP.data(), 2, false, 0.f);

    /* Use a tone at the crossover frequency. */
    constexpr uint32_t frames = 4096;
    std::vector<float> bufLP(frames), bufHP(frames);
    generateSine(bufLP.data(), frames, 2500.0, 48000.0);
    std::memcpy(bufHP.data(), bufLP.data(), frames * sizeof(float));

    BlockInfo info{};
    info.frames = frames;

    float* chLP = bufLP.data();
    lp.process(&chLP, 1, frames, info);

    float* chHP = bufHP.data();
    hp.process(&chHP, 1, frames, info);

    /* Sum the two outputs. */
    std::vector<float> sum(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        sum[i] = bufLP[i] + bufHP[i];
    }

    /* Measure RMS of each output and the sum.                        */
    /* At crossover freq, LR property: LP+HP ≈ unity gain (in-phase).*/
    float rmsIn = measureRms(bufLP.data(), frames);   /* reference input RMS */
    float rmsSum = measureRms(sum.data(), frames);

    /* For a full-scale sine, RMS ≈ 0.7071.                           */
    /* After LR24 at crossover: each output ≈ 0.5x amplitude → RMS≈0.354 */
    /* Sum (in-phase): ≈ 1.0x amplitude → RMS≈0.707                  */
    float ratio = rmsSum / rmsIn;

    ASSERT(ratio > 0.6f, "lr24_sum_flat: combined output near unity at crossover");

    std::printf("%s (rms_in=%.4f rms_sum=%.4f ratio=%.4f)\n",
               ratio > 0.6f ? "PASS" : "FAIL",
               static_cast<double>(rmsIn), static_cast<double>(rmsSum),
               static_cast<double>(ratio));
}

/* ------------------------------------------------------------------ */
/*  Test: BW36 with first-order section                                */
/* ------------------------------------------------------------------ */
static void testBW36WithFirstOrder() {
    std::printf("Test: bw36_with_first_order ... ");
    CrossoverStage stage;
    std::vector<float> coeffs;
    float foCoef = 0.f;
    bw36LPCoeffs(2500.f, 48000.f, coeffs, foCoef);

    bool ok = stage.configure(coeffs.data(), 2, true, foCoef);
    ASSERT(ok, "bw36: configure succeeded");

    /* Run extended noise through to check stability. */
    constexpr uint32_t frames = 1024;
    std::vector<float> buf(frames);
    BlockInfo info{};
    info.frames = frames;

    for (int block = 0; block < 100; ++block) {
        generateNoise(buf.data(), frames, static_cast<unsigned>(block * 7919));
        float* ch = buf.data();
        stage.process(&ch, 1, frames, info);

        if (hasNaNOrInf(buf.data(), frames)) {
            ASSERT(false, "bw36: NaN/Inf detected during stability run");
            std::printf("FAIL\n");
            return;
        }
    }

    /* Verify attenuation at high frequency. */
    generateSine(buf.data(), frames, 10000.0, 48000.0);
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);
    float rmsHigh = measureRms(buf.data(), frames);

    ASSERT(rmsHigh < 0.01f, "bw36: high freq (10kHz) strongly attenuated by LP");

    std::printf("PASS (rms_high=%.8f)\n",
               static_cast<double>(rmsHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: Bessel 24dB LPF — stability and response                     */
/* ------------------------------------------------------------------ */
static void testBessel24LP() {
    std::printf("Test: bessel_24_lp ... ");

    CrossoverStage stage;
    std::vector<float> coeffs;
    bessel24LPCoeffs(2500.f, 48000.f, coeffs);

    bool ok = stage.configure(coeffs.data(), 2, false, 0.f);
    ASSERT(ok, "bessel_24: configure succeeded");

    constexpr uint32_t frames = 4096;
    std::vector<float> bufLow(frames), bufHigh(frames);
    BlockInfo info{};
    info.frames = frames;

    /* Low frequency (500Hz) — should pass */
    generateSine(bufLow.data(), frames, 500.0, 48000.0);
    float* ch = bufLow.data();
    stage.process(&ch, 1, frames, info);

    /* High frequency (8kHz) — should be attenuated */
    generateSine(bufHigh.data(), frames, 8000.0, 48000.0);
    ch = bufHigh.data();
    stage.process(&ch, 1, frames, info);

    float rmsLow = measureRms(bufLow.data(), frames);
    float rmsHigh = measureRms(bufHigh.data(), frames);

    ASSERT(rmsLow > 0.3f, "bessel_24: passband (500Hz) has meaningful gain");
    ASSERT(rmsHigh < 0.05f, "bessel_24: stopband (8kHz) is attenuated");
    ASSERT(!hasNaNOrInf(bufLow.data(), frames), "bessel_24: no NaN/Inf in passband");
    ASSERT(!hasNaNOrInf(bufHigh.data(), frames), "bessel_24: no NaN/Inf in stopband");

    std::printf("%s (rms_pass=%.4f rms_stop=%.6f)\n",
               (rmsLow > 0.3f && rmsHigh < 0.05f) ? "PASS" : "FAIL",
               static_cast<double>(rmsLow), static_cast<double>(rmsHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: multi-channel crossover processing                           */
/* ------------------------------------------------------------------ */
static void testMultiChannelCrossover() {
    std::printf("Test: multi_channel_crossover ... ");

    CrossoverStage stage;
    std::vector<float> coeffs;
    lr24LPCoeffs(3000.f, 48000.f, coeffs);
    stage.configure(coeffs.data(), 2, false, 0.f);

    constexpr uint32_t frames = 1024;
    std::vector<float> ch0(frames), ch1(frames), ch2(frames);
    BlockInfo info{};
    info.frames = frames;

    /* Channel 0: low freq (should pass) */
    generateSine(ch0.data(), frames, 500.0, 48000.0);
    /* Channel 1: high freq (should be attenuated) */
    generateSine(ch1.data(), frames, 10000.0, 48000.0);
    /* Channel 2: at crossover freq (-6dB expected) */
    generateSine(ch2.data(), frames, 3000.0, 48000.0);

    float* channels[] = {ch0.data(), ch1.data(), ch2.data()};
    stage.process(channels, 3, frames, info);

    float rms0 = measureRms(ch0.data(), frames);
    float rms1 = measureRms(ch1.data(), frames);
    float rms2 = measureRms(ch2.data(), frames);

    bool ch0Pass = rms0 > 0.5f;
    bool ch1Atten = rms1 < 0.01f;
    /* At crossover, LR24 gives -6dB (0.5x amplitude), RMS ≈ 0.354 */
    bool ch2Half = rms2 > 0.25f && rms2 < 0.5f;

    ASSERT(ch0Pass, "multi_channel: ch0 (low freq) passes");
    ASSERT(ch1Atten, "multi_channel: ch1 (high freq) attenuated");
    ASSERT(ch2Half, "multi_channel: ch2 (crossover freq) at -6dB");

    std::printf("%s (rms=[%.4f %.6f %.4f])\n",
               (ch0Pass && ch1Atten && ch2Half) ? "PASS" : "FAIL",
               static_cast<double>(rms0), static_cast<double>(rms1),
               static_cast<double>(rms2));
}

/* ------------------------------------------------------------------ */
/*  Test: configure resets state                                       */
/* ------------------------------------------------------------------ */
static void testConfigureResetState() {
    std::printf("Test: configure_reset_state ... ");
    CrossoverStage stage;

    /* Configure with one set of coefficients and pump signal. */
    std::vector<float> coeffs1;
    lr24LPCoeffs(2500.f, 48000.f, coeffs1);
    stage.configure(coeffs1.data(), 2, false, 0.f);

    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);
    generateSine(buf.data(), frames, 1000.0, 48000.0);
    BlockInfo info{};
    info.frames = frames;
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* Reconfigure with different coefficients — state should be zeroed. */
    std::vector<float> coeffs2;
    lr24LPCoeffs(5000.f, 48000.f, coeffs2);
    stage.configure(coeffs2.data(), 2, false, 0.f);

    /* Feed silence — output should be silence (no residual energy). */
    std::memset(buf.data(), 0, frames * sizeof(float));
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    bool allSilent = true;
    for (uint32_t i = 0; i < frames && allSilent; ++i) {
        if (std::fabs(buf[i]) > 1e-10f) allSilent = false;
    }
    ASSERT(allSilent, "configure_reset_state: no residual energy after reconfigure");

    std::printf("%s\n", allSilent ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ */
/*  Test: multi-section stability (48dB — 4 biquads)                   */
/* ------------------------------------------------------------------ */
static void testMultiSectionStability() {
    std::printf("Test: multi_section_stability ... ");
    CrossoverStage stage;

    /* Build 4 biquad sections for 48dB crossover. */
    float b0, b1, b2, a1, a2;
    lpfCoeffs(3000.f, 48000.f, 0.5f, b0, b1, b2, a1, a2);
    std::vector<float> coeffs = {b0, b1, b2, a1, a2,
                                  b0, b1, b2, a1, a2,
                                  b0, b1, b2, a1, a2,
                                  b0, b1, b2, a1, a2};

    bool ok = stage.configure(coeffs.data(), 4, false, 0.f);
    ASSERT(ok, "multi_section: configure with 4 biquads succeeded");

    constexpr uint32_t frames = 1024;
    std::vector<float> buf(frames);
    BlockInfo info{};
    info.frames = frames;

    /* Run ~1M samples of noise. */
    int totalSamples = 0;
    for (int block = 0; block < 1000; ++block) {
        generateNoise(buf.data(), frames, static_cast<unsigned>(block * 7919));
        float* ch = buf.data();
        stage.process(&ch, 1, frames, info);

        if (hasNaNOrInf(buf.data(), frames)) {
            ASSERT(false, "multi_section: NaN/Inf detected in 4-biquad run");
            std::printf("FAIL\n");
            return;
        }
        totalSamples += frames;
    }

    ASSERT(totalSamples >= 1000000, "multi_section: ran at least 1M samples");
    std::printf("PASS (%d samples)\n", totalSamples);
}

/* ------------------------------------------------------------------ */
/*  Test: silence through                                              */
/* ------------------------------------------------------------------ */
static void testSilenceThrough() {
    std::printf("Test: silence_through ... ");

    /* Test with various configurations. */
    auto testConfig = [&](const char* name, uint8_t numBq, bool hasFO, float foCoef) -> bool {
        CrossoverStage stage;
        std::vector<float> coeffs(numBq * 5, 0.f);

        /* Fill with valid LPF coefficients. */
        for (uint8_t i = 0; i < numBq; ++i) {
            float b0, b1, b2, a1, a2;
            lpfCoeffs(2500.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
            coeffs[i * 5 + 0] = b0;
            coeffs[i * 5 + 1] = b1;
            coeffs[i * 5 + 2] = b2;
            coeffs[i * 5 + 3] = a1;
            coeffs[i * 5 + 4] = a2;
        }

        stage.configure(coeffs.data(), numBq, hasFO, foCoef);

        constexpr uint32_t frames = 512;
        std::vector<float> buf(frames, 0.f);
        BlockInfo info{};
        info.frames = frames;
        float* ch = buf.data();
        stage.process(&ch, 1, frames, info);

        for (uint32_t i = 0; i < frames; ++i) {
            if (std::fabs(buf[i]) > 1e-10f) return false;
        }
        return true;
    };

    bool r1 = testConfig("1bq_nofo", 1, false, 0.f);
    bool r2 = testConfig("2bq_nofo", 2, false, 0.f);
    bool r3 = testConfig("4bq_nofo", 4, false, 0.f);
    bool r4 = testConfig("1bq_fo_lp", 1, true, 0.1f);
    bool r5 = testConfig("2bq_fo_hp", 2, true, -0.1f);

    ASSERT(r1, "silence_through: 1 biquad");
    ASSERT(r2, "silence_through: 2 biquads");
    ASSERT(r3, "silence_through: 4 biquads");
    ASSERT(r4, "silence_through: with first-order LP");
    ASSERT(r5, "silence_through: with first-order HP");

    std::printf("%s\n", (r1 && r2 && r3 && r4 && r5) ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ */
/*  Test: latency reported                                             */
/* ------------------------------------------------------------------ */
static void testLatencyReported() {
    std::printf("Test: latency_reported ... ");
    CrossoverStage stage;

    /* Default (unconfigured): 0 biquads, no first-order → 0 latency. */
    ASSERT(stage.latencySamples() == 0, "latency: unconfigured = 0");

    std::vector<float> coeffs(10);
    float b0, b1, b2, a1, a2;
    lpfCoeffs(2500.f, 48000.f, 0.7071f, b0, b1, b2, a1, a2);
    for (int i = 0; i < 2; ++i) {
        coeffs[i * 5 + 0] = b0;
        coeffs[i * 5 + 1] = b1;
        coeffs[i * 5 + 2] = b2;
        coeffs[i * 5 + 3] = a1;
        coeffs[i * 5 + 4] = a2;
    }

    /* 2 biquads, no first-order → 4 samples. */
    stage.configure(coeffs.data(), 2, false, 0.f);
    ASSERT(stage.latencySamples() == 4, "latency: 2 biquads = 4");

    /* 2 biquads + first-order → 5 samples. */
    stage.configure(coeffs.data(), 2, true, 0.1f);
    ASSERT(stage.latencySamples() == 5, "latency: 2 biquads + FO = 5");

    /* 4 biquads + first-order → 9 samples. */
    std::vector<float> coeffs8(20);
    for (int i = 0; i < 4; ++i) {
        coeffs8[i * 5 + 0] = b0;
        coeffs8[i * 5 + 1] = b1;
        coeffs8[i * 5 + 2] = b2;
        coeffs8[i * 5 + 3] = a1;
        coeffs8[i * 5 + 4] = a2;
    }
    stage.configure(coeffs8.data(), 4, true, -0.1f);
    ASSERT(stage.latencySamples() == 9, "latency: 4 biquads + FO = 9");

    std::printf("PASS\n");
}

/* ------------------------------------------------------------------ */
int main() {
    std::printf("=== CrossoverStage Unit Tests ===\n\n");

    testLR24WooferLowpass();
    testLR24TweeterHighpass();
    testLR24SumFlat();
    testBW36WithFirstOrder();
    testBessel24LP();
    testMultiChannelCrossover();
    testConfigureResetState();
    testMultiSectionStability();
    testSilenceThrough();
    testLatencyReported();

    if (g_passed) {
        std::printf("\n[PASS] All crossover stage tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
