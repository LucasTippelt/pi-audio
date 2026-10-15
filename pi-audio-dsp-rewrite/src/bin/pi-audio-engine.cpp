#include "hal/engine.h"
#include "hal/sim_hal.h"
#include "dsp_core/dsp_core.h"
#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"
#include "control/config_loader.h"
#include "control/state_writer.h"

#ifdef __linux__
#include "hal/alsa_hal.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

/* ------------------------------------------------------------------ */
/*  pi-audio-engine — New main: wires HAL + Engine + control plane    */
/* ------------------------------------------------------------------ */

static constexpr uint32_t kDefaultSampleRate = 48000;
static constexpr uint32_t kDefaultBufferSize = 128;
static constexpr uint32_t kDefaultNumInputs  = 2;
static constexpr uint32_t kDefaultNumOutputs = 2;

int main(int argc, char* argv[]) {
    /* Parse simple CLI args. */
    const char* configFile   = nullptr;
    const char* stateFile    = "/tmp/engine-state.json";
    bool useSimHal           = false;
    uint32_t sampleRate      = kDefaultSampleRate;
    uint32_t bufferSize      = kDefaultBufferSize;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configFile = argv[++i];
        } else if (std::strcmp(argv[i], "--state-file") == 0 && i + 1 < argc) {
            stateFile = argv[++i];
        } else if (std::strcmp(argv[i], "--sim") == 0) {
            useSimHal = true;
        } else if (std::strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            sampleRate = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--buffer") == 0 && i + 1 < argc) {
            bufferSize = static_cast<uint32_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: pi-audio-engine [options]\n");
            std::printf("  --config <path>   JSON config file\n");
            std::printf("  --state-file <p>  State output path\n");
            std::printf("  --sim             Use SimHal instead of AlsaHal\n");
            std::printf("  --rate <hz>       Sample rate (default %u)\n", kDefaultSampleRate);
            std::printf("  --buffer <frames> Buffer size (default %u)\n", kDefaultBufferSize);
            return 0;
        }
    }

    /* Check environment for SimHal override. */
    if (std::getenv("PI_AUDIO_SIM_HAL"))
        useSimHal = true;

    std::printf("[engine] Starting with rate=%u buffer=%u mode=%s\n",
                sampleRate, bufferSize, useSimHal ? "sim" : "alsa");

    /* --- Construct Engine and queues. --- */
    Engine engine;

    CmdQueue<CmdLoadParams, 16> cmdQueue;
    CmdQueue<TelemetryBlock, 8> telQueue;

    /* Register queues with Engine (type-erased). */
    engine.setCommandQueue(&cmdQueue);
    engine.setTelemetryQueue(&telQueue);

    /* Init DSP core. */
    if (!engine.core().init(sampleRate, bufferSize, kDefaultNumInputs, kDefaultNumOutputs)) {
        std::fprintf(stderr, "[engine] Failed to init DSP core\n");
        return 1;
    }

    /* --- Construct HAL. --- */
#ifdef __linux__
    AlsaHal* alsaHal = nullptr;
#endif
    SimHal* simHal   = nullptr;
    AudioHal* hal    = nullptr;

    if (useSimHal) {
        simHal = new SimHal();
        simHal->setInputSource(SIM_INPUT_SINE);
        simHal->setClockDriftPpm(30); /* +30 ppm drift injection */
        simHal->setRunDurationSeconds(5.0);
        hal = simHal;
    } else {
#ifdef __linux__
        alsaHal = new AlsaHal();
        hal = alsaHal;
#else
        std::fprintf(stderr, "[engine] ALSA not available on this platform. Use --sim.\n");
        return 1;
#endif
    }

    hal->setEngine(&engine);

    if (!hal->open(sampleRate, bufferSize, kDefaultNumInputs, kDefaultNumOutputs)) {
        std::fprintf(stderr, "[engine] Failed to open HAL\n");
        delete hal;
        return 1;
    }

    /* --- Control plane: load config. --- */
    if (configFile) {
        ConfigLoader loader;
        CmdLoadParams params;
        if (loader.load(configFile, params)) {
            cmdQueue.push(params);
            std::printf("[engine] Loaded config from %s\n", configFile);
        } else {
            std::fprintf(stderr, "[engine] Failed to load config from %s\n", configFile);
        }
    }

    /* --- State writer thread. --- */
    StateWriter stateWriter;
    stateWriter.start(stateFile, 1000);

    std::printf("[engine] Running audio loop...\n");

    /* --- Run HAL (blocks until HAL closes). --- */
    hal->run();

    /* --- Shutdown. --- */
    stateWriter.stop();
    hal->close();
    delete hal;

    std::printf("[engine] Shut down complete.\n");
    return 0;
}
