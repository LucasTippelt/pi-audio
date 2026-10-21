#include "hal/engine.h"
#include "hal/sim_hal.h"
#include "dsp_core/dsp_core.h"
#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"
#include "control/config_loader.h"
#include "control/state_writer.h"
#include "stages/gain_stage.h"

#ifdef __linux__
#include "hal/alsa_hal.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <sys/stat.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ */
/*  pi-audio-engine — New main: wires HAL + Engine + control plane    */
/* ------------------------------------------------------------------ */

static constexpr uint32_t kDefaultSampleRate = 48000;

/* Global pointer for HAL reopen callback (avoids lambda capture issue). */
static StateWriter* g_stateWriter = nullptr;
static void onHalReopen(uint32_t sampleRate, uint32_t bufferSize) {
    if (g_stateWriter) {
        g_stateWriter->setAudioConfig(sampleRate, bufferSize);
    }
}
static constexpr uint32_t kDefaultBufferSize = 128;
static constexpr uint32_t kDefaultNumInputs  = 4;
static constexpr uint32_t kDefaultNumOutputs = 2;

int main(int argc, char* argv[]) {
    /* Parse simple CLI args. */
    const char* configFile   = nullptr;
    const char* stateFile    = "/run/pi-audio/engine-state.json";
    bool useSimHal           = false;
    uint32_t sampleRate      = kDefaultSampleRate;
    uint32_t bufferSize      = kDefaultBufferSize;
    uint32_t coreMask        = 0;
    const char* captureDev   = nullptr;
    const char* playbackDev  = nullptr;
    const char* rcInputDev   = nullptr;
    const char* rcOutputDev  = nullptr;

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
        } else if (std::strcmp(argv[i], "--core-mask") == 0 && i + 1 < argc) {
            coreMask = static_cast<uint32_t>(std::strtol(argv[++i], nullptr, 16));
        } else if (std::strcmp(argv[i], "--capture-dev") == 0 && i + 1 < argc) {
            captureDev = argv[++i];
        } else if (std::strcmp(argv[i], "--playback-dev") == 0 && i + 1 < argc) {
            playbackDev = argv[++i];
        } else if (std::strcmp(argv[i], "--rc-input-dev") == 0 && i + 1 < argc) {
            rcInputDev = argv[++i];
        } else if (std::strcmp(argv[i], "--rc-output-dev") == 0 && i + 1 < argc) {
            rcOutputDev = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("Usage: pi-audio-engine [options]\n");
            std::printf("  --config <path>   JSON config file\n");
            std::printf("  --state-file <p>  State output path\n");
            std::printf("  --sim             Use SimHal instead of AlsaHal\n");
            std::printf("  --rate <hz>       Sample rate (default %u)\n", kDefaultSampleRate);
            std::printf("  --buffer <frames> Buffer size (default %u)\n", kDefaultBufferSize);
            std::printf("  --core-mask <hex> CPU affinity mask for RT thread\n");
            std::printf("  --capture-dev <d> ALSA capture device (default: from config or 'default')\n");
            std::printf("  --playback-dev <d> ALSA playback device (default: from config or 'default')\n");
            std::printf("  --rc-input-dev <d>  ALSA capture device for room correction mics\n");
            std::printf("  --rc-output-dev <d> ALSA playback device for mic output (loopback)\n");
            return 0;
        } else if (argv[i][0] != '-' && configFile == nullptr) {
            /* Backward-compatible positional arg: first non-flag = config file. */
            configFile = argv[i];
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
    CmdQueue<CmdSetTestMode, 16> testCmdQueue;
    CmdQueue<CmdGeneric, 32> ctrlCmdQueue;

    /* Register queues with Engine (type-erased). */
    engine.setCommandQueue(&cmdQueue);
    engine.setTelemetryQueue(&telQueue);
    engine.setTestCommandQueue(&testCmdQueue);
    engine.setControlCommandQueue(&ctrlCmdQueue);

    /* Init DSP core. */
    if (!engine.core().init(sampleRate, bufferSize, kDefaultNumInputs, kDefaultNumOutputs)) {
        std::fprintf(stderr, "[engine] Failed to init DSP core\n");
        return 1;
    }

    /* --- Create single master GainStage (applied once to ALL outputs after chains). --- */
    GainStage* masterGain = new GainStage();
    engine.core().setMasterGain(masterGain);
    engine.setGainStages(masterGain, masterGain);

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
        if (coreMask)
            alsaHal->setCoreMask(coreMask);
        hal = alsaHal;
#else
        std::fprintf(stderr, "[engine] ALSA not available on this platform. Use --sim.\n");
        return 1;
#endif
    }

    /* --- Extract device names from config and apply to HAL. --- */
    char captureDevBuf[128] = "default";
    char playbackDevBuf[128] = "default";
    char rcInputDevBuf[128] = "";
    char rcOutputDevBuf[128] = "";

#ifdef __linux__
    if (alsaHal && configFile) {
        FILE* f = std::fopen(configFile, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 65536) {
                char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(sz) + 1));
                if (buf) {
                    (void)std::fread(buf, 1, static_cast<size_t>(sz), f);
                    buf[sz] = '\0';
                    ConfigLoader::parseDeviceNames(buf, static_cast<size_t>(sz), captureDevBuf, playbackDevBuf);
                    ConfigLoader::parseRcDeviceNames(buf, static_cast<size_t>(sz), rcInputDevBuf, rcOutputDevBuf);
                    std::free(buf);
                }
            }
            std::fclose(f);
        }
    }

    /* CLI args override config file values. */
    const char* finalCapture = captureDev ? captureDev : captureDevBuf;
    const char* finalPlayback = playbackDev ? playbackDev : playbackDevBuf;
    const char* finalRcInput = rcInputDev ? rcInputDev : (rcInputDevBuf[0] != '\0' ? rcInputDevBuf : nullptr);
    const char* finalRcOutput = rcOutputDev ? rcOutputDev : (rcOutputDevBuf[0] != '\0' ? rcOutputDevBuf : nullptr);
    if (alsaHal) {
        alsaHal->setCaptureDevice(finalCapture);
        alsaHal->setPlaybackDevice(finalPlayback);
        if (finalRcInput) alsaHal->setRcInputDevice(finalRcInput);
        if (finalRcOutput) alsaHal->setRcOutputDevice(finalRcOutput);
    }
#endif

    hal->setEngine(&engine);

    /* B4: Give Engine access to HAL for xrun counter telemetry. */
    engine.setHal(hal);

    if (!hal->open(sampleRate, bufferSize, kDefaultNumInputs, kDefaultNumOutputs)) {
        std::fprintf(stderr, "[engine] Failed to open HAL\n");
        delete hal;
        return 1;
    }

    /* Re-init DSP core with actual negotiated channel counts.
     * Hardware may negotiate fewer channels than requested (e.g. UR12 has 2 inputs, not 4). */
    {
        uint32_t actualInputs  = hal->actualNumInputs();
        uint32_t actualOutputs = hal->actualNumOutputs();
        if (actualInputs != kDefaultNumInputs || actualOutputs != kDefaultNumOutputs) {
            std::printf("[engine] Adjusting channels: %u/%u -> %u/%u\n",
                        kDefaultNumInputs, kDefaultNumOutputs, actualInputs, actualOutputs);
            engine.core().init(sampleRate, bufferSize, actualInputs, actualOutputs);
        }
    }

    /* --- Create /run/pi-audio/ directory and sentinel for status adapter. --- */
    {
        mkdir("/run/pi-audio", 0777);
        FILE* sentinel = std::fopen("/run/pi-audio/status", "w");
        if (sentinel) {
            std::fprintf(sentinel, "running\n");
            std::fclose(sentinel);
        }
    }

    /* --- State writer thread. --- */
    StateWriter stateWriter;
    stateWriter.setAudioConfig(sampleRate, bufferSize);

    /* Seed initial applied-state for reporting (do NOT auto-apply config).
     * Read volume/mute from current.json once so StateWriter has something to report.
     * Config is only applied when "Apply" button triggers /run/pi-audio/reload. */
    {
        char capBuf[128] = "";
        char pbBuf[128] = "";
        int initVolume = -1;
        bool initMuted = false;

        if (configFile) {
            FILE* f = std::fopen(configFile, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 65536) {
                    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(sz) + 1));
                    if (buf) {
                        (void)std::fread(buf, 1, static_cast<size_t>(sz), f);
                        buf[sz] = '\0';
                        ConfigLoader::parseDeviceNames(buf, static_cast<size_t>(sz), capBuf, pbBuf);
                        initVolume = ConfigLoader::extractVolumePercent(buf, static_cast<size_t>(sz));
                        bool gotMuted = ConfigLoader::extractMuted(buf, static_cast<size_t>(sz), &initMuted);
                        if (!gotMuted) initMuted = false;
                        std::free(buf);
                    }
                }
                std::fclose(f);
            }
        }

        /* Use device names from config for seeding (already used above for HAL). */
        const char* seedInput = capBuf[0] ? capBuf : finalCapture;
        const char* seedOutput = pbBuf[0] ? pbBuf : finalPlayback;

        stateWriter.setAppliedDevices(seedInput, seedOutput);
        if (finalRcInput || finalRcOutput) {
            stateWriter.setAppliedRcDevices(
                finalRcInput ? finalRcInput : "",
                finalRcOutput ? finalRcOutput : "");
        }
        if (initVolume >= 0) stateWriter.setAppliedVolume(initVolume);
        stateWriter.setAppliedMuted(initMuted);

        /* Seed initial DSP applied-state from config. */
        {
            FullConfig cfg;
            FILE* f2 = std::fopen(configFile, "r");
            if (f2) {
                fseek(f2, 0, SEEK_END);
                long sz2 = ftell(f2);
                fseek(f2, 0, SEEK_SET);
                if (sz2 > 0 && sz2 < 65536) {
                    char* buf2 = static_cast<char*>(std::malloc(static_cast<size_t>(sz2) + 1));
                    if (buf2) {
                        (void)std::fread(buf2, 1, static_cast<size_t>(sz2), f2);
                        buf2[sz2] = '\0';
                        if (ConfigLoader::parseFullConfig(buf2, static_cast<size_t>(sz2), cfg)) {
                            AppliedCrossover cxW = {};
                            cxW.frequencyHz = cfg.crossoverWoofer.frequencyHz;
                            std::strncpy(cxW.topology, cfg.crossoverWoofer.topologyName, sizeof(cxW.topology) - 1);
                            cxW.slopeDbOct = cfg.crossoverWoofer.slopeDbOct;
                            cxW.enabled = cfg.crossoverWoofer.enabled;
                            stateWriter.setAppliedCrossover(0, cxW);

                            AppliedCrossover cxT = {};
                            cxT.frequencyHz = cfg.crossoverTweeter.frequencyHz;
                            std::strncpy(cxT.topology, cfg.crossoverTweeter.topologyName, sizeof(cxT.topology) - 1);
                            cxT.slopeDbOct = cfg.crossoverTweeter.slopeDbOct;
                            cxT.enabled = cfg.crossoverTweeter.enabled;
                            stateWriter.setAppliedCrossover(1, cxT);

                            {
                                AppliedLimiter lim = {};
                                lim.thresholdDb = cfg.limiterThresholdDb;
                                lim.attackMs = cfg.limiterAttackMs;
                                lim.releaseMs = cfg.limiterReleaseMs;
                                lim.ratio = cfg.limiterRatio;
                                lim.enabled = cfg.limiterEnabled;
                                stateWriter.setAppliedLimiter(0, lim);
                                stateWriter.setAppliedLimiter(1, lim);
                            }

                            for (uint32_t i = 0; i < cfg.driverCount && i < kMaxDrivers; ++i) {
                                uint32_t delaySamples = ConfigLoader::delayMsToSamples(
                                    cfg.drivers[i].delayMs, cfg.sampleRate);
                                stateWriter.setAppliedDriver(
                                    cfg.drivers[i].name, delaySamples,
                                    cfg.drivers[i].trimDb, cfg.drivers[i].phaseInvert != 0);
                            }
                        }
                        std::free(buf2);
                    }
                }
                std::fclose(f2);
            }
        }
    }

#ifdef __linux__
    /* Set up callback so HAL notifies StateWriter after a successful runtime reopen. */
    g_stateWriter = &stateWriter;
    if (alsaHal) {
        alsaHal->setReopenCallback(onHalReopen);
    }
#endif

    stateWriter.start(stateFile, 200);

    /* C5: Telemetry consumer thread — drains telQueue and feeds StateWriter. */
    std::atomic<bool> telConsumerRunning{true};
    std::thread telConsumer([&telQueue, &stateWriter, &telConsumerRunning]() {
        TelemetryBlock tel{};
        while (telConsumerRunning.load()) {
            size_t n = telQueue.pop(&tel, 1);
            if (n > 0) {
                stateWriter.ingestTelemetry(tel);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });

    std::printf("[engine] Running audio loop...\n");

    /* --- Test control thread: watches /run/pi-audio/test-command file.
     * Reads JSON {driver, mode, duration}, sets test mode, sleeps for
     * duration, then clears test mode (auto-stop). */
    std::atomic<bool> testCtrlRunning{true};
    std::thread testCtrlThread([&]() {
        while (testCtrlRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            struct stat st;
            if (stat("/run/pi-audio/test-command", &st) != 0) continue;

            /* Read command file. */
            FILE* f = std::fopen("/run/pi-audio/test-command", "r");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > 1024) { std::fclose(f); continue; }

            char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(sz) + 1));
            if (!buf) { std::fclose(f); continue; }
            (void)std::fread(buf, 1, static_cast<size_t>(sz), f);
            buf[sz] = '\0';
            std::fclose(f);

            /* Parse mode and duration from JSON (minimal parser). */
            int mode = 0;
            double durationSec = 3.0;

            /* Extract "mode": N */
            const char* modeKey = std::strstr(buf, "\"mode\"");
            if (modeKey) {
                const char* colon = std::strchr(modeKey, ':');
                if (colon) mode = static_cast<int>(std::strtol(colon + 1, nullptr, 10));
            }

            /* Extract "duration": N */
            const char* durKey = std::strstr(buf, "\"duration\"");
            if (durKey) {
                const char* colon = std::strchr(durKey, ':');
                if (colon) durationSec = std::strtod(colon + 1, nullptr);
            }

            /* Clamp duration: minimum 0.5s, maximum 60s. */
            if (durationSec < 0.5) durationSec = 0.5;
            if (durationSec > 60.0) durationSec = 60.0;

            std::free(buf);

            /* Remove trigger file immediately so web server can send next command. */
            std::remove("/run/pi-audio/test-command");

            if (mode < 0 || mode > 9) continue;

            /* Push test mode start command. */
            if (mode >= 4 && mode <= 9) {
                /* Mic test: set RC test mode on HAL (maps to channel 0-5). */
                uint8_t rcMode = static_cast<uint8_t>(mode - 3); /* 4→1, 5→2, etc. */
                if (alsaHal) alsaHal->setRcTestMode(rcMode);
            } else {
                CmdSetTestMode startCmd{};
                startCmd.header.version = 1;
                startCmd.header.type = CMD_SET_TEST_MODE;
                startCmd.mode = static_cast<uint8_t>(mode);
                testCmdQueue.push(startCmd);
            }

#ifdef PI_AUDIO_DIAG_LOG
            std::printf("[engine] Test mode %d started, duration %.1fs\n", mode, durationSec);
            { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
              if (df) { std::fprintf(df, "[test-ctrl] mode=%d duration=%.1fs\n", mode, durationSec); std::fclose(df); } }
#endif

            /* Sleep for the requested duration. */
            {
                auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::duration<double>(durationSec));
                while (testCtrlRunning.load() &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    /* Check for early stop command. */
                    struct stat stopSt;
                    if (stat("/run/pi-audio/test-stop", &stopSt) == 0) {
                        std::remove("/run/pi-audio/test-stop");
                        break;
                    }
                }
            }

            /* Push test mode stop command. */
            if (mode >= 4 && mode <= 9) {
                /* Clear RC test mode for mic tests. */
                if (alsaHal) alsaHal->setRcTestMode(0);
            } else {
                CmdSetTestMode stopCmd{};
                stopCmd.header.version = 1;
                stopCmd.header.type = CMD_SET_TEST_MODE;
                stopCmd.mode = 0;
                testCmdQueue.push(stopCmd);
            }

#ifdef PI_AUDIO_DIAG_LOG
            std::printf("[engine] Test mode stopped\n");
#endif
        }
    });

    /* --- Reload control thread (AlsaHal only). --- */
#ifdef __linux__
    std::atomic<bool> reloadRunning{true};
    std::thread reloadThread([&]() {
        char curCapture[128];
        char curPlayback[128];
        char curRcInput[128] = "";
        char curRcOutput[128] = "";
        std::strncpy(curCapture, finalCapture, sizeof(curCapture) - 1);
        curCapture[sizeof(curCapture) - 1] = '\0';
        std::strncpy(curPlayback, finalPlayback, sizeof(curPlayback) - 1);
        curPlayback[sizeof(curPlayback) - 1] = '\0';
        if (finalRcInput) {
            std::strncpy(curRcInput, finalRcInput, sizeof(curRcInput) - 1);
            curRcInput[sizeof(curRcInput) - 1] = '\0';
        }
        if (finalRcOutput) {
            std::strncpy(curRcOutput, finalRcOutput, sizeof(curRcOutput) - 1);
            curRcOutput[sizeof(curRcOutput) - 1] = '\0';
        }

        /* Track current DSP format for reopen detection. */
        uint32_t curSampleRate = sampleRate;
        uint32_t curBufferSize = bufferSize;

        /* Track last volume/mute to avoid redundant commands. */
        float lastGain = 1.0f;
        bool lastMuted = false;
        bool hadVolume = false;
        bool hadMuted = false;

        while (reloadRunning.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            struct stat st;
            if (stat("/run/pi-audio/reload", &st) != 0) continue;

            /* Read config file for device names and DSP params. */
            char newCapture[128];
            char newPlayback[128];
            char newRcInput[128] = "";
            char newRcOutput[128] = "";
            uint32_t newSampleRate = 0;
            uint32_t newBufferSize = 0;

            FILE* f = std::fopen(configFile, "r");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 65536) {
                    char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(sz) + 1));
                    if (buf) {
                        (void)std::fread(buf, 1, static_cast<size_t>(sz), f);
                        buf[sz] = '\0';
                        ConfigLoader::parseDeviceNames(buf, static_cast<size_t>(sz), newCapture, newPlayback);
                        ConfigLoader::parseRcDeviceNames(buf, static_cast<size_t>(sz), newRcInput, newRcOutput);

                        /* Extract sample rate and buffer size for reopen detection. */
                        newSampleRate = ConfigLoader::extractSampleRate(buf, static_cast<size_t>(sz));
                        newBufferSize = ConfigLoader::extractBufferSize(buf, static_cast<size_t>(sz));

                        /* Push DSP params to command queue. */
                        {
                            ConfigLoader loader;
                            CmdLoadParams params;
                            if (loader.load(configFile, params)) {
                                cmdQueue.push(params);
                            }
                        }

                        /* Report applied DSP state to StateWriter from parsed config. */
                        {
                            FullConfig cfg;
                            if (ConfigLoader::parseFullConfig(buf, static_cast<size_t>(sz), cfg)) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));

                                AppliedCrossover cxW = {
                                    cfg.crossoverWoofer.frequencyHz,
                                    {},
                                    cfg.crossoverWoofer.slopeDbOct,
                                    cfg.crossoverWoofer.enabled
                                };
                                std::strncpy(cxW.topology, cfg.crossoverWoofer.topologyName, sizeof(cxW.topology) - 1);
                                stateWriter.setAppliedCrossover(0, cxW);

                                AppliedCrossover cxT = {
                                    cfg.crossoverTweeter.frequencyHz,
                                    {},
                                    cfg.crossoverTweeter.slopeDbOct,
                                    cfg.crossoverTweeter.enabled
                                };
                                std::strncpy(cxT.topology, cfg.crossoverTweeter.topologyName, sizeof(cxT.topology) - 1);
                                stateWriter.setAppliedCrossover(1, cxT);

                                {
                                    AppliedLimiter lim = {
                                        cfg.limiterThresholdDb, cfg.limiterAttackMs,
                                        cfg.limiterReleaseMs, cfg.limiterRatio, cfg.limiterEnabled
                                    };
                                    stateWriter.setAppliedLimiter(0, lim);
                                    stateWriter.setAppliedLimiter(1, lim);
                                }

                                for (uint32_t i = 0; i < cfg.driverCount && i < kMaxDrivers; ++i) {
                                    uint32_t delaySamples = ConfigLoader::delayMsToSamples(
                                        cfg.drivers[i].delayMs, cfg.sampleRate);
                                    stateWriter.setAppliedDriver(
                                        cfg.drivers[i].name, delaySamples,
                                        cfg.drivers[i].trimDb, cfg.drivers[i].phaseInvert != 0);
                                }
                            }
                        }

                        /* Extract volume/mute from config and push control commands. */
                        {
                            float newGain = 1.0f;
                            bool newMuted = false;
                            bool gotVolume = ConfigLoader::extractVolumeGain(buf, static_cast<size_t>(sz), &newGain);
                            bool gotMuted = ConfigLoader::extractMuted(buf, static_cast<size_t>(sz), &newMuted);

#ifdef PI_AUDIO_DIAG_LOG
                            /* Diagnostic log. */
                            {
                                FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                                if (df) {
                                    std::fprintf(df, "[reload] gotVolume=%d gain=%.4f gotMuted=%d muted=%d hadV=%d hadM=%d lastG=%.4f lastM=%d\n",
                                                  gotVolume, newGain, gotMuted, newMuted, hadVolume, hadMuted, lastGain, lastMuted);
                                    std::fclose(df);
                                }
                            }
#endif

                            if (gotVolume && (!hadVolume || newGain != lastGain)) {
                                CmdGeneric cmd;
                                std::memset(&cmd, 0, sizeof(cmd));
                                cmd.header.version = 1;
                                cmd.header.type = CMD_SET_GAIN;
                                cmd.gain = newGain;
                                ctrlCmdQueue.push(cmd);
                                lastGain = newGain;
                                hadVolume = true;
                            }
                            if (gotMuted && (!hadMuted || newMuted != lastMuted)) {
                                CmdGeneric cmd;
                                std::memset(&cmd, 0, sizeof(cmd));
                                cmd.header.version = 1;
                                cmd.header.type = CMD_SET_MUTE;
                                cmd.muted = static_cast<uint8_t>(newMuted ? 1 : 0);
                                ctrlCmdQueue.push(cmd);
                                lastMuted = newMuted;
                                hadMuted = true;
                            }

                            /* Report applied state to StateWriter (with short delay for DSP settling). */
                            {
                                int volPct = ConfigLoader::extractVolumePercent(buf, static_cast<size_t>(sz));
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                if (volPct >= 0) stateWriter.setAppliedVolume(volPct);
                                if (gotMuted) stateWriter.setAppliedMuted(newMuted);
                            }
                        }

                        std::free(buf);
                    }
                }
                std::fclose(f);
            }

            /* If devices changed AND explicitly set in config (non-empty), signal HAL reopen. */
            if (alsaHal && newCapture[0] != '\0' && newPlayback[0] != '\0' &&
                (std::strcmp(newCapture, curCapture) != 0 || std::strcmp(newPlayback, curPlayback) != 0)) {
                alsaHal->setCaptureDevice(newCapture);
                alsaHal->setPlaybackDevice(newPlayback);
                std::strncpy(curCapture, newCapture, sizeof(curCapture) - 1);
                std::strncpy(curPlayback, newPlayback, sizeof(curPlayback) - 1);
                /* Report device change to StateWriter. */
                stateWriter.setAppliedDevices(newCapture, newPlayback);
                /* Force HAL to close old streams and reopen with new device names.
                 * Passing (0, 0) keeps current sample rate and buffer size. */
                alsaHal->requestReopen(0, 0);
            }

            /* If RC devices changed, signal HAL reopen. */
            if (alsaHal &&
                (std::strcmp(newRcInput, curRcInput) != 0 || std::strcmp(newRcOutput, curRcOutput) != 0)) {
                const char* ri = newRcInput[0] != '\0' ? newRcInput : "";
                const char* ro = newRcOutput[0] != '\0' ? newRcOutput : "";
                alsaHal->setRcInputDevice(ri);
                alsaHal->setRcOutputDevice(ro);
                stateWriter.setAppliedRcDevices(ri, ro);
                std::strncpy(curRcInput, newRcInput, sizeof(curRcInput) - 1);
                curRcInput[sizeof(curRcInput) - 1] = '\0';
                std::strncpy(curRcOutput, newRcOutput, sizeof(curRcOutput) - 1);
                curRcOutput[sizeof(curRcOutput) - 1] = '\0';
                alsaHal->requestReopen(0, 0);
            }

            /* If sample rate or buffer size changed, signal HAL reopen with new params.
             * StateWriter will be updated by the HAL callback after reopen completes. */
#ifdef PI_AUDIO_DIAG_LOG
            {
                FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                if (df) {
                    std::fprintf(df, "[reload] sr=%u/%u bs=%u/%u\n",
                                 (unsigned)newSampleRate, (unsigned)curSampleRate,
                                 (unsigned)newBufferSize, (unsigned)curBufferSize);
                    std::fclose(df);
                }
            }
#endif
            if (alsaHal && (newSampleRate != curSampleRate || newBufferSize != curBufferSize)) {
                uint32_t sr = newSampleRate ? newSampleRate : curSampleRate;
                uint32_t bs = newBufferSize ? newBufferSize : curBufferSize;
#ifdef PI_AUDIO_DIAG_LOG
                { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                  if (df) { std::fprintf(df, "[reload] REOPEN requested %u/%u\n", (unsigned)sr, (unsigned)bs); std::fclose(df); } }
#endif
                alsaHal->requestReopen(sr, bs);
                if (newSampleRate) curSampleRate = newSampleRate;
                if (newBufferSize) curBufferSize = newBufferSize;
            }

            std::remove("/run/pi-audio/reload");
        }
    });
#endif

    /* --- Run HAL (blocks until HAL closes). --- */
    hal->run();

    /* --- Shutdown. --- */
#ifdef __linux__
    reloadRunning.store(false);
    if (reloadThread.joinable())
        reloadThread.join();
#endif
    testCtrlRunning.store(false);
    if (testCtrlThread.joinable())
        testCtrlThread.join();
    telConsumerRunning.store(false);
    if (telConsumer.joinable())
        telConsumer.join();
    stateWriter.stop();
    hal->close();
    delete hal;

    /* Clean up master GainStage. */
    delete masterGain;

    /* Remove sentinel file so status adapter reports disconnected. */
    std::remove("/run/pi-audio/status");

    std::printf("[engine] Shut down complete.\n");
    return 0;
}
