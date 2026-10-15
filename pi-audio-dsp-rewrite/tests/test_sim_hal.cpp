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

int main() {
    /* Build full pipeline: SimHal -> Engine -> Core. */
    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    CmdQueue<TelemetryBlock, 8> telQueue;

    engine.setCommandQueue(&cmdQueue);
    engine.setTelemetryQueue(&telQueue);

    /* Init core. */
    ASSERT(engine.core().init(48000, 128, 2, 2), "core init");

    /* Configure SimHal. */
    SimHal hal;
    hal.setInputSource(SIM_INPUT_SINE);
    hal.setClockDriftPpm(30);
    hal.setRunDurationSeconds(0.5); /* Short run for test. */
    hal.setEngine(&engine);

    ASSERT(hal.open(48000, 128, 2, 2), "hal open");

    /* Push a gain command before running. */
    CmdLoadParams cmd{};
    cmd.header.version      = 1;
    cmd.header.type         = CMD_LOAD_PARAMS;
    cmd.header.seq          = 1;
    cmd.paramVersion        = 1;
    cmd.payloadSize         = sizeof(float);
    float gain = 0.75f;
    std::memcpy(cmd.payload, &gain, sizeof(float));
    cmdQueue.push(cmd);

    /* Run HAL (blocks until duration expires). */
    hal.run();

    /* Check telemetry was produced. */
    TelemetryBlock tel{};
    size_t n = telQueue.pop(&tel, 1);
    ASSERT(n == 1, "telemetry available after run");
    ASSERT(tel.version == 1, "telemetry version correct");
    ASSERT(tel.sampleIndex > 0, "sample index advanced");

    /* Drain remaining telemetry. */
    TelemetryBlock telBuf[8]{};
    while (telQueue.pop(telBuf, 8) > 0) {}

    hal.close();

    if (passed) {
        std::printf("[PASS] All test_sim_hal tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
