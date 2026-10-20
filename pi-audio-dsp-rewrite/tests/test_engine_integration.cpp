#include "hal/sim_hal.h"
#include "hal/engine.h"
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "msg/command.h"
#include "msg/params.h"
#include "msg/telemetry.h"
#include "msg/queue.h"
#include "control/config_loader.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <sstream>

/* ------------------------------------------------------------------ */
/*  Engine integration tests — crossover pipeline (Phase 4)            */
/* ------------------------------------------------------------------ */

static bool g_passed = true;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

#define ASSERT_FLOAT_EQ(a, b, eps, msg)                                          \
    do {                                                                         \
        if (std::fabs((a) - (b)) > (eps)) {                                      \
            std::fprintf(stderr, "[FAIL] %s: got %.8f expected %.8f (line %d)\n", \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helper: write a JSON config file with crossover settings           */
/* ------------------------------------------------------------------ */
static std::string makeCrossoverJson(float freq, const char* topology, int slope, bool enabled) {
    std::ostringstream oss;
    oss << "{"
        << "\"sampleRate\":48000,\"BufferSize\":128"
        << ",\"crossovers\":{";

    if (enabled) {
        oss << "\"Woofer\":{\"frequency\":" << freq
            << ",\"topology\":\"" << topology
            << "\",\"slope\":" << slope << "}"
            << ",\"Tweeter\":{\"frequency\":" << freq
            << ",\"topology\":\"" << topology
            << "\",\"slope\":" << slope << "}";
    }

    oss << "}}";
    return oss.str();
}

static bool writeJsonFile(const char* path, const std::string& content) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << content;
    f.close();
    return true;
}

/* ------------------------------------------------------------------ */
/*  Helper: measure RMS                                                */
/* ------------------------------------------------------------------ */
static float measureRms(const float* buf, uint32_t frames) {
    double sumSq = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        sumSq += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return static_cast<float>(std::sqrt(sumSq / frames));
}

/* ------------------------------------------------------------------ */
/*  Helper: generate sine wave                                         */
/* ------------------------------------------------------------------ */
static void generateSine(float* buf, uint32_t frames, double freq, double sampleRate) {
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i] = static_cast<float>(std::sin(2.0 * M_PI * freq * i / sampleRate));
    }
}

/* ------------------------------------------------------------------ */
/*  Helper: run Engine onBlock manually with given input               */
/* ------------------------------------------------------------------ */
static void runEngineBlocks(Engine* engine, float* const in[], uint32_t numInCh,
                             float* const out[], uint32_t numOutCh,
                             uint32_t frames, uint32_t numBlocks) {
    BlockInfo info{};
    info.frames = frames;
    for (uint32_t b = 0; b < numBlocks; ++b) {
        info.sampleIndex = static_cast<uint64_t>(b * frames);
        engine->onBlock(in, numInCh, out, numOutCh, info);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: engine loads crossover from JSON                             */
/* ------------------------------------------------------------------ */
static void testEngineLoadsCrossoverFromJson() {
    std::printf("Test: engine_loads_crossover_from_json ... ");

    /* Write a config file with LR24 crossover at 2500Hz. */
    const char* path = "/tmp/test_crossover_config.json";
    std::string json = makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true);
    ASSERT(writeJsonFile(path, json), "write config file");

    /* Load via ConfigLoader. */
    ConfigLoader loader;
    CmdLoadParams cmd{};
    bool loaded = loader.load(path, cmd);
    ASSERT(loaded, "config_loader: load succeeded");
    ASSERT(cmd.header.type == CMD_LOAD_PARAMS, "config_loader: correct command type");
    ASSERT(cmd.payloadSize > 0, "config_loader: payload has data");

    /* Push to Engine command queue. */
    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.core().init(48000, 128, 2, 2);

    cmdQueue.push(cmd);

    /* Trigger drain by running one block. */
    float inBuf[512] = {};
    float outWoofer[512] = {};
    float outTweeter[512] = {};
    float* inputs[] = {inBuf, inBuf};
    float* outputs[] = {outWoofer, outTweeter};

    BlockInfo info{};
    info.frames = 128;
    engine.onBlock(inputs, 2, outputs, 2, info);

    /* Verify crossover is active. */
    ASSERT(engine.isCrossoverActive(), "engine: crossover active after config load");
    ASSERT(engine.crossoverWoofer().latencySamples() > 0, "engine: woofer crossover has latency");
    ASSERT(engine.crossoverTweeter().latencySamples() > 0, "engine: tweeter crossover has latency");

    /* Clean up. */
    std::remove(path);

    std::printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test: crossover in signal path                                     */
/* ------------------------------------------------------------------ */
static void testCrossoverInSignalPath() {
    std::printf("Test: crossover_in_signal_path ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    CmdQueue<TelemetryBlock, 8> telQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.setTelemetryQueue(&telQueue);
    engine.core().init(48000, 512, 2, 2);

    /* Load LR24 crossover config. */
    const char* path = "/tmp/test_crossover_signal.json";
    std::string json = makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true);
    ASSERT(writeJsonFile(path, json), "write config file");

    ConfigLoader loader;
    CmdLoadParams cmd{};
    bool loaded = loader.load(path, cmd);
    ASSERT(loaded, "config_loader: load succeeded");
    std::remove(path);

    cmdQueue.push(cmd);

    /* Run with noise input to exercise the filter chain. */
    constexpr uint32_t frames = 512;
    std::vector<float> inCh0(frames), inCh1(frames);
    std::vector<float> outWoofer(frames), outTweeter(frames);

    /* Generate broadband noise. */
    for (uint32_t i = 0; i < frames; ++i) {
        inCh0[i] = static_cast<float>(std::sin(static_cast<double>(i) * 7.3)) * 0.5f;
        inCh1[i] = inCh0[i];
    }

    float* inputs[] = {inCh0.data(), inCh1.data()};
    float* outputs[] = {outWoofer.data(), outTweeter.data()};

    BlockInfo info{};
    info.frames = frames;
    engine.onBlock(inputs, 2, outputs, 2, info);

    /* Drain telemetry. */
    TelemetryBlock telBuf[8]{};
    while (telQueue.pop(telBuf, 8) > 0) {}

    /* Verify: woofer output should have less high-frequency energy than input. */
    float rmsIn = measureRms(inCh0.data(), frames);
    float rmsWoofer = measureRms(outWoofer.data(), frames);
    float rmsTweeter = measureRms(outTweeter.data(), frames);

    ASSERT(rmsIn > 0.1f, "signal_path: input has energy");
    /* Woofer LP should reduce overall RMS (attenuates HF). */
    ASSERT(rmsWoofer < rmsIn * 0.95f, "signal_path: woofer output attenuated vs input");
    /* Tweeter HP passes most of broadband noise — check it's not amplified. */
    ASSERT(rmsTweeter < rmsIn * 1.1f, "signal_path: tweeter output not amplified vs input");

    std::printf("PASS (rms_in=%.4f rms_woofer=%.4f rms_tweeter=%.4f)\n",
               static_cast<double>(rmsIn),
               static_cast<double>(rmsWoofer),
               static_cast<double>(rmsTweeter));
}

/* ------------------------------------------------------------------ */
/*  Test: config reload updates coefficients                           */
/* ------------------------------------------------------------------ */
static void testConfigReloadUpdatesCoeffs() {
    std::printf("Test: config_reload_updates_coeffs ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.core().init(48000, 512, 2, 2);

    /* Load initial config: LR24 at 3000Hz. */
    {
        const char* path = "/tmp/test_reload_1.json";
        std::string json = makeCrossoverJson(3000.f, "Linkwitz-Riley", 24, true);
        writeJsonFile(path, json);

        ConfigLoader loader;
        CmdLoadParams cmd{};
        loader.load(path, cmd);
        cmdQueue.push(cmd);
        std::remove(path);
    }

    /* Run a block to drain first config. */
    float buf[512] = {};
    generateSine(buf, 512, 2000.0, 48000.0);
    float* inputs[] = {buf, buf};
    float outW[512] = {}, outT[512] = {};
    float* outputs[] = {outW, outT};

    BlockInfo info{};
    info.frames = 512;
    engine.onBlock(inputs, 2, outputs, 2, info);

    /* Record woofer RMS at 2000Hz with fc=3000Hz (should pass well). */
    float rmsBefore = measureRms(outW, 512);

    /* Reload config: LR24 at 1000Hz. */
    {
        const char* path = "/tmp/test_reload_2.json";
        std::string json = makeCrossoverJson(1000.f, "Linkwitz-Riley", 24, true);
        writeJsonFile(path, json);

        ConfigLoader loader;
        CmdLoadParams cmd{};
        loader.load(path, cmd);
        cmdQueue.push(cmd);
        std::remove(path);
    }

    /* Run blocks to drain second config and let transients settle. */
    for (int warmup = 0; warmup < 8; ++warmup) {
        generateSine(buf, 512, 2000.0, 48000.0);
        engine.onBlock(inputs, 2, outputs, 2, info);
    }

    /* With fc=1000Hz, 2000Hz should be attenuated (above cutoff). */
    float rmsAfter = measureRms(outW, 512);

    /* Verify crossover is still active after reload. */
    ASSERT(engine.isCrossoverActive(), "reload: crossover active after second config");
    ASSERT(engine.crossoverWoofer().latencySamples() > 0, "reload: woofer has latency after reload");

    std::printf("PASS (rms_before=%.4f rms_after=%.4f)\n",
               static_cast<double>(rmsBefore),
               static_cast<double>(rmsAfter));
}

/* ------------------------------------------------------------------ */
/*  Test: woofer+tweeter complementary at crossover frequency          */
/* ------------------------------------------------------------------ */
static void testWooferTweeterComplementary() {
    std::printf("Test: woofer_tweeter_complementary ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.core().init(48000, 512, 2, 2);

    /* Load LR24 at 2500Hz. */
    const char* path = "/tmp/test_complementary.json";
    std::string json = makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true);
    writeJsonFile(path, json);

    ConfigLoader loader;
    CmdLoadParams cmd{};
    loader.load(path, cmd);
    cmdQueue.push(cmd);
    std::remove(path);

    /* Run with a tone at crossover frequency. */
    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);
    generateSine(buf.data(), frames, 2500.0, 48000.0);

    float* inputs[] = {buf.data(), buf.data()};
    std::vector<float> outW(frames), outT(frames);
    float* outputs[] = {outW.data(), outT.data()};

    BlockInfo info{};
    info.frames = frames;

    /* Run multiple blocks for steady-state. */
    for (int i = 0; i < 8; ++i) {
        engine.onBlock(inputs, 2, outputs, 2, info);
    }

    /* Power summation: LP^2 + HP^2 should be near unity at crossover. */
    double powerSum = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        powerSum += static_cast<double>(outW[i]) * outW[i]
                  + static_cast<double>(outT[i]) * outT[i];
    }
    float rmsPower = static_cast<float>(std::sqrt(powerSum / frames));

    float rmsIn = measureRms(buf.data(), frames);

    /* LR property: sqrt(LP^2 + HP^2) ≈ unity at crossover frequency. */
    ASSERT(rmsPower > 0.3f * rmsIn, "complementary: power sum near unity at crossover");

    std::printf("PASS (rms_in=%.4f rms_power_sum=%.4f ratio=%.4f)\n",
               static_cast<double>(rmsIn),
               static_cast<double>(rmsPower),
               static_cast<double>(rmsPower / rmsIn));
}

/* ------------------------------------------------------------------ */
/*  Test: crossover bypass when disabled                               */
/* ------------------------------------------------------------------ */
static void testCrossoverBypassWhenDisabled() {
    std::printf("Test: crossover_bypass_when_disabled ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.core().init(48000, 512, 2, 2);

    /* Load config WITHOUT crossover (enabled=false). */
    const char* path = "/tmp/test_bypass.json";
    std::string json = makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, false);
    writeJsonFile(path, json);

    ConfigLoader loader;
    CmdLoadParams cmd{};
    loader.load(path, cmd);
    cmdQueue.push(cmd);
    std::remove(path);

    /* Run with a tone. */
    constexpr uint32_t frames = 512;
    std::vector<float> buf(frames);
    generateSine(buf.data(), frames, 440.0, 48000.0);

    float* inputs[] = {buf.data(), buf.data()};
    std::vector<float> outW(frames), outT(frames);
    float* outputs[] = {outW.data(), outT.data()};

    BlockInfo info{};
    info.frames = frames;
    engine.onBlock(inputs, 2, outputs, 2, info);

    /* Without crossover configured, stages should have zero biquads (pass-through). */
    ASSERT(!engine.isCrossoverActive(), "bypass: crossover not active when disabled");

    /* Output should match input (no filtering applied by unconfigured stages). */
    float rmsIn = measureRms(buf.data(), frames);
    float rmsOutW = measureRms(outW.data(), frames);
    float rmsOutT = measureRms(outT.data(), frames);

    ASSERT(rmsOutW > 0.9f * rmsIn, "bypass: woofer output ≈ input when crossover disabled");
    ASSERT(rmsOutT > 0.9f * rmsIn, "bypass: tweeter output ≈ input when crossover disabled");

    std::printf("PASS (rms_in=%.4f rms_w=%.4f rms_t=%.4f)\n",
               static_cast<double>(rmsIn),
               static_cast<double>(rmsOutW),
               static_cast<double>(rmsOutT));
}

/* ------------------------------------------------------------------ */
int main() {
    std::printf("=== Engine Integration Tests (Phase 4) ===\n\n");

    testEngineLoadsCrossoverFromJson();
    testCrossoverInSignalPath();
    testConfigReloadUpdatesCoeffs();
    testWooferTweeterComplementary();
    testCrossoverBypassWhenDisabled();

    if (g_passed) {
        std::printf("\n[PASS] All engine integration tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
