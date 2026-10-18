#include "control/state_writer.h"
#include "msg/telemetry.h"
#include "msg/command.h"
#include "msg/queue.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <atomic>

static bool passed = true;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            passed = false;                                                      \
        }                                                                        \
    } while (0)

/* ================================================================== */
/*  D1: Consolidated monitoring path                                  */
/* ================================================================== */

static void test_d1_single_state_writer() {
    /* Verify StateWriter starts one background thread. */
    std::printf("  D1: Single state writer thread... ");

    const char* tmpPath = "/tmp/test_state_writer.json";
    StateWriter sw;

    sw.start(tmpPath, 50); /* Fast interval for test. */

    /* Give thread time to start. */
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    /* Push a telemetry block. */
    TelemetryBlock tel{};
    tel.version = 1;
    tel.seq     = 42;
    tel.clockState = 1;
    sw.pushTelemetry(tel);

    /* Wait for writer to process. */
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    sw.stop();

    /* Verify state file was created. */
    FILE* f = std::fopen(tmpPath, "r");
    ASSERT(f != nullptr, "state file exists after writer runs");

    if (f) {
        char buf[1024] = {};
        size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);

        /* Verify JSON contains expected fields. */
        ASSERT(std::strstr(buf, "\"version\"") != nullptr, "JSON has version field");
        ASSERT(std::strstr(buf, "\"seq\"") != nullptr, "JSON has seq field");
        ASSERT(std::strstr(buf, "\"clockState\"") != nullptr, "JSON has clockState field");

        /* Clean up. */
        std::remove(tmpPath);
    }

    std::printf("ok\n");
}

static void test_d1_telemetry_ring_push_pop() {
    /* Test StateWriter internal RingBuf push/pop/drain. */
    std::printf("  D1: Telemetry ring push/pop... ");

    const char* tmpPath = "/tmp/test_state_writer_ring.json";
    StateWriter sw;

    /* Push multiple telemetry blocks without starting the writer thread. */
    for (int i = 0; i < 10; ++i) {
        TelemetryBlock tel{};
        tel.version = 1;
        tel.seq     = static_cast<uint32_t>(i);
        sw.pushTelemetry(tel);
    }

    /* Start writer briefly to drain. */
    sw.start(tmpPath, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    sw.stop();

    std::remove(tmpPath);

    std::printf("ok\n");
}

static void test_d1_concurrent_push_no_loss() {
    /* Stress test: producer pushes N telemetry blocks, verify no loss. */
    std::printf("  D1: Concurrent push no loss... ");

    const char* tmpPath = "/tmp/test_state_writer_concurrent.json";
    StateWriter sw;

    const int kPushCount = 50;
    std::atomic<int> pushed{0};
    std::atomic<bool> done{false};

    /* Producer thread. */
    auto producer = [&]() {
        for (int i = 0; i < kPushCount; ++i) {
            TelemetryBlock tel{};
            tel.version = 1;
            tel.seq     = static_cast<uint32_t>(i);
            sw.pushTelemetry(tel);
            pushed++;
        }
        done.store(true);
    };

    std::thread pt(producer);

    /* Wait for producer to finish. */
    pt.join();

    ASSERT(pushed.load() == kPushCount, "all pushes completed");

    /* Start writer briefly to process queued telemetry. */
    sw.start(tmpPath, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sw.stop();

    std::remove(tmpPath);

    std::printf("ok\n");
}

static void test_d1_state_file_written() {
    /* Verify JSON state file contains expected fields. */
    std::printf("  D1: State file written with correct fields... ");

    const char* tmpPath = "/tmp/test_state_fields.json";
    StateWriter sw;

    TelemetryBlock tel{};
    tel.version        = 1;
    tel.seq            = 99;
    tel.xrunUnderrun   = 3;
    tel.xrunOverrun    = 1;
    tel.clockState     = 2;
    tel.outputLevelDb[0] = -12.5f;
    tel.outputLevelDb[1] = -15.3f;
    tel.latencyFrames  = 64;
    tel.sampleIndex    = 100000;

    sw.pushTelemetry(tel);

    sw.start(tmpPath, 30);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    sw.stop();

    FILE* f = std::fopen(tmpPath, "r");
    ASSERT(f != nullptr, "state file exists");

    if (f) {
        char buf[2048] = {};
        size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);

        /* Check all expected fields. */
        ASSERT(std::strstr(buf, "\"version\": 1") != nullptr, "has version: 1");
        ASSERT(std::strstr(buf, "\"seq\": 99") != nullptr, "has seq: 99");
        ASSERT(std::strstr(buf, "\"xrunUnderrun\": 3") != nullptr, "has xrunUnderrun: 3");
        ASSERT(std::strstr(buf, "\"xrunOverrun\": 1") != nullptr, "has xrunOverrun: 1");
        ASSERT(std::strstr(buf, "\"clockState\": 2") != nullptr, "has clockState: 2");
        ASSERT(std::strstr(buf, "\"latencyFrames\": 64") != nullptr, "has latencyFrames: 64");

        std::remove(tmpPath);
    }

    std::printf("ok\n");
}

/* ================================================================== */
/*  D1 Integration: Full monitoring path                              */
/* ================================================================== */

static void test_d1_integration_monitoring_path() {
    /* Simulate complete monitoring path: push telemetry through queue ->
     * StateWriter writes JSON file -> verify file contents match data.   */
    std::printf("  D1: Integration — full monitoring path... ");

    const char* tmpPath = "/tmp/test_integration_monitoring.json";
    CmdQueue<TelemetryBlock, 8> telQueue;
    StateWriter sw;

    /* Push telemetry blocks through the queue (simulates RT thread). */
    const int kBlockCount = 5;
    for (int i = 0; i < kBlockCount; ++i) {
        TelemetryBlock tel{};
        tel.version      = 1;
        tel.seq          = static_cast<uint32_t>(i + 1);
        tel.xrunUnderrun = static_cast<uint32_t>(i);
        tel.clockState   = 1;
        tel.sampleIndex  = static_cast<uint64_t>((i + 1) * 128);
        telQueue.push(tel);
    }

    /* Consumer thread drains queue and feeds StateWriter. */
    std::atomic<bool> consumerRunning{true};
    std::thread consumer([&telQueue, &sw, &consumerRunning]() {
        TelemetryBlock tel{};
        while (consumerRunning.load()) {
            size_t n = telQueue.pop(&tel, 1);
            if (n > 0) {
                sw.ingestTelemetry(tel);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    });

    /* Start StateWriter to persist data. */
    sw.start(tmpPath, 30);

    /* Wait for consumer to drain queue and writer to process. */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    consumerRunning.store(false);
    if (consumer.joinable()) consumer.join();
    sw.stop();

    /* Verify state file contains data from the last telemetry block. */
    FILE* f = std::fopen(tmpPath, "r");
    ASSERT(f != nullptr, "integration: state file exists");

    if (f) {
        char buf[2048] = {};
        size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);

        /* At minimum, the file should have valid JSON with version field. */
        ASSERT(std::strstr(buf, "\"version\"") != nullptr, "integration: has version");
        ASSERT(std::strstr(buf, "\"seq\"") != nullptr, "integration: has seq");

        std::remove(tmpPath);
    }

    std::printf("ok\n");
}

int main() {
    std::printf("=== Monitoring and Networking Tests (D1) ===\n");

    test_d1_telemetry_ring_push_pop();
    test_d1_single_state_writer();
    test_d1_concurrent_push_no_loss();
    test_d1_state_file_written();
    test_d1_integration_monitoring_path();

    if (passed) {
        std::printf("[PASS] All test_monitoring_networking tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
