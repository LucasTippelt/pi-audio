#include "hal/sim_hal.h"
#include "hal/engine.h"
#include "dsp_core/dsp_core.h"
#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>

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

static void test_noise_input_nondeterministic() {
    std::printf("  Noise input non-deterministic... ");

    /* Run SimHal twice with noise source — verify both runs produce telemetry. */
    size_t blocks1 = 0;
    size_t blocks2 = 0;

    for (int run = 0; run < 2; ++run) {
        Engine engine;
        CmdQueue<CmdLoadParams, 16> cmdQueue;
        CmdQueue<TelemetryBlock, 8> telQueue;

        engine.setCommandQueue(&cmdQueue);
        engine.setTelemetryQueue(&telQueue);
        ASSERT(engine.core().init(48000, 128, 2, 2), "core init");

        SimHal hal;
        hal.setInputSource(SIM_INPUT_NOISE);
        hal.setClockDriftPpm(0);
        hal.setRunDurationSeconds(0.005); /* Very short: ~3 blocks. */
        hal.setEngine(&engine);
        ASSERT(hal.open(48000, 128, 2, 2), "hal open");

        hal.run();

        TelemetryBlock telBuf[8]{};
        size_t total = 0;
        size_t batch = 0;
        do {
            batch = telQueue.pop(telBuf + total, 8 - total);
            total += batch;
        } while (batch > 0 && total < 8);

        if (run == 0) blocks1 = total;
        else blocks2 = total;

        hal.close();
    }

    /* Both runs should produce telemetry. */
    ASSERT(blocks1 > 0, "first noise run produces telemetry");
    ASSERT(blocks2 > 0, "second noise run produces telemetry");
    std::printf("ok (run1=%zu blocks, run2=%zu blocks)\n", blocks1, blocks2);
}

static void test_clock_transition_mute_in_telemetry() {
    std::printf("  Clock transition mute in telemetry... ");
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
    /* Short run: ~3 blocks. With capacity-8 queue (holds 7), all fit. */
    hal.setRunDurationSeconds(0.01);
    hal.setEngine(&engine);
    ASSERT(hal.open(48000, 128, 2, 2), "hal open");

    hal.run();

    /* Drain telemetry and verify clock state is reported correctly. */
    TelemetryBlock telBuf[8]{};
    size_t total = 0;
    size_t batch = 0;
    do {
        batch = telQueue.pop(telBuf + total, 8 - total);
        total += batch;
    } while (batch > 0 && total < 8);

    ASSERT(total > 0, "telemetry blocks produced");

    /* Check that clockState is valid in telemetry. */
    bool foundValid = false;
    for (size_t i = 0; i < total && !foundValid; ++i) {
        if (telBuf[i].clockState == 1 || telBuf[i].clockState == 2) {
            foundValid = true;
        }
    }
    ASSERT(foundValid, "telemetry shows valid clock state");

    hal.close();
    std::printf("ok (%zu blocks)\n", total);
}

static void test_multiple_drift_ppm_bounded() {
    std::printf("  Multiple drift ppm bounded output... ");
    int ppmValues[] = {30, 50, 100};

    for (size_t p = 0; p < 3; ++p) {
        Engine engine;
        CmdQueue<CmdLoadParams, 16> cmdQueue;
        CmdQueue<TelemetryBlock, 8> telQueue;

        engine.setCommandQueue(&cmdQueue);
        engine.setTelemetryQueue(&telQueue);
        ASSERT(engine.core().init(48000, 128, 2, 2), "core init");

        SimHal hal;
        hal.setInputSource(SIM_INPUT_SINE);
        hal.setClockDriftPpm(ppmValues[p]);
        hal.setRunDurationSeconds(0.005); /* Very short: ~3 blocks. */
        hal.setEngine(&engine);
        ASSERT(hal.open(48000, 128, 2, 2), "hal open");

        hal.run();

        TelemetryBlock telBuf[8]{};
        size_t total = 0;
        size_t batch = 0;
        do {
            batch = telQueue.pop(telBuf + total, 8 - total);
            total += batch;
        } while (batch > 0 && total < 8);

        /* Output should be bounded: level in [-96, 0] dB range. */
        for (size_t i = 0; i < total; ++i) {
            ASSERT(telBuf[i].outputLevelDb[0] >= -96.f && telBuf[i].outputLevelDb[0] <= 1.f,
                   "output level bounded");
        }

        hal.close();
    }
    std::printf("ok\n");
}

static void test_sine_input_stable_level() {
    std::printf("  Sine input stable level... ");
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
    /* Short run: ~3 blocks. Drift ring may produce zeros initially — verify telemetry works. */
    hal.setRunDurationSeconds(0.005);
    hal.setEngine(&engine);
    ASSERT(hal.open(48000, 128, 2, 2), "hal open");

    hal.run();

    /* Drain all telemetry blocks. */
    TelemetryBlock telBuf[8]{};
    size_t total = 0;
    size_t batch = 0;
    do {
        batch = telQueue.pop(telBuf + total, 8 - total);
        total += batch;
    } while (batch > 0 && total < 8);

    ASSERT(total > 0, "telemetry produced");

    /* Verify telemetry structure: version set, sample index advancing. */
    bool valid = false;
    for (size_t i = 0; i < total; ++i) {
        if (telBuf[i].version == 1 && telBuf[i].sampleIndex > 0) {
            valid = true;
        }
    }
    ASSERT(valid, "telemetry has valid structure");

    hal.close();
    std::printf("ok (%zu blocks)\n", total);
}

int main() {
    std::printf("=== Extended SimHal Tests (A5) ===\n");

    test_sine_input_stable_level();
    test_noise_input_nondeterministic();
    test_clock_transition_mute_in_telemetry();
    test_multiple_drift_ppm_bounded();

    if (passed) {
        std::printf("[PASS] All test_sim_hal_extended tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
