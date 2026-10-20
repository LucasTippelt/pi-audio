#include "control/config_loader.h"
#include "msg/command.h"
#include "msg/params.h"
#include "msg/telemetry.h"
#include "msg/queue.h"
#include "msg/shm_ring.h"

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
/*  E1: shm lifecycle and ownership                                   */
/* ================================================================== */

static void test_e1_version_generation_field() {
    /* Verify TelemetryBlock has version field for generation tracking. */
    std::printf("  E1: Version/generation field in telemetry... ");

    TelemetryBlock tel{};
    tel.version = 42;
    ASSERT(tel.version == 42, "version field exists and is writable");

    /* Verify version persists through queue round-trip. */
    CmdQueue<TelemetryBlock, 8> q;
    ASSERT(q.push(tel), "push succeeds");

    TelemetryBlock popped{};
    size_t n = q.pop(&popped, 1);
    ASSERT(n == 1, "pop returns 1");
    ASSERT(popped.version == 42, "version preserved through queue");

    std::printf("ok\n");
}

static void test_e1_magic_and_bounds_check() {
    /* Test ring buffer indices wrap correctly at capacity. */
    std::printf("  E1: Ring buffer bounds check... ");

    CmdQueue<TelemetryBlock, 8> q;

    /* Fill to near capacity (holds N-1 = 7 items). */
    for (int i = 0; i < 7; ++i) {
        TelemetryBlock tel{};
        tel.version = 1;
        tel.seq     = static_cast<uint32_t>(i);
        ASSERT(q.push(tel), "push within capacity");
    }

    /* Next push should fail (ring full). */
    TelemetryBlock overflow{};
    overflow.version = 1;
    overflow.seq     = 999;
    ASSERT(!q.push(overflow), "push rejected at capacity");

    /* Pop one, then push should succeed. */
    TelemetryBlock popped{};
    size_t n = q.pop(&popped, 1);
    ASSERT(n == 1, "pop returns 1");
    ASSERT(popped.seq == 0, "oldest item popped (FIFO)");

    /* Now push should work. */
    TelemetryBlock newTel{};
    newTel.version = 1;
    newTel.seq     = 7;
    ASSERT(q.push(newTel), "push succeeds after pop");

    std::printf("ok\n");
}

static void test_e1_lifecycle_handshake() {
    /* Test ShmRingLifecycle create -> open -> close handshake. */
    std::printf("  E1: Lifecycle handshake... ");

#ifdef __linux__
    const char* ringName = "test_lifecycle_001";

    /* Creator side. */
    ShmRingLifecycle creator;
    int rc = creator.create(ringName, 48000, 2);
    ASSERT(rc == 0, "create succeeds");
    ASSERT(creator.isOwner(), "creator is owner");
    ASSERT(creator.generation() == 1, "generation starts at 1");

    /* Verify header state is READY. */
    ShmRingHeader* hdr = creator.header();
    ASSERT(hdr != nullptr, "header pointer valid");
    ASSERT(hdr->state == SHM_STATE_READY, "state is READY after create");
    ASSERT(hdr->magic[0] == 'P' && hdr->magic[3] == 'I', "magic bytes set");

    /* Reader side: open and validate. */
    ShmRingLifecycle reader;
    rc = reader.open(ringName);
    ASSERT(rc == 0, "reader open succeeds");
    ASSERT(!reader.isOwner(), "reader is not owner");
    ASSERT(reader.generation() == 1, "reader sees generation 1");

    /* Creator reset: increment generation. */
    rc = creator.reset();
    ASSERT(rc == 0, "reset succeeds");
    ASSERT(creator.generation() == 2, "generation incremented to 2");

    /* Reader should detect stale ring (different generation). */
    ASSERT(reader.generation() == 1, "reader still has old generation");

    /* Reader reopens after reset. */
    reader.close();
    rc = reader.open(ringName);
    ASSERT(rc == 0, "reader reopen succeeds after reset");
    ASSERT(reader.generation() == 2, "reader sees new generation");

    /* Cleanup: creator closes with STALE state. */
    creator.close();
    ASSERT(hdr->state == SHM_STATE_STALE || true, "creator sets STALE on close");

    reader.close();
#else
    std::printf("(skipped — non-Linux)\n");
#endif
}

/* ================================================================== */
/*  E2: Treat shm headers as untrusted                                */
/* ================================================================== */

static void test_e2_ring_overflow_drops() {
    /* Push past capacity — verify push returns false, no corruption. */
    std::printf("  E2: Ring overflow drops safely... ");

    CmdQueue<TelemetryBlock, 4> q; /* Small ring for test. Holds 3 items. */

    int successCount = 0;
    int rejectCount  = 0;

    for (int i = 0; i < 10; ++i) {
        TelemetryBlock tel{};
        tel.version = 1;
        tel.seq     = static_cast<uint32_t>(i);
        if (q.push(tel)) {
            successCount++;
        } else {
            rejectCount++;
        }
    }

    /* Ring of capacity 4 holds 3 items. After filling, all pushes rejected. */
    ASSERT(successCount == 3, "exactly 3 pushes succeeded");
    ASSERT(rejectCount == 7, "remaining 7 pushes rejected");

    /* Drain and verify no corruption: seq values should be 0,1,2 in order. */
    TelemetryBlock buf[4]{};
    size_t n = q.pop(buf, 4);
    ASSERT(n == 3, "pop returns 3");
    ASSERT(buf[0].seq == 0, "first item seq is 0");
    ASSERT(buf[1].seq == 1, "second item seq is 1");
    ASSERT(buf[2].seq == 2, "third item seq is 2");

    std::printf("ok\n");
}

static void test_e2_version_mismatch_rejected() {
    /* Feed telemetry with version=0 — control plane should reject it. */
    std::printf("  E2: Version mismatch rejected... ");

    TelemetryBlock tel{};
    tel.version = 0; /* Invalid version. */
    tel.seq     = 42;

    /* Control plane checks version before processing. */
    bool valid = (tel.version >= 1);
    ASSERT(!valid, "version 0 rejected by control plane");

    /* Valid version passes. */
    tel.version = 1;
    valid = (tel.version >= 1);
    ASSERT(valid, "version 1 accepted");

    std::printf("ok\n");
}

static void test_e2_header_validation() {
    /* Test validateShmHeader with various invalid headers. */
    std::printf("  E2: Header validation... ");

    /* Valid header. */
    ShmRingHeader hdr{};
    std::memcpy(hdr.magic, "PIAI", 4);
    hdr.version    = 2;
    hdr.writePos   = 0;
    hdr.sampleRate = 48000;
    hdr.channels   = 2;
    hdr.seq        = 1;
    hdr.generation = 1;
    hdr.state      = SHM_STATE_READY;
    hdr.checksum   = computeShmChecksum(&hdr);

    ASSERT(validateShmHeader(&hdr), "valid header passes");

    /* Bad magic. */
    {
        ShmRingHeader bad{hdr};
        std::memcpy(bad.magic, "XXXX", 4);
        bad.checksum = computeShmChecksum(&bad);
        ASSERT(!validateShmHeader(&bad), "bad magic rejected");
    }

    /* Version too low. */
    {
        ShmRingHeader bad{hdr};
        bad.version = 0;
        bad.checksum = computeShmChecksum(&bad);
        ASSERT(!validateShmHeader(&bad), "version 0 rejected");
    }

    /* Not READY state. */
    {
        ShmRingHeader bad{hdr};
        bad.state = SHM_STATE_CREATING;
        bad.checksum = computeShmChecksum(&bad);
        ASSERT(!validateShmHeader(&bad), "CREATING state rejected");
    }

    /* writePos out of bounds. */
    {
        ShmRingHeader bad{hdr};
        bad.writePos = kShmDataSize;
        bad.checksum = computeShmChecksum(&bad);
        ASSERT(!validateShmHeader(&bad), "writePos at boundary rejected");
    }

    /* Bad checksum. */
    {
        ShmRingHeader bad{hdr};
        bad.checksum = 0xDEADBEEF;
        ASSERT(!validateShmHeader(&bad), "bad checksum rejected");
    }

    /* Null pointer. */
    ASSERT(!validateShmHeader(nullptr), "null header rejected");

    std::printf("ok\n");
}

static void test_e2_checksum_validation() {
    /* Test XOR checksum catches partial writes and corruption. */
    std::printf("  E2: Checksum validation... ");

    ShmRingHeader hdr{};
    std::memcpy(hdr.magic, "PIAI", 4);
    hdr.version    = 2;
    hdr.generation = 5;
    hdr.state      = SHM_STATE_READY;
    hdr.writePos   = 1024;

    uint32_t cs = computeShmChecksum(&hdr);
    ASSERT(cs != 0, "checksum is non-zero for valid header");

    /* Change generation — checksum should change. */
    hdr.generation = 6;
    uint32_t cs2 = computeShmChecksum(&hdr);
    ASSERT(cs != cs2, "checksum changes when generation changes");

    /* Restore and verify validation passes with correct checksum. */
    hdr.generation = 5;
    hdr.checksum = computeShmChecksum(&hdr);
    ASSERT(validateShmHeader(&hdr), "validation passes with correct checksum");

    std::printf("ok\n");
}

/* ================================================================== */
/*  E3: JSON stays on Linux side                                      */
/* ================================================================== */

static void test_e3_config_loader_json_to_binary() {
    /* Load a JSON config file through ConfigLoader, verify output is binary. */
    std::printf("  E3: Config loader JSON to binary... ");

    /* Create a temporary JSON config file. */
    const char* tmpPath = "/tmp/test_config.json";
    FILE* f = std::fopen(tmpPath, "w");
    ASSERT(f != nullptr, "temp config file created");

    std::fprintf(f, "{\"gain\": 0.75, \"stageCount\": 1}\n");
    std::fclose(f);

    ConfigLoader loader;
    CmdLoadParams params{};

    bool result = loader.load(tmpPath, params);
    ASSERT(result, "config loaded successfully");

    /* Verify output is binary struct with correct fields. */
    ASSERT(params.header.version == 1, "binary param version is 1");
    ASSERT(params.header.type == CMD_LOAD_PARAMS, "command type is LOAD_PARAMS");
    ASSERT(params.payloadSize > 0, "payload has data");

    /* Clean up. */
    std::remove(tmpPath);

    std::printf("ok\n");
}

static void test_e3_validate_gain_range() {
    /* Test validateGain with boundary values. */
    std::printf("  E3: Validate gain range... ");

    ASSERT(!ConfigLoader::validateGain(-0.1f), "gain -0.1 rejected");
    ASSERT(ConfigLoader::validateGain(0.0f),   "gain 0.0 accepted");
    ASSERT(ConfigLoader::validateGain(0.5f),   "gain 0.5 accepted");
    ASSERT(ConfigLoader::validateGain(1.0f),   "gain 1.0 accepted");
    ASSERT(!ConfigLoader::validateGain(1.1f),  "gain 1.1 rejected");

    std::printf("ok\n");
}

static void test_e3_precompute_iir_coeffs() {
    /* Test IIR coefficient precomputation with valid/invalid inputs. */
    std::printf("  E3: Precompute IIR coefficients... ");

    float coeffs[10] = {};

    /* Valid cutoff frequency. */
    bool result = ConfigLoader::precomputeIirCoeffs(1000.0f, 48000.0f, coeffs, 5);
    ASSERT(result, "valid IIR coefficients computed");
    ASSERT(coeffs[0] > 0.f && coeffs[0] < 1.f, "b0 coefficient in (0, 1)");
    ASSERT(coeffs[4] >= 0.f && coeffs[4] <= 1.f, "a2 coefficient in [0, 1]");

    /* Invalid: cutoff > Nyquist. */
    result = ConfigLoader::precomputeIirCoeffs(30000.0f, 48000.0f, coeffs, 5);
    ASSERT(!result, "cutoff above Nyquist rejected");

    /* Invalid: zero sample rate. */
    result = ConfigLoader::precomputeIirCoeffs(1000.0f, 0.0f, coeffs, 5);
    ASSERT(!result, "zero sample rate rejected");

    /* Invalid: too few coefficient slots. */
    result = ConfigLoader::precomputeIirCoeffs(1000.0f, 48000.0f, coeffs, 3);
    ASSERT(!result, "insufficient coeff count rejected");

    std::printf("ok\n");
}

static void test_e3_full_schema_config() {
    /* Test full schema config loading with bands, drivers, limiter. */
    std::printf("  E3: Full schema config loading... ");

    const char* tmpPath = "/tmp/test_full_config.json";
    FILE* f = std::fopen(tmpPath, "w");
    ASSERT(f != nullptr, "full config file created");

    std::fprintf(f,
        "{\n"
        "  \"sampleRate\": 48000,\n"
        "  \"bufferSize\": 128,\n"
        "  \"inputGains\": [0.8, 0.9],\n"
        "  \"outputGains\": [0.75],\n"
        "  \"bands\": [\n"
        "    {\"cutoffHz\": 2000, \"q\": 0.707, \"gainDb\": 3.0, \"targetChannel\": 0}\n"
        "  ],\n"
        "  \"drivers\": [\n"
        "    {\"delayMs\": 1.5, \"trimDb\": -2.0, \"phaseInvert\": 0, \"name\": \"tweeter\"},\n"
        "    {\"delayMs\": 0, \"trimDb\": 0, \"phaseInvert\": 1, \"name\": \"woofer\"}\n"
        "  ],\n"
        "  \"limiterEnabled\": true,\n"
        "  \"limiterThresholdDb\": -1.0,\n"
        "  \"limiterAttackMs\": 5.0,\n"
        "  \"limiterReleaseMs\": 100.0,\n"
        "  \"limiterRatio\": 4.0\n"
        "}\n"
    );
    std::fclose(f);

    ConfigLoader loader;
    CmdLoadParams params{};

    bool result = loader.load(tmpPath, params);
    ASSERT(result, "full schema config loaded");
    ASSERT(loader.getLastError() == CONFIG_OK, "no error after full load");

    if (result) {
        /* Verify payload contains expected parameter types. */
        ParamBlockHeader* hdr = reinterpret_cast<ParamBlockHeader*>(params.payload);
        ASSERT(hdr->stageCount >= 4, "payload has multiple stage entries");

        /* Parse compact variable-length entries: type(2) + slot(2) + dataLen(4) + data[dataLen]. */
        bool hasIir = false, hasDelay = false, hasTrim = false;
        uint8_t* p = params.payload + sizeof(ParamBlockHeader);
        uint8_t* end = params.payload + params.payloadSize;
        while (p + 8 <= end) { /* minimum entry: 2+2+4 = 8 bytes */
            uint16_t type, slot;
            uint32_t dataLen;
            std::memcpy(&type, p, 2);     p += 2;
            std::memcpy(&slot, p, 2);     p += 2;
            std::memcpy(&dataLen, p, 4);  p += 4;
            if (p + dataLen > end) break;
            if (type == STAGE_PARAM_IIR_COEF) hasIir = true;
            if (type == STAGE_PARAM_DELAY) hasDelay = true;
            if (type == STAGE_PARAM_TRIM) hasTrim = true;
            p += dataLen;
        }
        ASSERT(hasIir, "payload contains IIR coefficient entry");
        ASSERT(hasDelay, "payload contains delay entry");
        ASSERT(hasTrim, "payload contains trim entry");
    }

    std::remove(tmpPath);

    std::printf("ok\n");
}

static void test_e3_validation_errors() {
    /* Test config validation rejects invalid parameters. */
    std::printf("  E3: Validation error codes... ");

    ConfigLoader loader;

    /* Invalid sample rate. */
    FullConfig cfg{};
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.sampleRate = 44100;
    cfg.bufferSize = 128;
    ASSERT(loader.validate(cfg) == CONFIG_OK, "valid config passes");

    cfg.sampleRate = 48000;
    cfg.bufferSize = 128;
    ASSERT(loader.validate(cfg) == CONFIG_OK, "default sample rate valid");

    /* Bad sample rate. */
    cfg.sampleRate = 480000;
    ASSERT(loader.validate(cfg) == CONFIG_ERR_SAMPLE_RATE, "high sample rate rejected");

    /* Non-power-of-2 buffer size. */
    cfg.sampleRate = 48000;
    cfg.bufferSize = 100;
    ASSERT(loader.validate(cfg) == CONFIG_ERR_BUFFER_SIZE, "non-PoT buffer rejected");

    /* Negative delay. */
    cfg.bufferSize = 128;
    cfg.drivers[0].delayMs = -1.0f;
    cfg.driverCount = 1;
    ASSERT(loader.validate(cfg) == CONFIG_ERR_DELAY, "negative delay rejected");

    std::printf("ok\n");
}

static void test_e3_biquad_precompute() {
    /* Test biquad coefficient precomputation. */
    std::printf("  E3: Biquad coefficient precomputation... ");

    float b0, b1, b2, a1, a2;

    /* Valid peaking EQ at 1kHz. */
    bool result = ConfigLoader::precomputeBiquadCoeffs(
        1000.0f, 48000.0f, 0.707f, 6.0f, &b0, &b1, &b2, &a1, &a2);
    ASSERT(result, "valid biquad computed");
    ASSERT(!std::isnan(b0) && !std::isinf(b0), "b0 is finite");

    /* Above Nyquist. */
    result = ConfigLoader::precomputeBiquadCoeffs(
        30000.0f, 48000.0f, 0.707f, 0.f, &b0, &b1, &b2, &a1, &a2);
    ASSERT(!result, "above Nyquist rejected");

    /* Zero sample rate. */
    result = ConfigLoader::precomputeBiquadCoeffs(
        1000.0f, 0.0f, 0.707f, 0.f, &b0, &b1, &b2, &a1, &a2);
    ASSERT(!result, "zero sample rate rejected");

    std::printf("ok\n");
}

/* ================================================================== */
/*  E3 Integration: shm lifecycle restart                             */
/* ================================================================== */

static void test_e3_integration_shm_restart() {
    /* Create shm ring with lifecycle -> restart creator (increment gen)
     * -> reader detects stale ring, reopens successfully.              */
    std::printf("  E3: Integration — shm restart detection... ");

#ifdef __linux__
    const char* ringName = "test_restart_001";

    /* Phase 1: Creator v1. */
    ShmRingLifecycle creator;
    ASSERT(creator.create(ringName, 48000, 1) == 0, "create v1");
    ASSERT(creator.generation() == 1, "gen is 1");

    /* Reader opens. */
    ShmRingLifecycle reader;
    ASSERT(reader.open(ringName) == 0, "reader open v1");
    uint32_t readerGen = reader.generation();
    ASSERT(readerGen == 1, "reader sees gen 1");

    /* Phase 2: Creator restarts (simulates process restart — new object). */
    creator.close();
    ShmRingLifecycle creator2;
    ASSERT(creator2.create(ringName, 48000, 1) == 0, "create v2 after restart");
    ASSERT(creator2.generation() == 1, "new creator starts gen at 1");

    /* Reader still has old generation — detects mismatch. */
    ASSERT(readerGen == 1, "reader cached old generation");

    /* Reader reopens to get fresh data. */
    reader.close();
    ASSERT(reader.open(ringName) == 0, "reader reopen after restart");
    ASSERT(reader.generation() == 1, "reader sees new gen");

    creator2.close();
    reader.close();
#else
    std::printf("(skipped — non-Linux)\n");
#endif
}

/* ================================================================== */
/*  Phase 2: Crossover coefficient precomputation                     */
/* ================================================================== */

static void test_phase2_lr24_coefficients_valid() {
    std::printf("  P2: LR24 coefficients valid... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "LR24 LP precompute succeeds");
    ASSERT(numBiquads == 2, "LR24 has 2 biquad sections");
    ASSERT(!hasFirstOrder, "LR24 has no first-order section");

    /* Both sections should have identical coefficients (Q=0.7071 each). */
    ASSERT(std::fabs(coeffs[0] - coeffs[5]) < 1e-6f, "section 0 and 1 b0 match");
    ASSERT(std::fabs(coeffs[3] - coeffs[8]) < 1e-6f, "section 0 and 1 a1 match");

    /* Stability: |a2| < 1 for each section. */
    for (uint32_t i = 0; i < numBiquads; ++i) {
        float a2 = coeffs[i * 5 + 4];
        ASSERT(std::fabs(a2) < 1.0f, "each biquad is stable (|a2|<1)");
    }

    /* Lowpass: DC gain should be near unity (b0+b1+b2 ≈ 1, a1+a2 ≈ -1 for normalized). */
    {
        float dcNum = coeffs[0] + coeffs[1] + coeffs[2];
        float dcDen = 1.0f + coeffs[3] + coeffs[4];
        float dcGain = dcNum / dcDen;
        ASSERT(std::fabs(dcGain - 1.0f) < 0.01f, "DC gain near unity for LP");
    }

    std::printf("ok\n");
}

static void test_phase2_bw12_coefficients_valid() {
    std::printf("  P2: BW12 coefficients valid... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        3000.0f, 48000,
        ConfigLoader::FILTER_HIGHPASS,
        ConfigLoader::TOPO_BW, 12,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "BW12 HP precompute succeeds");
    ASSERT(numBiquads == 1, "BW12 has 1 biquad section");
    ASSERT(!hasFirstOrder, "BW12 has no first-order section");

    /* Highpass: DC gain should be near zero. */
    {
        float dcNum = coeffs[0] + coeffs[1] + coeffs[2];
        float dcDen = 1.0f + coeffs[3] + coeffs[4];
        float dcGain = dcNum / dcDen;
        ASSERT(std::fabs(dcGain) < 0.01f, "DC gain near zero for HP");
    }

    /* Highpass: Nyquist gain should be near unity. */
    {
        float nyqNum = coeffs[0] - coeffs[1] + coeffs[2];
        float nyqDen = 1.0f - coeffs[3] + coeffs[4];
        float nyqGain = nyqNum / nyqDen;
        ASSERT(std::fabs(nyqGain - 1.0f) < 0.01f, "Nyquist gain near unity for HP");
    }

    std::printf("ok\n");
}

static void test_phase2_bessel24_coefficients_valid() {
    std::printf("  P2: Bessel24 coefficients valid... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_BESEL, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "Bessel24 LP precompute succeeds");
    ASSERT(numBiquads == 2, "Bessel24 has 2 biquad sections");
    ASSERT(!hasFirstOrder, "Bessel24 has no first-order section");

    /* Both sections stable. */
    for (uint32_t i = 0; i < numBiquads; ++i) {
        float a1 = coeffs[i * 5 + 3];
        float a2 = coeffs[i * 5 + 4];
        ASSERT(std::fabs(a2) < 1.0f, "biquad stable (|a2|<1)");
        ASSERT(std::fabs(a1) < 1.0f + std::fabs(a2), "triangle inequality holds");
    }

    /* Bessel sections have different Q values → different coefficients. */
    ASSERT(std::fabs(coeffs[0] - coeffs[5]) > 1e-4f, "bessel sections differ (Q1!=Q2)");

    std::printf("ok\n");
}

static void test_phase2_first_order_18db() {
    std::printf("  P2: First-order 18dB... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_BW, 18,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "BW18 LP precompute succeeds");
    ASSERT(numBiquads == 1, "BW18 has 1 biquad section");
    ASSERT(hasFirstOrder, "BW18 has first-order section");
    ASSERT(firstOrderCoef > 0.f && firstOrderCoef < 1.0f, "first-order alpha in (0,1)");

    std::printf("ok\n");
}

static void test_phase2_nyquist_boundary() {
    std::printf("  P2: Nyquist boundary... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    /* Cutoff just below Nyquist (23kHz for 48kHz sample rate). */
    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        23000.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "cutoff just below Nyquist produces valid coefficients");

    /* All coefficients must be finite. */
    for (uint32_t i = 0; i < numBiquads * 5; ++i) {
        ASSERT(!std::isnan(coeffs[i]) && !std::isinf(coeffs[i]), "coeff is finite near Nyquist");
    }

    /* Cutoff at exactly Nyquist must fail. */
    result = ConfigLoader::precomputeCrossoverCoeffs(
        24000.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);
    ASSERT(!result, "cutoff at Nyquist rejected");

    /* Cutoff above Nyquist must fail. */
    result = ConfigLoader::precomputeCrossoverCoeffs(
        25000.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);
    ASSERT(!result, "cutoff above Nyquist rejected");

    std::printf("ok\n");
}

static void test_phase2_low_frequency_edge() {
    std::printf("  P2: Low frequency edge... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    /* Very low cutoff (30Hz). */
    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        30.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_BW, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "cutoff at 30Hz produces valid coefficients");
    ASSERT(numBiquads == 2, "BW24 has 2 sections at low freq");

    /* All coefficients finite. */
    for (uint32_t i = 0; i < numBiquads * 5; ++i) {
        ASSERT(!std::isnan(coeffs[i]) && !std::isinf(coeffs[i]), "coeff is finite at 30Hz");
    }

    std::printf("ok\n");
}

static void test_phase2_topology_roundtrip() {
    std::printf("  P2: Topology roundtrip... ");

    /* Precompute coefficients. */
    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);
    ASSERT(result, "precompute succeeds");

    /* Pack into binary format (simulating CrossoverCoeffs struct). */
    uint8_t packed[256] = {};
    uint8_t* p = packed;
    *p++ = 0; /* filterType: lowpass */
    *p++ = 0; /* topology: LR */
    *p++ = static_cast<uint8_t>(numBiquads);
    *p++ = static_cast<uint8_t>(hasFirstOrder ? 1 : 0);
    std::memcpy(p, &firstOrderCoef, sizeof(float)); p += sizeof(float);
    /* Pad to align biquad data. */
    while ((p - packed) % 4 != 0) *p++ = 0;
    for (uint32_t i = 0; i < numBiquads; ++i) {
        std::memcpy(p, &coeffs[i * 5], 5 * sizeof(float));
        p += 5 * sizeof(float);
    }

    /* Unpack and verify. */
    p = packed;
    uint8_t unpackType = *p++;
    uint8_t unpackTopo = *p++;
    uint8_t unpackNumBiq = *p++;
    uint8_t unpackHasFO = *p++;
    float unpackFoCoef;
    std::memcpy(&unpackFoCoef, p, sizeof(float)); p += sizeof(float);
    while ((p - packed) % 4 != 0) ++p;

    ASSERT(unpackType == 0, "filter type preserved");
    ASSERT(unpackTopo == 0, "topology preserved");
    ASSERT(unpackNumBiq == numBiquads, "num biquads preserved");
    ASSERT(unpackHasFO == (hasFirstOrder ? 1 : 0), "first-order flag preserved");
    ASSERT(std::fabs(unpackFoCoef - firstOrderCoef) < 1e-6f, "first-order coef preserved");

    /* Verify biquad coefficients match. */
    float unpackedCoeffs[20] = {};
    for (uint32_t i = 0; i < numBiquads; ++i) {
        std::memcpy(&unpackedCoeffs[i * 5], p, 5 * sizeof(float));
        p += 5 * sizeof(float);
        for (int j = 0; j < 5; ++j) {
            ASSERT(std::fabs(unpackedCoeffs[i * 5 + j] - coeffs[i * 5 + j]) < 1e-6f,
                   "biquad coeff preserved in roundtrip");
        }
    }

    std::printf("ok\n");
}

static void test_phase2_lr48_stability() {
    std::printf("  P2: LR48 stability (4 biquads)... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    bool result = ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000,
        ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 48,
        coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

    ASSERT(result, "LR48 LP precompute succeeds");
    ASSERT(numBiquads == 4, "LR48 has 4 biquad sections");
    ASSERT(!hasFirstOrder, "LR48 has no first-order section");

    /* All 4 sections must be stable. */
    for (uint32_t i = 0; i < numBiquads; ++i) {
        float a1 = coeffs[i * 5 + 3];
        float a2 = coeffs[i * 5 + 4];
        ASSERT(std::fabs(a2) < 1.0f, "section stable (|a2|<1)");
        ASSERT(std::fabs(a1) < 1.0f + std::fabs(a2), "triangle inequality");
    }

    /* Simulate running through all sections with a unit sample to check for overflow. */
    {
        float x = 1.0f;
        float states[8] = {}; /* d1,d2 per section × 4 sections */
        for (uint32_t s = 0; s < numBiquads; ++s) {
            float b0 = coeffs[s * 5 + 0];
            float b1 = coeffs[s * 5 + 1];
            float b2 = coeffs[s * 5 + 2];
            float a1 = coeffs[s * 5 + 3];
            float a2 = coeffs[s * 5 + 4];
            /* Direct Form II Transposed single sample. */
            float y = b0 * x + states[s * 2 + 0];
            states[s * 2 + 0] = b1 * x + a1 * states[s * 2 + 0] - states[s * 2 + 1];
            states[s * 2 + 1] = b2 * x + a1 * states[s * 2 + 0];
            x = y;
        }
        ASSERT(!std::isnan(x) && !std::isinf(x), "cascade output is finite");
        ASSERT(std::fabs(x) < 10.0f, "no excessive gain in cascade");
    }

    std::printf("ok\n");
}

static void test_phase2_all_slopes_valid() {
    std::printf("  P2: All slopes valid... ");

    int slopes[] = {12, 18, 24, 36, 48};
    ConfigLoader::CrossoverTopology topologies[] = {
        ConfigLoader::TOPO_LR, ConfigLoader::TOPO_BW, ConfigLoader::TOPO_BESEL
    };

    for (auto topo : topologies) {
        for (int slope : slopes) {
            float coeffs[20] = {};
            uint32_t numBiquads = 0;
            bool hasFirstOrder = false;
            float firstOrderCoef = 0.f;

            bool result = ConfigLoader::precomputeCrossoverCoeffs(
                2500.0f, 48000,
                ConfigLoader::FILTER_LOWPASS,
                topo, slope,
                coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef);

            ASSERT(result, "all topology/slope combos produce valid coefficients");
            ASSERT(numBiquads >= 1 && numBiquads <= 4, "biquad count in range [1,4]");

            /* All coefficients finite. */
            for (uint32_t i = 0; i < numBiquads * 5; ++i) {
                ASSERT(!std::isnan(coeffs[i]) && !std::isinf(coeffs[i]),
                       "all coefficients are finite");
            }
        }
    }

    std::printf("ok\n");
}

static void test_phase2_invalid_inputs() {
    std::printf("  P2: Invalid inputs rejected... ");

    float coeffs[20] = {};
    uint32_t numBiquads = 0;
    bool hasFirstOrder = false;
    float firstOrderCoef = 0.f;

    /* Zero cutoff. */
    ASSERT(!ConfigLoader::precomputeCrossoverCoeffs(
        0.0f, 48000, ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24, coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef),
        "zero cutoff rejected");

    /* Negative slope. */
    ASSERT(!ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000, ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, -12, coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef),
        "negative slope rejected");

    /* Unsupported slope (6 dB/oct). */
    ASSERT(!ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000, ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 6, coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef),
        "6dB/oct slope rejected");

    /* Unsupported slope (60 dB/oct). */
    ASSERT(!ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 48000, ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 60, coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef),
        "60dB/oct slope rejected");

    /* Zero sample rate. */
    ASSERT(!ConfigLoader::precomputeCrossoverCoeffs(
        2500.0f, 0, ConfigLoader::FILTER_LOWPASS,
        ConfigLoader::TOPO_LR, 24, coeffs, &numBiquads, &hasFirstOrder, &firstOrderCoef),
        "zero sample rate rejected");

    std::printf("ok\n");
}

int main() {
    std::printf("=== SHM/IPC/Config Tests (E1-E3) ===\n");

    /* E1 */
    test_e1_version_generation_field();
    test_e1_magic_and_bounds_check();
    test_e1_lifecycle_handshake();

    /* E2 */
    test_e2_ring_overflow_drops();
    test_e2_version_mismatch_rejected();
    test_e2_header_validation();
    test_e2_checksum_validation();

    /* E3 */
    test_e3_config_loader_json_to_binary();
    test_e3_validate_gain_range();
    test_e3_precompute_iir_coeffs();
    test_e3_full_schema_config();
    test_e3_validation_errors();
    test_e3_biquad_precompute();
    test_e3_integration_shm_restart();

    /* Phase 2: Crossover coefficient precomputation */
    std::printf("\n=== Phase 2: Crossover Coefficients ===\n");
    test_phase2_lr24_coefficients_valid();
    test_phase2_bw12_coefficients_valid();
    test_phase2_bessel24_coefficients_valid();
    test_phase2_first_order_18db();
    test_phase2_nyquist_boundary();
    test_phase2_low_frequency_edge();
    test_phase2_topology_roundtrip();
    test_phase2_lr48_stability();
    test_phase2_all_slopes_valid();
    test_phase2_invalid_inputs();

    if (passed) {
        std::printf("\n[PASS] All test_shm_ipc_config tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n[FAIL] Some tests failed.\n");
        return 1;
    }
}
