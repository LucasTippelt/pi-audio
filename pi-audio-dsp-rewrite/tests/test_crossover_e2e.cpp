#include "hal/sim_hal.h"
#include "hal/engine.h"
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "dsp_core/stage.h"
#include "stages/crossover_stage.h"
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
/*  End-to-End Crossover Validation (Phase 5)                          */
/*  Full pipeline: JSON -> ConfigLoader -> Engine -> CrossoverStage    */
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
            std::fprintf(stderr, "[FAIL] %s: got %.6f expected %.6f (line %d)\n", \
                         msg, static_cast<double>(a), static_cast<double>(b), __LINE__); \
            g_passed = false;                                                    \
        }                                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
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

static float measureRms(const float* buf, uint32_t frames) {
    double sumSq = 0.0;
    for (uint32_t i = 0; i < frames; ++i)
        sumSq += static_cast<double>(buf[i]) * buf[i];
    return static_cast<float>(std::sqrt(sumSq / frames));
}

static void generateSine(float* buf, uint32_t frames, double freq, double sampleRate) {
    for (uint32_t i = 0; i < frames; ++i)
        buf[i] = static_cast<float>(std::sin(2.0 * M_PI * freq * i / sampleRate));
}

/* Simple DFT magnitude at a specific bin. */
static float dftMagnitude(const float* buf, uint32_t frames, uint32_t bin, double sampleRate) {
    double re = 0.0, im = 0.0;
    double freq = static_cast<double>(bin) * sampleRate / frames;
    for (uint32_t i = 0; i < frames; ++i) {
        double angle = 2.0 * M_PI * bin * i / frames;
        re += buf[i] * std::cos(angle);
        im -= buf[i] * std::sin(angle);
    }
    return static_cast<float>(std::sqrt(re * re + im * im) / frames);
}

/* Run engine for given number of blocks with sine input. */
static void runBlocks(Engine* engine, float* inBuf, float* outW, float* outT,
                      uint32_t frames, uint32_t numBlocks, double freqHz) {
    BlockInfo info{};
    info.frames = frames;
    for (uint32_t b = 0; b < numBlocks; ++b) {
        generateSine(inBuf, frames, freqHz, 48000.0);
        float* inputs[] = {inBuf, inBuf};
        float* outputs[] = {outW, outT};
        engine->onBlock(inputs, 2, outputs, 2, info);
    }
}

/* ------------------------------------------------------------------ */
/*  Test: full_pipeline_lr24_2500hz                                   */
/* ------------------------------------------------------------------ */
static void testFullPipelineLr24() {
    std::printf("Test: full_pipeline_lr24_2500hz ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    CmdQueue<TelemetryBlock, 8> telQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.setTelemetryQueue(&telQueue);
    engine.core().init(48000, 512, 2, 2);

    /* Load LR24 at 2500Hz. */
    const char* path = "/tmp/e2e_lr24.json";
    writeJsonFile(path, makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true));
    ConfigLoader loader;
    CmdLoadParams cmd{};
    ASSERT(loader.load(path, cmd), "load lr24 config");
    cmdQueue.push(cmd);
    std::remove(path);

    constexpr uint32_t frames = 512;
    float inBuf[frames] = {}, outW[frames] = {}, outT[frames] = {};

    /* Test lowpass: 500Hz should pass, 8000Hz should be attenuated. */
    runBlocks(&engine, inBuf, outW, outT, frames, 4, 500.0);
    float rmsWooferLow = measureRms(outW, frames);

    ASSERT(engine.isCrossoverActive(), "lr24: crossover should be active");

    runBlocks(&engine, inBuf, outW, outT, frames, 2, 8000.0);
    float rmsWooferHigh = measureRms(outW, frames);

    ASSERT(rmsWooferLow > 0.3f, "lr24: woofer passes low freq");
    ASSERT(rmsWooferHigh < rmsWooferLow * 0.1f, "lr24: woofer attenuates high freq");

    /* Test highpass: 500Hz should be attenuated, 8000Hz should pass. */
    runBlocks(&engine, inBuf, outW, outT, frames, 2, 500.0);
    float rmsTweeterLow = measureRms(outT, frames);

    runBlocks(&engine, inBuf, outW, outT, frames, 4, 8000.0);
    float rmsTweeterHigh = measureRms(outT, frames);

    ASSERT(rmsTweeterLow < rmsTweeterHigh * 0.1f, "lr24: tweeter attenuates low freq");
    ASSERT(rmsTweeterHigh > 0.3f, "lr24: tweeter passes high freq");

    /* Drain telemetry. */
    TelemetryBlock telBuf[8]{};
    while (telQueue.pop(telBuf, 8) > 0) {}

    std::printf("PASS (w_low=%.4f w_high=%.4f t_low=%.4f t_high=%.4f)\n",
               static_cast<double>(rmsWooferLow), static_cast<double>(rmsWooferHigh),
               static_cast<double>(rmsTweeterLow), static_cast<double>(rmsTweeterHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: full_pipeline_bw12_3000hz                                   */
/* ------------------------------------------------------------------ */
static void testFullPipelineBw12() {
    std::printf("Test: full_pipeline_bw12_3000hz ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.core().init(48000, 512, 2, 2);

    const char* path = "/tmp/e2e_bw12.json";
    writeJsonFile(path, makeCrossoverJson(3000.f, "Butterworth", 12, true));
    ConfigLoader loader;
    CmdLoadParams cmd{};
    ASSERT(loader.load(path, cmd), "load bw12 config");
    cmdQueue.push(cmd);
    std::remove(path);

    constexpr uint32_t frames = 512;
    float inBuf[frames] = {}, outW[frames] = {}, outT[frames] = {};

    /* BW12: single biquad, gentler rolloff. */
    runBlocks(&engine, inBuf, outW, outT, frames, 4, 1000.0);
    float rmsWLow = measureRms(outW, frames);

    runBlocks(&engine, inBuf, outW, outT, frames, 2, 10000.0);
    float rmsWHigh = measureRms(outW, frames);

    ASSERT(rmsWLow > 0.3f, "bw12: woofer passes low freq");
    ASSERT(rmsWHigh < rmsWLow * 0.3f, "bw12: woofer attenuates high freq (gentle rolloff)");

    /* Verify latency: BW12 = 1 biquad -> 2 samples. */
    ASSERT(engine.crossoverWoofer().latencySamples() == 2, "bw12: latency = 2 samples");
    ASSERT(engine.crossoverTweeter().latencySamples() == 2, "bw12: tweeter latency = 2 samples");

    std::printf("PASS (w_low=%.4f w_high=%.4f)\n",
               static_cast<double>(rmsWLow), static_cast<double>(rmsWHigh));
}

/* ------------------------------------------------------------------ */
/*  Test: full_pipeline_bessel36_2000hz                               */
/* ------------------------------------------------------------------ */
static void testFullPipelineBessel36() {
    std::printf("Test: full_pipeline_bessel36_2000hz ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.core().init(48000, 512, 2, 2);

    const char* path = "/tmp/e2e_bessel36.json";
    writeJsonFile(path, makeCrossoverJson(2000.f, "Bessel", 36, true));
    ConfigLoader loader;
    CmdLoadParams cmd{};
    ASSERT(loader.load(path, cmd), "load bessel36 config");
    cmdQueue.push(cmd);
    std::remove(path);

    constexpr uint32_t frames = 512;
    float inBuf[frames] = {}, outW[frames] = {}, outT[frames] = {};

    /* Bessel36: first-order + 2 biquads. Steep rolloff, flat group delay. */
    runBlocks(&engine, inBuf, outW, outT, frames, 4, 500.0);
    float rmsWLow = measureRms(outW, frames);

    runBlocks(&engine, inBuf, outW, outT, frames, 2, 10000.0);
    float rmsWHigh = measureRms(outW, frames);

    ASSERT(rmsWLow > 0.3f, "bessel36: woofer passes low freq");
    ASSERT(rmsWHigh < rmsWLow * 0.05f, "bessel36: woofer strongly attenuates high freq");

    /* Verify latency: Bessel36 = 2 biquads + first-order -> 5 samples. */
    uint32_t lat = engine.crossoverWoofer().latencySamples();
    ASSERT(lat >= 4 && lat <= 6, "bessel36: latency in expected range");

    std::printf("PASS (w_low=%.4f w_high=%.4f lat=%u)\n",
               static_cast<double>(rmsWLow), static_cast<double>(rmsWHigh), lat);
}

/* ------------------------------------------------------------------ */
/*  Test: test_mode_woofer_with_crossover                             */
/* ------------------------------------------------------------------ */
static void testTestModeWooferCrossover() {
    std::printf("Test: test_mode_woofer_with_crossover ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    CmdQueue<CmdSetTestMode, 16> testQueue;
    CmdQueue<TelemetryBlock, 8> telQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.setTestCommandQueue(&testQueue);
    engine.setTelemetryQueue(&telQueue);
    engine.core().init(48000, 512, 2, 2);

    /* Load LR24 crossover. */
    const char* path = "/tmp/e2e_testmode_w.json";
    writeJsonFile(path, makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true));
    ConfigLoader loader;
    CmdLoadParams lcmd{};
    ASSERT(loader.load(path, lcmd), "load config");
    cmdQueue.push(lcmd);
    std::remove(path);

    /* Enable test mode 1 (woofer noise). */
    CmdSetTestMode tcmd{};
    tcmd.header.type = CMD_SET_TEST_MODE;
    tcmd.mode = 1;
    testQueue.push(tcmd);

    constexpr uint32_t frames = 512;
    float inBuf[frames] = {}, outW[frames] = {}, outT[frames] = {};

    /* Run blocks — noise should flow through crossover chain. */
    BlockInfo info{};
    info.frames = frames;
    for (int i = 0; i < 8; ++i) {
        float* inputs[] = {inBuf, inBuf};
        float* outputs[] = {outW, outT};
        engine.onBlock(inputs, 2, outputs, 2, info);
    }

    /* Woofer output should have reduced HF content due to crossover. */
    float rmsW = measureRms(outW, frames);
    ASSERT(rmsW > 0.f && rmsW < 0.3f, "testmode_woofer: output has energy but attenuated by LP");

    std::printf("PASS (rms_w=%.4f)\n", static_cast<double>(rmsW));
}

/* ------------------------------------------------------------------ */
/*  Test: test_mode_tweeter_with_crossover                            */
/* ------------------------------------------------------------------ */
static void testTestModeTweeterCrossover() {
    std::printf("Test: test_mode_tweeter_with_crossover ... ");

    Engine engine;
    CmdQueue<CmdLoadParams, 16> cmdQueue;
    CmdQueue<CmdSetTestMode, 16> testQueue;
    CmdQueue<TelemetryBlock, 8> telQueue;
    engine.setCommandQueue(&cmdQueue);
    engine.setTestCommandQueue(&testQueue);
    engine.setTelemetryQueue(&telQueue);
    engine.core().init(48000, 512, 2, 2);

    const char* path = "/tmp/e2e_testmode_t.json";
    writeJsonFile(path, makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true));
    ConfigLoader loader;
    CmdLoadParams lcmd{};
    ASSERT(loader.load(path, lcmd), "load config");
    cmdQueue.push(lcmd);
    std::remove(path);

    /* Enable test mode 2 (tweeter noise). */
    CmdSetTestMode tcmd{};
    tcmd.header.type = CMD_SET_TEST_MODE;
    tcmd.mode = 2;
    testQueue.push(tcmd);

    constexpr uint32_t frames = 512;
    float inBuf[frames] = {}, outW[frames] = {}, outT[frames] = {};

    BlockInfo info{};
    info.frames = frames;
    for (int i = 0; i < 8; ++i) {
        float* inputs[] = {inBuf, inBuf};
        float* outputs[] = {outW, outT};
        engine.onBlock(inputs, 2, outputs, 2, info);
    }

    /* Tweeter output should have reduced LF content due to HP crossover. */
    float rmsT = measureRms(outT, frames);
    ASSERT(rmsT > 0.f && rmsT < 0.3f, "testmode_tweeter: output has energy but attenuated by HP");

    std::printf("PASS (rms_t=%.4f)\n", static_cast<double>(rmsT));
}

/* ------------------------------------------------------------------ */
/*  Test: config_roundtrip_web_to_dsp                                 */
/* ------------------------------------------------------------------ */
static void testConfigRoundtrip() {
    std::printf("Test: config_roundtrip_web_to_dsp ... ");

    /* Simulate web schema defaults: LR24 at 2500Hz. */
    const char* path = "/tmp/e2e_roundtrip.json";
    writeJsonFile(path, makeCrossoverJson(2500.f, "Linkwitz-Riley", 24, true));

    ConfigLoader loader;
    CmdLoadParams cmd{};
    ASSERT(loader.load(path, cmd), "roundtrip: load succeeded");
    std::remove(path);

    /* Verify binary block structure. */
    ASSERT(cmd.header.type == CMD_LOAD_PARAMS, "roundtrip: correct command type");
    ASSERT(cmd.payloadSize > sizeof(ParamBlockHeader), "roundtrip: payload has entries");

    const ParamBlockHeader* hdr = reinterpret_cast<const ParamBlockHeader*>(cmd.payload);
    ASSERT(hdr->stageCount >= 2, "roundtrip: at least 2 stage entries (woofer+tweeter)");

    /* Parse entries and verify crossover coefficients. */
    const uint8_t* p = cmd.payload + sizeof(ParamBlockHeader);
    int wooferFound = 0, tweeterFound = 0;
    for (uint32_t i = 0; i < hdr->stageCount; ++i) {
        uint16_t type, slot;
        std::memcpy(&type, p, 2); p += 2;
        std::memcpy(&slot, p, 2); p += 2;
        uint32_t dataLen = 0;
        std::memcpy(&dataLen, p, 4); p += 4;

        if (type == STAGE_PARAM_CROSSOVER) {
            uint8_t numBq = p[2];
            if (slot == 0) {
                wooferFound = 1;
                ASSERT(numBq == 2, "roundtrip: woofer has 2 biquads for LR24");
                /* Verify LP coefficients: b0 should be small positive. */
                float b0; std::memcpy(&b0, &p[10], sizeof(float));
                ASSERT(b0 > 0.f && b0 < 0.1f, "roundtrip: woofer b0 in expected range");
            } else if (slot == 1) {
                tweeterFound = 1;
                ASSERT(numBq == 2, "roundtrip: tweeter has 2 biquads for LR24");
                /* Verify HP coefficients: b0 should be larger. */
                float b0; std::memcpy(&b0, &p[10], sizeof(float));
                ASSERT(b0 > 0.5f && b0 < 1.f, "roundtrip: tweeter b0 in expected range");
            }
        }
        p += dataLen;
    }

    ASSERT(wooferFound && tweeterFound, "roundtrip: both woofer and tweeter entries found");

    std::printf("PASS\n");
}

/* ------------------------------------------------------------------ */
/*  Test: direct crossover stage (bypass Engine/DspCore)               */
/* ------------------------------------------------------------------ */
static void testDirectCrossoverStage() {
    std::printf("Test: direct_crossover_stage ... ");

    /* Compute LR24 LP coefficients at 2500Hz. */
    float coeffs[20] = {};
    uint32_t numBq = 0;
    bool hasFO = false;
    float foCoef = 0.f;

    bool ok = ConfigLoader::precomputeCrossoverCoeffs(
        2500.f, 48000, ConfigLoader::FILTER_LOWPASS, ConfigLoader::TOPO_LR, 24,
        coeffs, &numBq, &hasFO, &foCoef);
    ASSERT(ok, "direct: precompute succeeded");
    ASSERT(numBq == 2, "direct: LR24 has 2 biquads");

    /* Create a CrossoverStage and configure it. */
    CrossoverStage stage;
    stage.configure(coeffs, static_cast<uint8_t>(numBq), hasFO, foCoef);
    ASSERT(stage.latencySamples() == numBq * 2, "direct: latency matches biquads");

    /* Generate 500Hz sine (should pass through LP). */
    constexpr uint32_t frames = 512;
    float bufLow[frames] = {};
    generateSine(bufLow, frames, 500.0, 48000.0);
    float rmsInLow = measureRms(bufLow, frames);

    /* Generate 8000Hz sine (should be attenuated by LP). */
    float bufHigh[frames] = {};
    generateSine(bufHigh, frames, 8000.0, 48000.0);
    float rmsInHigh = measureRms(bufHigh, frames);

    /* Process through CrossoverStage directly. */
    BlockInfo info{};
    info.frames = frames;

    float outLow[frames] = {};
    std::memcpy(outLow, bufLow, frames * sizeof(float));
    float* channelsLow[] = {outLow};
    stage.process(channelsLow, 1, frames, info);
    float rmsOutLow = measureRms(outLow, frames);

    float outHigh[frames] = {};
    std::memcpy(outHigh, bufHigh, frames * sizeof(float));
    float* channelsHigh[] = {outHigh};
    stage.process(channelsHigh, 1, frames, info);
    float rmsOutHigh = measureRms(outHigh, frames);

    ASSERT(rmsInLow > 0.3f, "direct: input low has energy");
    ASSERT(rmsInHigh > 0.3f, "direct: input high has energy");
    ASSERT(rmsOutLow > 0.3f, "direct: LP passes low freq");
    /* Single-block transient: expect some attenuation but not steady-state. */
    ASSERT(rmsOutHigh < rmsInHigh * 0.5f, "direct: LP attenuates high freq (single block)");

    std::printf("PASS (numBq=%u inLow=%.4f outLow=%.4f inHigh=%.4f outHigh=%.4f)\n",
               static_cast<unsigned>(numBq),
               static_cast<double>(rmsInLow), static_cast<double>(rmsOutLow),
               static_cast<double>(rmsInHigh), static_cast<double>(rmsOutHigh));
}

/* ------------------------------------------------------------------ */
int main() {
    std::printf("=== End-to-End Crossover Validation (Phase 5) ===\n\n");

    testDirectCrossoverStage();
    testFullPipelineLr24();
    testFullPipelineBw12();
    testFullPipelineBessel36();
    testTestModeWooferCrossover();
    testTestModeTweeterCrossover();
    testConfigRoundtrip();

    if (g_passed) {
        std::printf("\n[PASS] All E2E validation tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some E2E tests failed.\n");
        return 1;
    }
}
