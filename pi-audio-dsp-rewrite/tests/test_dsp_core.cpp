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

int main() {
    DspCore core;

    /* Test init. */
    ASSERT(core.init(48000, 128, 2, 2), "core init");

    /* Allocate planar buffers. */
    float inBuf[2][128]   = {};
    float outBuf[2][128]  = {};
    float* inCh[]   = { inBuf[0], inBuf[1] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* Generate sine input. */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            inBuf[ch][f] = std::sin(2.0 * M_PI * 440.0 * f / 48000.0);
        }
    }

    /* Process block. */
    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = 128;
    info.clockState  = 1;

    core.process(inCh, 2, outCh, 2, 128, info);

    /* Verify output matches input (no stages added yet). */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t f = 0; f < 128; ++f) {
            ASSERT_FLOAT_EQ(inBuf[ch][f], outBuf[ch][f], 1e-5f, "output matches input");
        }
    }

    /* Test telemetry. */
    TelemetryBlock tel{};
    core.getTelemetry(tel);
    ASSERT(tel.version == 1, "telemetry version");
    ASSERT(tel.sampleIndex == 128, "telemetry sample index");

    /* Test multiple blocks with advancing sample index. */
    info.sampleIndex = 128;
    core.process(inCh, 2, outCh, 2, 128, info);
    core.getTelemetry(tel);
    ASSERT(tel.sampleIndex == 256, "telemetry advances with blocks");

    /* Test total latency (should be 0 with no stages). */
    ASSERT(core.totalLatencySamples() == 0, "total latency is zero");

    if (passed) {
        std::printf("[PASS] All test_dsp_core tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
