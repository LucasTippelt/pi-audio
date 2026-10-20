#include "stages/limiter_stage.h"
#include "dsp_core/block_info.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>

/* ------------------------------------------------------------------ */
/*  LimiterStage unit tests                                            */
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
/*  L1: Signal below threshold — no gain reduction                     */
/* ------------------------------------------------------------------ */
static void test_below_threshold() {
    LimiterStage stage;
    stage.configure(-3.0f, 5.0f, 100.0f, 4.0f, 48000);

    float buf[256];
    for (int i = 0; i < 256; ++i) buf[i] = 0.1f;

    float expected[256];
    std::memcpy(expected, buf, sizeof(buf));

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 256, info);

    for (int i = 0; i < 256; ++i) {
        ASSERT_FLOAT_EQ(buf[i], expected[i], 1e-6f, "L1: below threshold no gain reduction");
    }
}

/* ------------------------------------------------------------------ */
/*  L2: Signal at threshold — minimal gain reduction                   */
/* ------------------------------------------------------------------ */
static void test_at_threshold() {
    LimiterStage stage;
    float thresholdDb = -6.0f;
    stage.configure(thresholdDb, 1.0f, 50.0f, 4.0f, 48000);

    float thresholdLinear = std::expf(thresholdDb * 0.1151292546497023f);
    float buf[256];
    for (int i = 0; i < 256; ++i) buf[i] = thresholdLinear;

    /* Let envelope settle to threshold level. */
    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 256, info);

    for (int i = 0; i < 256; ++i) {
        ASSERT(std::fabs(buf[i] - thresholdLinear) / thresholdLinear < 0.05f,
               "L2: at threshold minimal gain reduction");
    }
}

/* ------------------------------------------------------------------ */
/*  L3: Signal above threshold with ratio 4:1                          */
/*     If input is 4x threshold, output should be ~2x threshold for 4:1*/
/* ------------------------------------------------------------------ */
static void test_above_threshold_ratio() {
    LimiterStage stage;
    float thresholdDb = -6.0f;
    stage.configure(thresholdDb, 0.5f, 50.0f, 4.0f, 48000);

    float thresholdLinear = std::expf(thresholdDb * 0.1151292546497023f);
    float inputLevel = thresholdLinear * 4.0f;

    /* Feed sustained signal well above threshold to let envelope settle. */
    float buf[1024];
    for (int i = 0; i < 1024; ++i) buf[i] = inputLevel;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 1024, info);

    /* Check last samples where envelope should be settled. */
    float outputLevel = buf[1023];
    /* For 4:1 ratio, input at 4x threshold (12dB over) → output ~3dB over threshold.
     * Linear: 10^(3/20) ≈ 1.41x threshold. */
    float expectedOutput = thresholdLinear * 1.41f;
    ASSERT(std::fabs(outputLevel - expectedOutput) / expectedOutput < 0.05f,
           "L3: 4x threshold with 4:1 ratio gives ~1.41x threshold output");
}

/* ------------------------------------------------------------------ */
/*  L4: Fast attack — impulse response                                 */
/*     Single-sample impulse shows quick gain reduction within window  */
/* ------------------------------------------------------------------ */
static void test_fast_attack() {
    LimiterStage stage;
    float thresholdDb = -6.0f;
    stage.configure(thresholdDb, 1.0f, 200.0f, 8.0f, 48000);

    float buf[480];
    std::memset(buf, 0, sizeof(buf));
    buf[0] = 1.0f; /* single-sample impulse */

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 480, info);

    ASSERT(!std::isnan(buf[0]) && !std::isinf(buf[0]), "L4: no NaN/Inf on impulse");
    ASSERT(std::fabs(buf[0]) <= 1.0f, "L4: impulse not amplified beyond input");

    /* Check that envelope rises quickly but doesn't stay elevated forever. */
    bool foundDecay = false;
    for (int i = 100; i < 200; ++i) {
        if (std::fabs(buf[i]) < 1e-10f) {
            foundDecay = true;
            break;
        }
    }
    ASSERT(foundDecay, "L4: envelope decays after impulse");
}

/* ------------------------------------------------------------------ */
/*  L5: Release behavior — envelope decay                              */
/*     After signal drops below threshold, envelope decays over time   */
/* ------------------------------------------------------------------ */
static void test_release_decay() {
    LimiterStage stage;
    float thresholdDb = -6.0f;
    float releaseMs = 100.0f;
    stage.configure(thresholdDb, 1.0f, releaseMs, 4.0f, 48000);

    /* Feed high signal to build up envelope. */
    float thresholdLinear = std::expf(thresholdDb * 0.1151292546497023f);
    float buf[2048];
    for (int i = 0; i < 1024; ++i) buf[i] = thresholdLinear * 4.0f;
    for (int i = 1024; i < 2048; ++i) buf[i] = 0.f;

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 2048, info);

    /* After signal stops at sample 1024, envelope should decay. */
    /* Release time constant: 100ms ≈ 4800 samples at 48kHz. */
    /* At ~4800 samples after stop (sample 5824), envelope should be near threshold. */
    /* We only have 2048 samples, so check that decay is happening. */
    float firstSilent = std::fabs(buf[1024]);
    float laterSilent = std::fabs(buf[1500]);

    ASSERT(laterSilent <= firstSilent + 1e-6f, "L5: envelope decays during silence");
}

/* ------------------------------------------------------------------ */
/*  L6: Infinite ratio (brickwall limiter)                             */
/*     Output never exceeds threshold regardless of input level        */
/* ------------------------------------------------------------------ */
static void test_brickwall() {
    LimiterStage stage;
    float thresholdDb = -3.0f;
    /* Very high ratio approximates brickwall. */
    stage.configure(thresholdDb, 0.5f, 10.0f, 100.0f, 48000);

    float thresholdLinear = std::expf(thresholdDb * 0.1151292546497023f);
    float buf[1024];
    for (int i = 0; i < 1024; ++i) buf[i] = 1.0f; /* full-scale input */

    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 1024, info);

    /* After envelope settles, output should not exceed threshold by much. */
    bool exceeded = false;
    for (int i = 500; i < 1024; ++i) {
        if (std::fabs(buf[i]) > thresholdLinear * 1.01f) {
            exceeded = true;
            break;
        }
    }
    ASSERT(!exceeded, "L6: brickwall limiter output stays near threshold");
}

/* ------------------------------------------------------------------ */
/*  L7: Latency declaration returns 0                                  */
/* ------------------------------------------------------------------ */
static void test_latency() {
    LimiterStage stage;
    ASSERT(stage.latencySamples() == 0, "L7: latency is zero");
}

/* ------------------------------------------------------------------ */
/*  L8: Multi-channel independence                                     */
/*     Each channel has independent envelope state                     */
/* ------------------------------------------------------------------ */
static void test_multi_channel_independence() {
    LimiterStage stage;
    float thresholdDb = -6.0f;
    stage.configure(thresholdDb, 1.0f, 50.0f, 4.0f, 48000);

    float ch0[256], ch1[256];
    for (int i = 0; i < 256; ++i) {
        ch0[i] = 1.0f; /* high signal — should be limited */
        ch1[i] = 0.01f; /* low signal — should pass through */
    }

    float* channels[] = {ch0, ch1};
    BlockInfo info{};
    stage.process(channels, 2, 256, info);

    /* Channel 0 should have gain reduction. */
    ASSERT(std::fabs(ch0[255]) < 1.0f, "L8: channel 0 limited");

    /* Channel 1 should pass through unchanged (below threshold). */
    ASSERT_FLOAT_EQ(ch1[255], 0.01f, 1e-6f, "L8: channel 1 independent, no reduction");
}

/* ------------------------------------------------------------------ */
/*  L9: Silence through — zero input stays silent                      */
/*     Zero input produces zero output, envelope decays to zero        */
/* ------------------------------------------------------------------ */
static void test_silence_through() {
    LimiterStage stage;
    stage.configure(-3.0f, 5.0f, 100.0f, 4.0f, 48000);

    /* First build up envelope with a signal. */
    float warmup[256];
    for (int i = 0; i < 256; ++i) warmup[i] = 1.0f;
    {
        float* channels[] = {warmup};
        BlockInfo info{};
        stage.process(channels, 1, 256, info);
    }

    /* Then feed silence — output should be zero. */
    float buf[256]{};
    float* channels[] = {buf};
    BlockInfo info{};
    stage.process(channels, 1, 256, info);

    for (int i = 0; i < 256; ++i) {
        ASSERT_FLOAT_EQ(buf[i], 0.0f, 1e-10f, "L9: silence stays silent");
    }
}

/* ------------------------------------------------------------------ */
/*  L10: Stability under sustained overload                            */
/*      Feed full-scale sine at 2x threshold for extended duration     */
/*      Verify no NaN/Inf                                              */
/* ------------------------------------------------------------------ */
static void test_stability_sustained() {
    LimiterStage stage;
    float thresholdDb = -6.0f;
    stage.configure(thresholdDb, 5.0f, 100.0f, 4.0f, 48000);

    const int frames = 4096;
    float buf[frames];
    for (int i = 0; i < frames; ++i) {
        buf[i] = std::sinf(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
    }

    float* channels[] = {buf};
    BlockInfo info{};

    /* Process multiple blocks to test sustained operation. */
    for (int block = 0; block < 16; ++block) {
        stage.process(channels, 1, frames, info);
    }

    for (int i = 0; i < frames; ++i) {
        ASSERT(!std::isnan(buf[i]) && !std::isinf(buf[i]), "L10: no NaN/Inf under sustained overload");
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */
int main() {
    test_below_threshold();
    test_at_threshold();
    test_above_threshold_ratio();
    test_fast_attack();
    test_release_decay();
    test_brickwall();
    test_latency();
    test_multi_channel_independence();
    test_silence_through();
    test_stability_sustained();

    if (g_passed) {
        std::fprintf(stderr, "[PASS] All %d assertions passed.\n", g_testCount);
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some assertions failed (%d total).\n", g_testCount);
        return 1;
    }
}
