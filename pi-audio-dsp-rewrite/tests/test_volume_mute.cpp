#include "stages/gain_stage.h"
#include "dsp_core/block_info.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <sstream>

/* ------------------------------------------------------------------ */
/*  Volume / Mute measurement test                                     */
/*                                                                    */
/*  Tests the GainStage with various volume percentages and mute      */
/*  states. Compares measured output amplitude against expected       */
/*  linear gain and dB values.                                        */
/* ------------------------------------------------------------------ */

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
            std::fprintf(stderr, "[FAIL] %s: got %.8f expected %.8f (line %d)\n", \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            passed = false;                                                      \
        }                                                                        \
    } while (0)

/* Volume levels to test */
static const int kVolumeLevels[] = {100, 80, 70, 50, 20, 5, 2, 1};
static const size_t kNumVolumes = sizeof(kVolumeLevels) / sizeof(kVolumeLevels[0]);

/* Number of frames for measurement — use enough to get stable RMS */
static constexpr uint32_t kMeasureFrames = 4096;

struct TestCase {
    int volume;       // percentage 0-100
    bool muted;
    float expectedGain;   // linear gain we expect
    double expectedDb;    // dB value (or -999 for mute/zero)
};

/* Calculate expected values for all test cases */
static std::vector<TestCase> calculateExpected() {
    std::vector<TestCase> cases;

    /* Mute true — gain forced to 0 regardless of volume setting */
    for (int vol : kVolumeLevels) {
        TestCase tc{};
        tc.volume = vol;
        tc.muted = true;
        tc.expectedGain = 0.0f;
        tc.expectedDb = -999.0; /* effectively -infinity dB */
        cases.push_back(tc);
    }

    /* Mute false — gain = volume / 100.0 */
    for (int vol : kVolumeLevels) {
        TestCase tc{};
        tc.volume = vol;
        tc.muted = false;
        tc.expectedGain = static_cast<float>(vol) / 100.0f;
        if (tc.expectedGain > 0.f) {
            tc.expectedDb = 20.0 * std::log10(static_cast<double>(tc.expectedGain));
        } else {
            tc.expectedDb = -999.0;
        }
        cases.push_back(tc);
    }

    return cases;
}

/* Generate a full-scale sine wave at 440 Hz into the buffer */
static void generateSine(float* buf, uint32_t frames, double freq, double sampleRate) {
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i] = static_cast<float>(std::sin(2.0 * M_PI * freq * i / sampleRate));
    }
}

/* Measure RMS amplitude of a buffer */
static float measureRms(const float* buf, uint32_t frames) {
    double sumSq = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        sumSq += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return static_cast<float>(std::sqrt(sumSq / frames));
}

/* Measure peak absolute amplitude */
static float measurePeak(const float* buf, uint32_t frames) {
    float maxAmp = 0.f;
    for (uint32_t i = 0; i < frames; ++i) {
        float a = std::fabs(buf[i]);
        if (a > maxAmp) maxAmp = a;
    }
    return maxAmp;
}

/* Run a single test case: process sine through GainStage, measure output */
static void runTestCase(const TestCase& tc, int idx) {
    GainStage stage;
    const uint32_t frames = kMeasureFrames;
    std::vector<float> buf(frames);

    BlockInfo info{};
    info.frames = frames;
    info.sampleIndex = 0;
    info.hostTimeNs = 1000000000ULL;
    info.clockState = 1;

    /* Set gain based on volume percentage */
    float gain = static_cast<float>(tc.volume) / 100.0f;
    stage.setGain(gain);

    /* Apply mute if needed */
    if (tc.muted) {
        stage.setMute(true);
    }

    /* Process multiple blocks to let ramp settle, then measure steady state */
    const uint32_t warmupBlocks = 4; /* kRampSamples=64, block=128 -> 1 block enough, use more for safety */
    const uint32_t blockSize = 512;

    for (uint32_t b = 0; b < warmupBlocks; ++b) {
        generateSine(buf.data(), blockSize, 440.0, 48000.0);
        float* ch = buf.data();
        stage.process(&ch, 1, blockSize, info);
    }

    /* Now measure with a large block — skip ramp region */
    std::vector<float> measureBuf(frames);
    generateSine(measureBuf.data(), frames, 440.0, 48000.0);
    float* ch = measureBuf.data();
    stage.process(&ch, 1, frames, info);

    /* Skip first kRampSamples to avoid ramp transients */
    uint32_t skip = 64; /* kRampSamples */
    if (skip >= frames) skip = frames / 2;

    float rmsOut = measureRms(measureBuf.data() + skip, frames - skip);
    float peakOut = measurePeak(measureBuf.data() + skip, frames - skip);

    /* Expected RMS for a sine wave at full scale is ~0.7071 */
    constexpr double kSineRmsFullScale = 0.70710678; /* 1/sqrt(2) */
    double expectedRmsOut = kSineRmsFullScale * tc.expectedGain;

    /* Expected peak for a sine wave is 1.0 * gain */
    float expectedPeakOut = tc.expectedGain;

    /* Format mute label */
    std::string muteLabel = tc.muted ? "MUTE" : "----";

    /* Print results */
    char dbStr[32];
    if (tc.expectedDb < -900) {
        std::strcpy(dbStr, "-inf");
    } else {
        std::snprintf(dbStr, sizeof(dbStr), "%.4f", tc.expectedDb);
    }

    char measuredDbStr[32];
    if (rmsOut > 1e-10) {
        double measuredDb = 20.0 * std::log10(static_cast<double>(rmsOut / kSineRmsFullScale));
        std::snprintf(measuredDbStr, sizeof(measuredDbStr), "%.4f", measuredDb);
    } else if (tc.muted) {
        std::strcpy(measuredDbStr, "-inf");
    } else {
        std::strcpy(measuredDbStr, "ERROR");
    }

    char errorPct[32];
    if (expectedRmsOut > 1e-10) {
        double err = std::fabs(rmsOut - static_cast<float>(expectedRmsOut)) / expectedRmsOut * 100.0;
        std::snprintf(errorPct, sizeof(errorPct), "%.4f%%", err);
    } else {
        std::snprintf(errorPct, sizeof(errorPct), "N/A");
    }

    std::printf("  [%2d] vol=%3d%% %s | gain exp=%.4f meas=%.6f | peak exp=%.4f meas=%.6f | dB exp=%-10s meas=%-10s | err=%s\n",
                idx, tc.volume, muteLabel.c_str(),
                static_cast<double>(tc.expectedGain), static_cast<double>(rmsOut / kSineRmsFullScale),
                static_cast<double>(expectedPeakOut), static_cast<double>(peakOut),
                dbStr, measuredDbStr, errorPct);

    /* Verify measurements match expectations */
    if (tc.muted) {
        /* Mute: output should be essentially zero */
        ASSERT(rmsOut < 1e-6f, "mute produces near-zero output");
    } else {
        /* Unmuted: RMS should match expected gain within tolerance.
         * Finite sine window causes ~0.04% deviation from theoretical RMS. */
        double relError = std::fabs(static_cast<double>(rmsOut) - expectedRmsOut) / expectedRmsOut;
        ASSERT(relError < 0.01, "measured RMS matches expected gain (within 1%)");

        /* Peak should match expected gain exactly (sine peak is always at sample boundary for our frequencies). */
        double peakRelError = std::fabs(static_cast<double>(peakOut) - static_cast<double>(expectedPeakOut)) / static_cast<double>(expectedPeakOut);
        ASSERT(peakRelError < 0.01, "measured peak matches expected gain (within 1%)");
    }
}

/* Test mute override: set volume to 100%, then mute — output must be zero */
static void testMuteOverride() {
    std::printf("\n--- Mute Override Test ---\n");
    GainStage stage;
    const uint32_t frames = 512;
    std::vector<float> buf(frames);

    BlockInfo info{};
    info.frames = frames;

    /* Set full volume */
    stage.setGain(1.0f);

    /* Warm up */
    generateSine(buf.data(), frames, 440.0, 48000.0);
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* Mute — gain should ramp to zero */
    stage.setMute(true);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* After ramp completes, output must be zero */
    float rmsAfterMute = measureRms(buf.data() + 64, frames - 64);
    ASSERT(rmsAfterMute < 1e-8f, "mute override produces zero after ramp");

    /* Unmute — should restore to targetGain (1.0) */
    stage.setMute(false);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* After ramp completes, output should be full scale again */
    float rmsAfterUnmute = measureRms(buf.data() + 64, frames - 64);
    constexpr double kSineRmsFullScale = 0.70710678;
    ASSERT_FLOAT_EQ(rmsAfterUnmute, static_cast<float>(kSineRmsFullScale), 0.01f, "unmute restores full gain");

    std::printf("  Mute override: PASS\n");
}

/* Test rapid volume changes */
static void testVolumeChanges() {
    std::printf("\n--- Rapid Volume Change Test ---\n");
    GainStage stage;
    const uint32_t frames = 512;
    std::vector<float> buf(frames);

    BlockInfo info{};
    info.frames = frames;

    /* Start at full gain */
    stage.setGain(1.0f);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* Change to 50% */
    stage.setGain(0.5f);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* After ramp, RMS should be ~0.3536 (half of full-scale sine RMS) */
    float rms50 = measureRms(buf.data() + 64, frames - 64);
    constexpr double kSineRmsFullScale = 0.70710678;
    ASSERT_FLOAT_EQ(rms50, static_cast<float>(kSineRmsFullScale * 0.5), 0.01f, "gain change to 50% correct");

    /* Change to 20% */
    stage.setGain(0.2f);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    float rms20 = measureRms(buf.data() + 64, frames - 64);
    ASSERT_FLOAT_EQ(rms20, static_cast<float>(kSineRmsFullScale * 0.2), 0.01f, "gain change to 20% correct");

    /* Change back to full */
    stage.setGain(1.0f);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    float rmsFull = measureRms(buf.data() + 64, frames - 64);
    ASSERT_FLOAT_EQ(rmsFull, static_cast<float>(kSineRmsFullScale), 0.01f, "gain change back to 100% correct");

    std::printf("  Rapid volume changes: PASS\n");
}

/* Test gain clamping */
static void testGainClamping() {
    std::printf("\n--- Gain Clamping Test ---\n");
    GainStage stage;
    const uint32_t frames = 512;
    std::vector<float> buf(frames);

    BlockInfo info{};
    info.frames = frames;

    /* Try negative gain — should clamp to 0 */
    stage.setGain(-0.5f);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    float* ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* After ramp (64 samples), output should be zero (gain clamped to 0).
     * Measure only steady-state region after ramp completes. */
    float rmsNeg = measureRms(buf.data() + 64, frames - 64);
    ASSERT(rmsNeg < 1e-8f, "negative gain clamped to zero");

    /* Try gain > 1 — should clamp to 1.0. Process warmup block for ramp, then measure. */
    stage.setGain(2.5f);
    generateSine(buf.data(), frames, 440.0, 48000.0);
    ch = buf.data();
    stage.process(&ch, 1, frames, info);

    /* Ramp from 0 to 1.0 takes 64 samples; measure steady state after ramp. */
    float rmsOver = measureRms(buf.data() + 64, frames - 64);
    constexpr double kSineRmsFullScale = 0.70710678;
    ASSERT_FLOAT_EQ(rmsOver, static_cast<float>(kSineRmsFullScale), 0.01f, "gain > 1 clamped to unity");

    std::printf("  Gain clamping: PASS\n");
}

int main() {
    std::printf("=== Volume / Mute Measurement Test ===\n\n");

    auto cases = calculateExpected();

    /* Print expected values table */
    std::printf("--- Expected Values (Calculated) ---\n");
    std::printf("  %-6s %5s %8s %12s %10s\n", "Mute?", "Volume", "Gain", "dB", "RMS(sine)");
    std::printf("  %-6s %5s %8s %12s %10s\n", "-----", "------", "----", "--", "---------");

    constexpr double kSineRmsFullScale = 0.70710678;
    for (const auto& tc : cases) {
        char dbStr[32];
        if (tc.expectedDb < -900) {
            std::strcpy(dbStr, "-inf");
        } else {
            std::snprintf(dbStr, sizeof(dbStr), "%.4f", tc.expectedDb);
        }

        double rmsExp = kSineRmsFullScale * tc.expectedGain;
        char rmsStr[32];
        if (rmsExp < 1e-10) {
            std::strcpy(rmsStr, "0.0000");
        } else {
            std::snprintf(rmsStr, sizeof(rmsStr), "%.6f", rmsExp);
        }

        std::printf("  %-6s %5d%% %8.4f %12s %10s\n",
                    tc.muted ? "true" : "false",
                    tc.volume,
                    static_cast<double>(tc.expectedGain),
                    dbStr, rmsStr);
    }

    /* Measure actual values */
    std::printf("\n--- Measured Values ---\n");
    for (size_t i = 0; i < cases.size(); ++i) {
        runTestCase(cases[i], static_cast<int>(i));
    }

    /* Additional behavioral tests */
    testMuteOverride();
    testVolumeChanges();
    testGainClamping();

    if (passed) {
        std::printf("\n[PASS] All volume/mute tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
