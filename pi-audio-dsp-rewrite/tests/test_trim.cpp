#include "stages/trim_stage.h"
#include "dsp_core/block_info.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

/* ------------------------------------------------------------------ */
/*  TrimStage unit tests                                               */
/* ------------------------------------------------------------------ */

static bool g_passed = true;
static int g_testCount = 0;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        ++g_testCount;                                                           \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

#define ASSERT_FLOAT_EQ(a, b, eps, msg)                                          \
    do {                                                                         \
        ++g_testCount;                                                           \
        if (std::fabs((a) - (b)) > (eps)) {                                      \
            std::fprintf(stderr, "[FAIL] %s: got %.8f expected %.8f (line %d)\n", \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/*  T1: Unity trim (0 dB) — output equals input                        */
/* ------------------------------------------------------------------ */
static void test_unity_trim() {
    TrimStage stage;
    stage.setTrimDb(0.0f);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = static_cast<float>(i % 7 - 3) / 4.0f;
    float expected[128];
    std::memcpy(expected, buf, sizeof(buf));

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expected[i], 1e-6f, "T1: unity trim output matches input");
    }
}

/* ------------------------------------------------------------------ */
/*  T2: Positive trim (+6dB ≈ 2x gain)                                 */
/* ------------------------------------------------------------------ */
static void test_positive_trim() {
    TrimStage stage;
    stage.setTrimDb(6.0f);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = 0.5f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    float expectedGain = std::expf(6.0f * 0.1151292546497023f);
    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], 0.5f * expectedGain, 0.01f, "T2: +6dB doubles amplitude");
    }
}

/* ------------------------------------------------------------------ */
/*  T3: Negative trim (-6dB ≈ 0.5x gain)                               */
/* ------------------------------------------------------------------ */
static void test_negative_trim() {
    TrimStage stage;
    stage.setTrimDb(-6.0f);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = 1.0f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    float expectedGain = std::expf(-6.0f * 0.1151292546497023f);
    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expectedGain, 0.01f, "T3: -6dB halves amplitude");
    }
}

/* ------------------------------------------------------------------ */
/*  T4: Phase inversion flips sign                                     */
/* ------------------------------------------------------------------ */
static void test_phase_invert() {
    TrimStage stage;
    stage.setGain(1.0f);
    stage.setPhaseInvert(true);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = static_cast<float>(i % 5 - 2) / 3.0f;
    float expected[128];
    for (int i = 0; i < 128; ++i) expected[i] = -buf[i];

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expected[i], 1e-6f, "T4: phase invert negates samples");
    }
}

/* ------------------------------------------------------------------ */
/*  T5: Combined -3dB + phase invert                                   */
/* ------------------------------------------------------------------ */
static void test_combined_trim_invert() {
    TrimStage stage;
    stage.setTrimDb(-3.0f);
    stage.setPhaseInvert(true);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = 1.0f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    float magGain = std::expf(-3.0f * 0.1151292546497023f);
    float expected = -magGain;
    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expected, 0.001f, "T5: combined -3dB + invert");
    }
}

/* ------------------------------------------------------------------ */
/*  T6: Extreme positive trim (+24dB ≈ 15.8x)                          */
/* ------------------------------------------------------------------ */
static void test_extreme_positive() {
    TrimStage stage;
    stage.setTrimDb(24.0f);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = 0.01f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    float expectedGain = std::expf(24.0f * 0.1151292546497023f);
    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], 0.01f * expectedGain, 0.1f, "T6: +24dB extreme boost");
        ASSERT(!std::isnan(buf[i]) && !std::isinf(buf[i]), "T6: no overflow for moderate input");
    }
}

/* ------------------------------------------------------------------ */
/*  T7: Extreme negative trim (-48dB ≈ 0.000126x)                      */
/* ------------------------------------------------------------------ */
static void test_extreme_negative() {
    TrimStage stage;
    stage.setTrimDb(-48.0f);

    float buf[128];
    for (int i = 0; i < 128; ++i) buf[i] = 1.0f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 128, info);

    float expectedGain = std::expf(-48.0f * 0.1151292546497023f);
    for (int i = 0; i < 128; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expectedGain, 1e-8f, "T7: -48dB near-silence");
        ASSERT(!std::isnan(buf[i]) && !std::isinf(buf[i]), "T7: no NaN/Inf");
    }
}

/* ------------------------------------------------------------------ */
/*  T8: Multi-channel processing                                       */
/* ------------------------------------------------------------------ */
static void test_multi_channel() {
    TrimStage stage;
    stage.setGain(2.0f);

    float ch0[64], ch1[64], ch2[64];
    for (int i = 0; i < 64; ++i) {
        ch0[i] = 0.5f;
        ch1[i] = -0.25f;
        ch2[i] = 0.1f;
    }

    float* channels[] = {ch0, ch1, ch2};
    BlockInfo info{};
    stage.process(channels, 3, 64, info);

    for (int i = 0; i < 64; ++i) {
        ASSERT_FLOAT_EQ(ch0[i], 1.0f, 1e-6f, "T8: ch0 doubled");
        ASSERT_FLOAT_EQ(ch1[i], -0.5f, 1e-6f, "T8: ch1 doubled");
        ASSERT_FLOAT_EQ(ch2[i], 0.2f, 1e-6f, "T8: ch2 doubled");
    }
}

/* ------------------------------------------------------------------ */
/*  T9: Latency declaration returns 0                                  */
/* ------------------------------------------------------------------ */
static void test_latency() {
    TrimStage stage;
    ASSERT(stage.latencySamples() == 0, "T9: latency is zero");
}

/* ------------------------------------------------------------------ */
/*  T10: Silence through                                               */
/* ------------------------------------------------------------------ */
static void test_silence_through() {
    float gains[] = {0.0f, 1.0f, -1.0f, 10.0f, -10.0f, 0.5f};

    for (float gain : gains) {
        TrimStage stage;
        stage.setGain(gain);

        float buf[64]{};

        float* channels[] = {buf};
        BlockInfo info{};
        stage.process(channels, 1, 64, info);

        for (int i = 0; i < 64; ++i) {
            ASSERT_FLOAT_EQ(buf[i], 0.0f, 1e-10f, "T10: silence stays silent");
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main() {
    test_unity_trim();
    test_positive_trim();
    test_negative_trim();
    test_phase_invert();
    test_combined_trim_invert();
    test_extreme_positive();
    test_extreme_negative();
    test_multi_channel();
    test_latency();
    test_silence_through();

    if (g_passed) {
        std::fprintf(stderr, "[PASS] All %d assertions passed.\n", g_testCount);
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some assertions failed (%d total).\n", g_testCount);
        return 1;
    }
}
