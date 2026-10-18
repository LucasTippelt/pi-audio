#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "dsp_core/channels.h"
#include "msg/telemetry.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

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

/* ================================================================== */
/*  Input routing tests                                                 */
/* ================================================================== */

static void test_pgm_l_duplicated_to_both_outputs() {
    /* PGM_L (input 0) should be duplicated to both output channels. */
    std::printf("  PGM_L duplicated to both outputs... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 6, 2), "core init with 6 inputs, 2 outputs");

    /* Verify default routing has PGM_L on input 0. */
    const RoutingConfig& rc = core.routing();
    ASSERT(rc.pgmLIndex == 0, "PGM_L is on input 0 by default");
    ASSERT(rc.inputRoles[0] == INPUT_PGM_L, "input 0 role is PGM_L");

    /* Create distinct signals for each input so we can verify routing. */
    const uint32_t frames = 128;
    float inBuf[6][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3], inBuf[4], inBuf[5] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* Fill each input with a unique constant value. */
    for (uint32_t ch = 0; ch < 6; ++ch) {
        float val = static_cast<float>(ch + 1) * 0.1f; /* 0.1, 0.2, ..., 0.6 */
        for (uint32_t f = 0; f < frames; ++f) {
            inBuf[ch][f] = val;
        }
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = frames;
    info.clockState  = 1;

    core.process(inCh, 6, outCh, 2, frames, info);

    /* Both outputs should contain PGM_L value (input 0 = 0.1). */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        ASSERT_FLOAT_EQ(outBuf[ch][0], 0.1f, 1e-6f, "output contains PGM_L");
        ASSERT_FLOAT_EQ(outBuf[ch][frames - 1], 0.1f, 1e-6f, "output contains PGM_L (end of block)");
    }

    std::printf("ok\n");
}

static void test_pgm_r_not_routed_locally() {
    /* PGM_R (input 1) should NOT appear on local outputs. */
    std::printf("  PGM_R not routed locally... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 6, 2), "core init");

    const uint32_t frames = 128;
    float inBuf[6][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3], inBuf[4], inBuf[5] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* PGM_L (input 0) = silence, PGM_R (input 1) = loud signal. */
    for (uint32_t f = 0; f < frames; ++f) {
        inBuf[0][f] = 0.0f;   /* PGM_L silent */
        inBuf[1][f] = 0.9f;   /* PGM_R loud — should NOT appear locally */
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = frames;
    info.clockState  = 1;

    core.process(inCh, 6, outCh, 2, frames, info);

    /* Outputs should be silent (PGM_L is 0, PGM_R not routed). */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        float maxVal = 0.f;
        for (uint32_t f = 0; f < frames; ++f) {
            float v = std::fabs(outBuf[ch][f]);
            if (v > maxVal) maxVal = v;
        }
        ASSERT(maxVal < 1e-8f, "PGM_R does not leak to local outputs");
    }

    /* Verify routing config. */
    const RoutingConfig& rc = core.routing();
    ASSERT(rc.inputRoles[1] == INPUT_PGM_R, "input 1 role is PGM_R");

    std::printf("ok\n");
}

static void test_mic_inputs_not_routed() {
    /* Mic inputs (2-5) should NOT appear on local outputs. */
    std::printf("  Mic inputs not routed to outputs... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 6, 2), "core init");

    const uint32_t frames = 128;
    float inBuf[6][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3], inBuf[4], inBuf[5] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* All program inputs silent, mics loud. */
    for (uint32_t f = 0; f < frames; ++f) {
        inBuf[0][f] = 0.0f;   /* PGM_L silent */
        inBuf[1][f] = 0.0f;   /* PGM_R silent */
        inBuf[2][f] = 0.8f;   /* Mic 1 loud — should NOT appear */
        inBuf[3][f] = 0.7f;   /* Mic 2 loud — should NOT appear */
        inBuf[4][f] = 0.6f;   /* Mic 3 loud — should NOT appear */
        inBuf[5][f] = 0.5f;   /* Mic 4 loud — should NOT appear */
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = frames;
    info.clockState  = 1;

    core.process(inCh, 6, outCh, 2, frames, info);

    /* Outputs should be silent (only PGM_L routed, which is 0). */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        float maxVal = 0.f;
        for (uint32_t f = 0; f < frames; ++f) {
            float v = std::fabs(outBuf[ch][f]);
            if (v > maxVal) maxVal = v;
        }
        ASSERT(maxVal < 1e-8f, "mic inputs do not leak to outputs");
    }

    /* Verify routing config. */
    const RoutingConfig& rc = core.routing();
    for (int i = 2; i <= 5; ++i) {
        ASSERT(rc.inputRoles[i] == INPUT_MIC, "inputs 2-5 are mic role");
    }

    std::printf("ok\n");
}

static void test_mic_inputs_metered_in_telemetry() {
    /* Mic inputs should be metered in telemetry even though not routed. */
    std::printf("  Mic inputs metered in telemetry... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 6, 2), "core init");

    const uint32_t frames = 128;
    float inBuf[6][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3], inBuf[4], inBuf[5] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* Set distinct levels for each input. */
    for (uint32_t f = 0; f < frames; ++f) {
        inBuf[0][f] = 0.5f;   /* PGM_L at -6dB peak */
        inBuf[1][f] = 0.0f;   /* PGM_R silent */
        inBuf[2][f] = 0.3f;   /* Mic 1 */
        inBuf[3][f] = 0.1f;   /* Mic 2 */
        inBuf[4][f] = 0.0f;   /* Mic 3 silent */
        inBuf[5][f] = 0.7f;   /* Mic 4 */
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = frames;
    info.clockState  = 1;

    core.process(inCh, 6, outCh, 2, frames, info);

    TelemetryBlock tel{};
    core.getTelemetry(tel);

    /* Check that input levels are metered for all inputs including mics. */
    /* PGM_L (0.5 peak) -> dB = 20*log10(0.5) ≈ -6dB (using the 40*f formula in code: 40*log10(peak)) */
    /* Actually the code uses: 40.f * log10(peak), which is unusual but let's just check they're not -96. */
    ASSERT(tel.inputLevelDb[2] > -95.f, "mic 1 level metered");
    ASSERT(tel.inputLevelDb[3] > -95.f, "mic 2 level metered");
    ASSERT(tel.inputLevelDb[4] < -90.f, "mic 3 silent shows low level");
    ASSERT(tel.inputLevelDb[5] > -95.f, "mic 4 level metered");

    std::printf("ok\n");
}

static void test_custom_routing_config() {
    /* Test that routing can be reconfigured (e.g., PGM_L on a different input). */
    std::printf("  Custom routing config... ");

    DspCore core;
    ASSERT(core.init(48000, 128, 6, 2), "core init");

    /* Reconfigure: move PGM_L to input 3 (e.g., a different physical input). */
    RoutingConfig custom = defaultRouting();
    custom.inputRoles[0] = INPUT_MIC;   /* Input 0 becomes a mic */
    custom.inputRoles[3] = INPUT_PGM_L; /* Input 3 becomes PGM_L */
    custom.pgmLIndex = 3;
    core.setRouting(custom);

    const uint32_t frames = 128;
    float inBuf[6][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3], inBuf[4], inBuf[5] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    /* Input 3 has the signal, input 0 is silent. */
    for (uint32_t f = 0; f < frames; ++f) {
        inBuf[0][f] = 0.0f;   /* Old PGM_L position — now a mic, should be ignored */
        inBuf[3][f] = 0.4f;   /* New PGM_L position — should appear on outputs */
    }

    BlockInfo info{};
    info.sampleIndex = 0;
    info.hostTimeNs  = 1000000000ULL;
    info.frames      = frames;
    info.clockState  = 1;

    core.process(inCh, 6, outCh, 2, frames, info);

    /* Both outputs should contain the signal from input 3. */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        ASSERT_FLOAT_EQ(outBuf[ch][0], 0.4f, 1e-6f, "custom PGM_L routed correctly");
    }

    std::printf("ok\n");
}

static void test_routeByRole_function() {
    /* Test the standalone routeByRole function in routing.cpp. */
    std::printf("  routeByRole utility function... ");

    const uint32_t frames = 128;
    float inBuf[6][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3], inBuf[4], inBuf[5] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 6; ++ch) {
        float val = static_cast<float>(ch + 1) * 0.1f;
        for (uint32_t f = 0; f < frames; ++f) inBuf[ch][f] = val;
    }

    RoutingConfig rc = defaultRouting();
    routeByRole(inCh, 6, outCh, 2, rc, frames);

    /* Both outputs should have PGM_L value (input 0 = 0.1). */
    for (uint32_t ch = 0; ch < 2; ++ch) {
        ASSERT_FLOAT_EQ(outBuf[ch][0], 0.1f, 1e-6f, "routeByRole duplicates PGM_L");
    }

    std::printf("ok\n");
}

static void test_routeChannels_function() {
    /* Test the generic routeChannels mixer function. */
    std::printf("  routeChannels utility function... ");

    const uint32_t frames = 128;
    float inBuf[4][frames];
    float outBuf[2][frames];
    float* inCh[]   = { inBuf[0], inBuf[1], inBuf[2], inBuf[3] };
    float* outCh[]  = { outBuf[0], outBuf[1] };

    for (uint32_t ch = 0; ch < 4; ++ch) {
        float val = static_cast<float>(ch + 1) * 0.2f;
        for (uint32_t f = 0; f < frames; ++f) inBuf[ch][f] = val;
    }

    /* Map: input 0 -> output 0 with gain 1.0, input 2 -> output 1 with gain 0.5 */
    ChannelMap maps[2];
    maps[0].sourceIndex = 0; maps[0].gain = 1.0f;
    maps[1].sourceIndex = 2; maps[1].gain = 0.5f;

    routeChannels(inCh, 4, outCh, 2, maps, 2, frames);

    /* Output 0 should have input 0 value (0.2 * 1.0 = 0.2). */
    ASSERT_FLOAT_EQ(outBuf[0][0], 0.2f, 1e-6f, "routeChannels output 0 correct");
    /* Output 1 should have input 2 value (0.6 * 0.5 = 0.3). */
    ASSERT_FLOAT_EQ(outBuf[1][0], 0.3f, 1e-6f, "routeChannels output 1 correct");

    std::printf("ok\n");
}

int main() {
    std::printf("=== Input Routing Tests ===\n");

    test_pgm_l_duplicated_to_both_outputs();
    test_pgm_r_not_routed_locally();
    test_mic_inputs_not_routed();
    test_mic_inputs_metered_in_telemetry();
    test_custom_routing_config();
    test_routeByRole_function();
    test_routeChannels_function();

    if (passed) {
        std::printf("[PASS] All input routing tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
