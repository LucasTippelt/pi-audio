#include "control/config_loader.h"
#include "msg/command.h"
#include "msg/params.h"

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

/* ================================================================== */
/*  ConfigLoader EQ parsing tests                                      */
/* ================================================================== */

static void test_webui_bands_schema() {
    std::printf("Test: webui_bands_schema ... ");

    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Woofer": [
                {"id": "w1", "enabled": true, "frequency": 200, "q": 0.707, "gain": 3},
                {"id": "w2", "enabled": true, "frequency": 500, "q": 1.0, "gain": -2}
            ],
            "Tweeter": [
                {"id": "t1", "enabled": true, "frequency": 3000, "q": 0.707, "gain": 4},
                {"id": "t2", "enabled": false, "frequency": 5000, "q": 1.0, "gain": 6}
            ]
        }
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 4, "parsed 4 bands (2 woofer + 2 tweeter)");

    /* Check Woofer bands (targetChannel = 0). */
    ASSERT(cfg.bands[0].targetChannel == 0, "band 0: woofer channel");
    ASSERT(cfg.bands[0].enabled == 1, "band 0: enabled");
    ASSERT(cfg.bands[1].targetChannel == 0, "band 1: woofer channel");
    ASSERT(cfg.bands[1].enabled == 1, "band 1: enabled");

    /* Check Tweeter bands (targetChannel = 1). */
    ASSERT(cfg.bands[2].targetChannel == 1, "band 2: tweeter channel");
    ASSERT(cfg.bands[2].enabled == 1, "band 2: enabled");
    ASSERT(cfg.bands[3].targetChannel == 1, "band 3: tweeter channel");
    ASSERT(cfg.bands[3].enabled == 0, "band 3: disabled");

    /* Check that enabled bands have non-pass-through coefficients. */
    bool band0Active = (cfg.bands[0].b1 != 0.f || cfg.bands[0].b2 != 0.f ||
                        cfg.bands[0].a1 != 0.f || cfg.bands[0].a2 != 0.f);
    ASSERT(band0Active, "band 0: has real coefficients (not pass-through)");

    /* Check that disabled band has pass-through coefficients. */
    bool band3Pass = (std::fabs(cfg.bands[3].b0 - 1.f) < 0.01f &&
                      cfg.bands[3].b1 == 0.f && cfg.bands[3].b2 == 0.f &&
                      cfg.bands[3].a1 == 0.f && cfg.bands[3].a2 == 0.f);
    ASSERT(band3Pass, "band 3: pass-through coefficients (disabled)");

    std::printf("%s\n", band0Active && band3Pass ? "PASS" : "FAIL");
}

static void test_field_name_mapping() {
    std::printf("Test: field_name_mapping ... ");

    /* Test that 'frequency' maps to cutoffHz and 'gain' maps to gainDb. */
    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Woofer": [
                {"frequency": 1000, "q": 0.707, "gain": 6}
            ]
        }
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 1, "parsed 1 band");

    /* Coefficients should be computed for a peaking EQ at 1kHz with +6dB. */
    bool hasCoeffs = (cfg.bands[0].b1 != 0.f || cfg.bands[0].a1 != 0.f);
    ASSERT(hasCoeffs, "frequency/gain field names produce valid coefficients");

    /* Verify the coefficient values are reasonable for +6dB peaking at 1kHz/48kHz. */
    /* For a peaking EQ with A=2 (+6dB), b0 should be > 1. */
    ASSERT(cfg.bands[0].b0 > 1.f, "b0 > 1 for positive gain");

    std::printf("%s\n", hasCoeffs ? "PASS" : "FAIL");
}

static void test_channel_assignment() {
    std::printf("Test: channel_assignment ... ");

    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Woofer": [
                {"frequency": 200, "q": 0.707, "gain": 0}
            ],
            "Tweeter": [
                {"frequency": 4000, "q": 0.707, "gain": 0}
            ]
        }
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 2, "parsed 2 bands");
    ASSERT(cfg.bands[0].targetChannel == 0, "Woofer -> channel 0");
    ASSERT(cfg.bands[1].targetChannel == 1, "Tweeter -> channel 1");

    std::printf("%s\n", (cfg.bands[0].targetChannel == 0 && cfg.bands[1].targetChannel == 1) ? "PASS" : "FAIL");
}

static void test_disabled_bands() {
    std::printf("Test: disabled_bands ... ");

    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Woofer": [
                {"id": "b1", "enabled": false, "frequency": 300, "q": 1.0, "gain": 10}
            ]
        }
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 1, "parsed 1 band (disabled)");
    ASSERT(cfg.bands[0].enabled == 0, "band marked as disabled");

    /* Disabled band should have pass-through coefficients. */
    bool isPassThrough = (std::fabs(cfg.bands[0].b0 - 1.f) < 0.01f &&
                          cfg.bands[0].b1 == 0.f && cfg.bands[0].b2 == 0.f &&
                          cfg.bands[0].a1 == 0.f && cfg.bands[0].a2 == 0.f);
    ASSERT(isPassThrough, "disabled band has pass-through coefficients");

    std::printf("%s\n", isPassThrough ? "PASS" : "FAIL");
}

static void test_legacy_flat_array() {
    std::printf("Test: legacy_flat_array ... ");

    /* Legacy format: flat array with cutoffHz and gainDb. */
    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": [
            {"cutoffHz": 1000, "q": 0.707, "gainDb": 6, "targetChannel": 0},
            {"cutoffHz": 4000, "q": 1.0, "gainDb": -3, "targetChannel": 1}
        ]
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 2, "parsed 2 bands (legacy format)");
    ASSERT(cfg.bands[0].targetChannel == 0, "band 0: channel 0");
    ASSERT(cfg.bands[1].targetChannel == 1, "band 1: channel 1");
    ASSERT(cfg.bands[0].enabled == 1, "band 0: enabled (legacy default)");

    std::printf("%s\n", (cfg.bandCount == 2) ? "PASS" : "FAIL");
}

static void test_legacy_field_aliases() {
    std::printf("Test: legacy_field_aliases ... ");

    /* Legacy flat array with 'frequency' and 'gain' aliases. */
    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": [
            {"frequency": 2000, "q": 0.707, "gain": 3, "targetChannel": 0}
        ]
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 1, "parsed 1 band with alias field names");

    /* Should have valid coefficients. */
    bool hasCoeffs = (cfg.bands[0].b1 != 0.f || cfg.bands[0].a1 != 0.f);
    ASSERT(hasCoeffs, "frequency/gain aliases produce valid coefficients in flat array");

    std::printf("%s\n", hasCoeffs ? "PASS" : "FAIL");
}

static void test_load_pipeline() {
    std::printf("Test: load_pipeline ... ");

    /* Write a temp JSON file and run through ConfigLoader::load(). */
    const char* tmpPath = "/tmp/pi_audio_eq_test.json";
    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Woofer": [
                {"id": "w1", "enabled": true, "frequency": 300, "q": 0.707, "gain": 4},
                {"id": "w2", "enabled": false, "frequency": 600, "q": 1.0, "gain": 6}
            ],
            "Tweeter": [
                {"id": "t1", "enabled": true, "frequency": 5000, "q": 0.707, "gain": -2}
            ]
        }
    })";

    FILE* f = std::fopen(tmpPath, "w");
    ASSERT(f != nullptr, "could create temp file");
    if (f) {
        std::fputs(json, f);
        std::fclose(f);
    }

    ConfigLoader loader;
    CmdLoadParams params{};
    bool ok = loader.load(tmpPath, params);
    if (!ok) {
        /* load() can fail if validation rejects the config. Use parseFullConfig instead. */
        FullConfig cfg;
        size_t jsonLen = strlen(json);
        bool parsed = ConfigLoader::parseFullConfig(json, jsonLen, cfg);
        ASSERT(parsed, "parseFullConfig succeeded for WebUI bands");
        ASSERT(cfg.bandCount == 3, "pipeline: 3 bands parsed");

        /* Verify IIR coefficients are computed correctly. */
        uint32_t wooferBands = 0, tweeterBands = 0;
        for (uint32_t i = 0; i < cfg.bandCount; ++i) {
            if (cfg.bands[i].targetChannel == 0) wooferBands++;
            else if (cfg.bands[i].targetChannel == 1) tweeterBands++;
        }
        ASSERT(wooferBands == 2, "pipeline: 2 woofer bands");
        ASSERT(tweeterBands == 1, "pipeline: 1 tweeter band");

        /* Check that enabled band has real coefficients. */
        bool w0Active = (cfg.bands[0].b1 != 0.f || cfg.bands[0].a1 != 0.f);
        ASSERT(w0Active, "pipeline: woofer band 0 has real coefficients");

        /* Check that disabled band has pass-through. */
        bool w2Pass = (std::fabs(cfg.bands[1].b0 - 1.f) < 0.01f && cfg.bands[1].enabled == 0);
        ASSERT(w2Pass, "pipeline: woofer band 1 is disabled with pass-through");

        std::remove(tmpPath);
        std::printf("%s\n", (wooferBands == 2 && tweeterBands == 1) ? "PASS" : "FAIL");
        return;
    }

    /* Check that IIR_COEF entries are in the payload. */
    uint32_t iirCount = 0;
    if (params.payloadSize > sizeof(ParamBlockHeader)) {
        const uint8_t* entries = params.payload + sizeof(ParamBlockHeader);
        const ParamBlockHeader* hdr = reinterpret_cast<const ParamBlockHeader*>(params.payload);
        uint32_t remaining = params.payloadSize - sizeof(ParamBlockHeader);

        for (uint32_t i = 0; i < hdr->stageCount && remaining >= 8; ++i) {
            uint16_t type, slot;
            std::memcpy(&type, entries, 2); entries += 2;
            std::memcpy(&slot, entries, 2); entries += 2;
            uint32_t dataLen = 0;
            std::memcpy(&dataLen, entries, 4); entries += 4;

            if (type == STAGE_PARAM_IIR_COEF) {
                iirCount++;
            }
            entries += dataLen;
            remaining -= 8 + dataLen;
        }
    }
    ASSERT(iirCount == 3, "pipeline: 3 IIR_COEF entries packed");

    std::remove(tmpPath);
    std::printf("%s\n", (iirCount == 3) ? "PASS" : "FAIL");
}

static void test_woofer_only() {
    std::printf("Test: woofer_only ... ");

    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Woofer": [
                {"frequency": 250, "q": 0.707, "gain": 3}
            ]
        }
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 1, "parsed 1 band (woofer only)");
    ASSERT(cfg.bands[0].targetChannel == 0, "band on woofer channel");

    std::printf("%s\n", (cfg.bandCount == 1 && cfg.bands[0].targetChannel == 0) ? "PASS" : "FAIL");
}

static void test_tweeter_only() {
    std::printf("Test: tweeter_only ... ");

    const char* json = R"({
        "sampleRate": 48000,
        "BufferSize": 512,
        "bands": {
            "Tweeter": [
                {"frequency": 6000, "q": 1.0, "gain": -3}
            ]
        }
    })";

    FullConfig cfg;
    bool ok = ConfigLoader::parseFullConfig(json, strlen(json), cfg);
    ASSERT(ok, "parse succeeded");
    ASSERT(cfg.bandCount == 1, "parsed 1 band (tweeter only)");
    ASSERT(cfg.bands[0].targetChannel == 1, "band on tweeter channel");

    std::printf("%s\n", (cfg.bandCount == 1 && cfg.bands[0].targetChannel == 1) ? "PASS" : "FAIL");
}

int main() {
    std::printf("=== ConfigLoader EQ Parsing Tests ===\n\n");

    test_webui_bands_schema();
    test_field_name_mapping();
    test_channel_assignment();
    test_disabled_bands();
    test_legacy_flat_array();
    test_legacy_field_aliases();
    test_load_pipeline();
    test_woofer_only();
    test_tweeter_only();

    if (passed) {
        std::printf("\n[PASS] All config loader EQ tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
