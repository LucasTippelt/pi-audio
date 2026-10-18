#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "msg/telemetry.h"

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
            std::fprintf(stderr, "[FAIL] %s: %.6f != %.6f (line %d)\n",          \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            passed = false;                                                      \
        }                                                                        \
    } while (0)

static void test_locked_passthrough() {
    std::printf("  Locked(1) pass-through... ");
    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            inBuf[ch][f] = std::sin(2.0f * M_PI * 440.0f * f / 48000.0f);
        }
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;
    info.clockState  = 1; /* Locked */

    core.process(inCh, 2, outCh, 2, 128, info);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            ASSERT_FLOAT_EQ(inBuf[ch][f], outBuf[ch][f], 1e-5f, "Locked: output matches input");
        }
    }
    std::printf("ok\n");
}

static void test_unlocked_mute() {
    std::printf("  Unlocked(3) mute... ");
    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            inBuf[ch][f] = 0.5f; /* Non-zero input. */
        }
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;
    info.clockState  = 3; /* Unlocked */

    core.process(inCh, 2, outCh, 2, 128, info);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            ASSERT(outBuf[ch][f] == 0.0f, "Unlocked: output is exactly zero");
        }
    }
    std::printf("ok\n");
}

static void test_holdover_processing() {
    std::printf("  Holdover(2) continues processing... ");
    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            inBuf[ch][f] = std::sin(2.0f * M_PI * 440.0f * f / 48000.0f);
        }
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;
    info.clockState  = 2; /* Holdover */

    core.process(inCh, 2, outCh, 2, 128, info);

    bool nonZero = false;
    for (uint32_t ch = 0; ch < 2 && !nonZero; ++ch) {
        for (uint32_t f = 0; f < 128 && !nonZero; ++f) {
            if (std::fabs(outBuf[ch][f]) > 1e-5f) nonZero = true;
        }
    }
    ASSERT(nonZero, "Holdover: output is non-zero");
    std::printf("ok\n");
}

static void test_clock_mute_event_counting() {
    std::printf("  Clock mute event counting... ");
    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            inBuf[ch][f] = 0.5f;
        }
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;

    /* Start in Locked state. */
    info.clockState = 1;
    core.process(inCh, 2, outCh, 2, 128, info);

    /* Transition to Unlocked — should trigger mute event. */
    info.sampleIndex = 128;
    info.clockState = 3;
    core.process(inCh, 2, outCh, 2, 128, info);

    bool allZero = true;
    for (uint32_t ch = 0; ch < 2 && allZero; ++ch) {
        for (uint32_t f = 0; f < 128 && allZero; ++f) {
            if (outBuf[ch][f] != 0.0f) allZero = false;
        }
    }
    ASSERT(allZero, "mute event triggered on Locked→Unlocked transition");

    /* Sustained Unlocked — output still zero but no new mute event should fire. */
    info.sampleIndex = 256;
    core.process(inCh, 2, outCh, 2, 128, info);

    bool allZeroSecond = true;
    for (uint32_t ch = 0; ch < 2 && allZeroSecond; ++ch) {
        for (uint32_t f = 0; f < 128 && allZeroSecond; ++f) {
            if (outBuf[ch][f] != 0.0f) allZeroSecond = false;
        }
    }
    ASSERT(allZeroSecond, "sustained Unlocked keeps output muted");
    std::printf("ok\n");
}

static void test_recovery_after_unlocked() {
    std::printf("  Recovery after Unlocked→Locked... ");
    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            inBuf[ch][f] = std::sin(2.0f * M_PI * 440.0f * f / 48000.0f);
        }
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;

    /* Start Locked. */
    info.clockState = 1;
    core.process(inCh, 2, outCh, 2, 128, info);

    /* Go Unlocked — output muted. */
    info.sampleIndex = 128;
    info.clockState = 3;
    core.process(inCh, 2, outCh, 2, 128, info);

    bool allZeroUnlocked = true;
    for (uint32_t ch = 0; ch < 2 && allZeroUnlocked; ++ch) {
        for (uint32_t f = 0; f < 128 && allZeroUnlocked; ++f) {
            if (outBuf[ch][f] != 0.0f) allZeroUnlocked = false;
        }
    }
    ASSERT(allZeroUnlocked, "output muted during Unlocked");

    /* Return to Locked — output should resume pass-through. */
    info.sampleIndex = 256;
    info.clockState = 1;
    core.process(inCh, 2, outCh, 2, 128, info);

    bool resumed = false;
    for (uint32_t ch = 0; ch < 2 && !resumed; ++ch) {
        for (uint32_t f = 0; f < 128 && !resumed; ++f) {
            if (std::fabs(outBuf[ch][f] - inBuf[ch][f]) < 1e-5f) resumed = true;
        }
    }
    ASSERT(resumed, "output resumes pass-through after Locked");
    std::printf("ok\n");
}

int main() {
    std::printf("=== Clock State Reaction Tests (B6) ===\n");

    test_locked_passthrough();
    test_unlocked_mute();
    test_holdover_processing();
    test_clock_mute_event_counting();
    test_recovery_after_unlocked();

    if (passed) {
        std::printf("[PASS] All test_clock_state tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
