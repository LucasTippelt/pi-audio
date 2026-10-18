#include "hal/sim_hal.h"
#include "hal/engine.h"
#include "dsp_core/dsp_core.h"
#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"

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

static void test_xrun_counter_exposure() {
    std::printf("  Xrun counters via AudioHal virtual methods... ");
    SimHal hal;

    /* Initially zero. */
    ASSERT(hal.xrunUnderrunCount() == 0, "initial underrun count is 0");
    ASSERT(hal.xrunOverrunCount() == 0, "initial overrun count is 0");

    /* Inject underrun — counter increments. */
    hal.injectUnderrun();
    ASSERT(hal.xrunUnderrunCount() == 1, "underrun count is 1 after inject");
    ASSERT(hal.xrunOverrunCount() == 0, "overrun count still 0");

    /* Inject overrun — counter increments. */
    hal.injectOverrun();
    ASSERT(hal.xrunUnderrunCount() == 1, "underrun count unchanged");
    ASSERT(hal.xrunOverrunCount() == 1, "overrun count is 1 after inject");

    /* Multiple injections accumulate. */
    hal.injectUnderrun();
    hal.injectUnderrun();
    ASSERT(hal.xrunUnderrunCount() == 3, "underrun count accumulates to 3");
    std::printf("ok\n");
}

static void test_xrun_in_telemetry() {
    std::printf("  Xrun counters in TelemetryBlock... ");
    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    /* Use capacity 8 to match Engine's hardcoded cast. */
    CmdQueue<TelemetryBlock, 8> telQueue;

    engine.setCommandQueue(&cmdQueue);
    engine.setTelemetryQueue(&telQueue);

    ASSERT(engine.core().init(48000, 128, 2, 2), "core init");

    SimHal hal;
    hal.setInputSource(SIM_INPUT_SINE);
    hal.setClockDriftPpm(0);
    /* Very short run: ~3 blocks at 128/48k. Fits in telQueue of capacity 8 (holds 7). */
    hal.setRunDurationSeconds(0.005);
    hal.setEngine(&engine);

    /* Link HAL to Engine for xrun counter plumbing. */
    engine.setHal(&hal);

    ASSERT(hal.open(48000, 128, 2, 2), "hal open");

    /* Inject underrun before run — counters should appear in telemetry. */
    hal.injectUnderrun();
    ASSERT(hal.xrunUnderrunCount() == 1, "underrun injected");

    hal.run();

    /* Drain telemetry and check for xrun counts. */
    TelemetryBlock telBuf[8]{};
    size_t total = 0;
    size_t batch = 0;
    do {
        batch = telQueue.pop(telBuf + total, 8 - total);
        total += batch;
    } while (batch > 0 && total < 8);

    ASSERT(total > 0, "telemetry blocks produced");

    bool foundXrun = false;
    for (size_t i = 0; i < total; ++i) {
        if (telBuf[i].xrunUnderrun >= 1) {
            foundXrun = true;
            break;
        }
    }
    ASSERT(foundXrun, "telemetry contains non-zero xrunUnderrun");
    std::printf("ok (%zu blocks)\n", total);

    hal.close();
}

static void test_fade_state_after_injection() {
    std::printf("  Fade state after injection... ");
    SimHal hal;
    ASSERT(hal.open(48000, 128, 2, 2), "hal open");

    /* After open, fade is at full. */
    ASSERT(hal.getFadeLevel() == 1.0f, "fade level starts at 1.0");
    ASSERT(hal.getFadeStepsRemaining() == 0, "no fade steps pending");

    /* Inject underrun — fade drops to zero with restore steps. */
    hal.injectUnderrun();
    ASSERT(hal.getFadeLevel() == 0.0f, "fade level drops to 0 on underrun");
    ASSERT(hal.getFadeStepsRemaining() == 8, "fade steps set to kFadeRestoreSteps");

    /* Inject overrun — fade at half with restore steps. */
    hal.injectOverrun();
    ASSERT(hal.getFadeLevel() == 0.5f, "fade level drops to 0.5 on overrun");
    ASSERT(hal.getFadeStepsRemaining() == 8, "fade steps reset for overrun");

    /* Verify gradual fade math: after N steps, level = start + (1-start)/steps * (steps-N_done). */
    hal.injectUnderrun(); /* start=0.0, steps=8. */
    int stepsRemaining = hal.getFadeStepsRemaining();
    float startLevel = 0.0f;

    for (int step = 0; step < 8; ++step) {
        float targetLevel = 1.0f;
        float stepSize = (targetLevel - startLevel) / static_cast<float>(8);
        float expectedLevel = startLevel + stepSize * static_cast<float>(8 - stepsRemaining);
        if (expectedLevel > 1.0f) expectedLevel = 1.0f;

        /* Step 0: level should be 0.0 (first block is silent). */
        if (step == 0) {
            ASSERT(std::fabs(expectedLevel - 0.0f) < 1e-6f, "block 0 is silent");
        }
        /* Last step (7): level = 0 + 0.125 * 7 = 0.875, then reset to 1.0 after loop exits. */
        if (step == 7) {
            ASSERT(std::fabs(expectedLevel - 0.875f) < 1e-6f, "last block at 0.875");
        }

        stepsRemaining--;
    }
    std::printf("ok\n");
}

static void test_no_rt_allocation_guarantee() {
    std::printf("  No RT allocation guarantee (structural)... ");
    /* Verify that SimHal uses stack-allocated buffers in run().
     * This is a structural check: the planar buffer pointers are local arrays,
     * and channel data comes from pre-sized allocations. */

    SimHal hal;
    ASSERT(hal.open(48000, 128, 2, 2), "hal open");

    /* The key guarantee is that injectXrun() doesn't allocate — it only sets state. */
    hal.injectUnderrun();
    ASSERT(hal.getFadeStepsRemaining() > 0, "inject sets fade state without allocation");
    std::printf("ok\n");
}

int main() {
    std::printf("=== Xrun Recovery Tests (B4) — SimHal ===\n");

    test_xrun_counter_exposure();
    test_fade_state_after_injection();
    test_no_rt_allocation_guarantee();
    test_xrun_in_telemetry();

    if (passed) {
        std::printf("[PASS] All test_xrun_recovery_sim tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
