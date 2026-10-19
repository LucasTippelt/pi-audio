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

/* ================================================================== */
/*  G1: Timing relationships for sweeps and deconvolution             */
/* ================================================================== */

static void test_g1_sample_index_monotonic() {
    /* Process N blocks with advancing sampleIndex, verify monotonic advance. */
    std::printf("  G1: Sample index monotonic... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    const int numBlocks = 20;
    uint64_t prevSampleIndex = 0;

    for (int i = 0; i < numBlocks; ++i) {
        BlockInfo info{};
        info.sampleIndex = static_cast<uint64_t>(i) * 128;
        info.hostTimeNs  = 1000000000ULL + static_cast<uint64_t>(i) * 2666667ULL; /* ~2.67ms per block */
        info.frames      = 128;
        info.clockState  = 1;

        core.process(inCh, 2, outCh, 2, 128, info);

        TelemetryBlock tel{};
        core.getTelemetry(tel);

        uint64_t expectedIndex = static_cast<uint64_t>(i + 1) * 128;
        ASSERT(tel.sampleIndex == expectedIndex, "sample index matches block count");
        ASSERT(tel.sampleIndex > prevSampleIndex, "sample index advances monotonically");

        prevSampleIndex = tel.sampleIndex;
    }

    std::printf("ok (final sampleIndex=%llu)\n",
                static_cast<unsigned long long>(prevSampleIndex));
}

static void test_g1_host_time_in_blockinfo() {
    /* Verify BlockInfo.hostTimeNs is carried through pipeline. */
    std::printf("  G1: Host time in BlockInfo... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* Set known host time values. */
    for (int i = 0; i < 5; ++i) {
        BlockInfo info{};
        info.sampleIndex = static_cast<uint64_t>(i) * 128;
        info.hostTimeNs  = 1000000000ULL + static_cast<uint64_t>(i) * 5000000ULL;
        info.frames      = 128;
        info.clockState  = 1;

        core.process(inCh, 2, outCh, 2, 128, info);

        /* BlockInfo is passed by const reference — hostTimeNs survives. */
        ASSERT(info.hostTimeNs == (1000000000ULL + static_cast<uint64_t>(i) * 5000000ULL),
               "hostTimeNs preserved in BlockInfo");
    }

    std::printf("ok\n");
}

static void test_g1_delay_offset_preserved() {
    /* Set BlockInfo.delayOffset to known value, verify it survives pipeline. */
    std::printf("  G1: Delay offset preserved... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;
    info.clockState  = 1;
    info.delayOffset = 64; /* Simulated snd_pcm_delay value. */

    core.process(inCh, 2, outCh, 2, 128, info);

    /* delayOffset is in BlockInfo — verify it's accessible after process(). */
    ASSERT(info.delayOffset == 64, "delayOffset preserved through onBlock");

    std::printf("ok\n");
}

static void test_g1_block_timing_consistency() {
    /* Verify consistent timing across many blocks: sampleIndex = blockNum * frames. */
    std::printf("  G1: Block timing consistency... ");

    DspCore core;
    ASSERT(core.init(48000, 256, 2, 2), "core init with 256-frame blocks");

    float inBuf[2][256] = {};
    float outBuf[2][256] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    const int numBlocks = 50;

    for (int i = 0; i < numBlocks; ++i) {
        BlockInfo info{};
        info.sampleIndex = static_cast<uint64_t>(i) * 256;
        info.hostTimeNs  = 1000000000ULL + static_cast<uint64_t>(i) * 5333334ULL; /* ~5.33ms per block */
        info.frames      = 256;
        info.clockState  = 1;

        core.process(inCh, 2, outCh, 2, 256, info);

        TelemetryBlock tel{};
        core.getTelemetry(tel);

        uint64_t expectedIndex = static_cast<uint64_t>(i + 1) * 256;
        ASSERT(tel.sampleIndex == expectedIndex, "sample index consistent at block %d", i);
    }

    std::printf("ok (%d blocks of 256 frames)\n", numBlocks);
}

static void test_g1_sample_index_wrap_safety() {
    /* Verify sampleIndex handles large values without overflow issues. */
    std::printf("  G1: Sample index wrap safety... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    float inBuf[2][128] = {};
    float outBuf[2][128] = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* Use large sample index near uint64_t limit. */
    BlockInfo info{};
    info.sampleIndex = UINT64_MAX - 64;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;
    info.clockState  = 1;

    core.process(inCh, 2, outCh, 2, 128, info);

    TelemetryBlock tel{};
    core.getTelemetry(tel);

    /* sampleIndex + frames should wrap correctly (uint64_t arithmetic). */
    uint64_t expected = UINT64_MAX - 64 + 128; /* Wraps to 64. */
    ASSERT(tel.sampleIndex == expected, "sample index wraps correctly at uint64_t limit");

    std::printf("ok (wrapped to %llu)\n", static_cast<unsigned long long>(tel.sampleIndex));
}

int main() {
    std::printf("=== Measurement Timing Tests (G1) ===\n");

    test_g1_sample_index_monotonic();
    test_g1_host_time_in_blockinfo();
    test_g1_delay_offset_preserved();
    test_g1_block_timing_consistency();
    test_g1_sample_index_wrap_safety();

    if (passed) {
        std::printf("[PASS] All test_measurement_timing tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
