#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"

#include <cstdio>
#include <thread>
#include <atomic>
#include <cstring>

static bool passed = true;

#define ASSERT(cond, msg)                                                        \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "[FAIL] %s (line %d)\n", msg, __LINE__);       \
            passed = false;                                                      \
        }                                                                        \
    } while (0)

int main() {
    /* Test basic push/pop. */
    CmdQueue<CmdLoadParams, 16> q;

    CmdLoadParams cmd{};
    cmd.header.version      = 1;
    cmd.header.type         = CMD_LOAD_PARAMS;
    cmd.header.seq          = 1;
    cmd.paramVersion        = 1;
    cmd.payloadSize         = sizeof(float);
    float gain = 0.5f;
    std::memcpy(cmd.payload, &gain, sizeof(float));

    ASSERT(q.push(cmd), "push succeeds");
    ASSERT(q.pending() == 1, "pending count is 1");

    CmdLoadParams popped{};
    size_t n = q.pop(&popped, 1);
    ASSERT(n == 1, "pop returns 1");
    ASSERT(popped.header.seq == 1, "seq matches");
    ASSERT(q.pending() == 0, "pending count is 0 after pop");

    /* Test multiple push/pop. SPSC ring of capacity N holds N-1 items. */
    for (int i = 0; i < 15; ++i) {
        CmdLoadParams c{};
        c.header.version = 1;
        c.header.type    = CMD_LOAD_PARAMS;
        c.header.seq     = static_cast<uint32_t>(i + 1);
        ASSERT(q.push(c), "push in loop");
    }
    ASSERT(q.pending() == 15, "pending is 15");

    CmdLoadParams buf[16]{};
    n = q.pop(buf, 16);
    ASSERT(n == 15, "pop all returns 15");
    for (int i = 0; i < 15; ++i) {
        ASSERT(buf[i].header.seq == static_cast<uint32_t>(i + 1), "seq order preserved");
    }

    /* Test queue full. */
    for (int i = 0; i < 15; ++i) {
        CmdLoadParams c{};
        c.header.version = 1;
        c.header.type    = CMD_LOAD_PARAMS;
        c.header.seq     = static_cast<uint32_t>(i + 1);
        q.push(c);
    }
    CmdLoadParams extra{};
    extra.header.version = 1;
    extra.header.type    = CMD_LOAD_PARAMS;
    extra.header.seq     = 999;
    ASSERT(!q.push(extra), "push fails when full");

    /* Test concurrent push/pop (stress test). */
    CmdQueue<CmdLoadParams, 256> stressQ;
    std::atomic<uint32_t> pushCount{0};
    std::atomic<uint32_t> popCount{0};
    std::atomic<bool> done{false};

    auto producer = [&]() {
        for (int i = 0; i < 10000; ++i) {
            CmdLoadParams c{};
            c.header.version = 1;
            c.header.type    = CMD_LOAD_PARAMS;
            c.header.seq     = static_cast<uint32_t>(i);
            while (!stressQ.push(c)) {
                /* Spin on full. */
            }
            pushCount++;
        }
        done.store(true);
    };

    auto consumer = [&]() {
        uint32_t lastSeq = 0;
        bool sawGap = false;
        while (!done.load() || stressQ.pending() > 0) {
            CmdLoadParams c{};
            size_t got = stressQ.pop(&c, 1);
            if (got == 1) {
                if (lastSeq > 0 && c.header.seq > lastSeq + 1)
                    sawGap = true;
                lastSeq = c.header.seq;
                popCount++;
            }
        }
        if (sawGap) {
            std::fprintf(stderr, "[FAIL] Gap detected in sequence\n");
            passed = false;
        }
    };

    std::thread t1(producer);
    std::thread t2(consumer);
    t1.join();
    t2.join();

    ASSERT(pushCount.load() == 10000, "all pushes completed");
    ASSERT(popCount.load() == 10000, "all pops matched pushes");

    /* Test telemetry queue. */
    CmdQueue<TelemetryBlock, 8> telQ;
    TelemetryBlock tel{};
    tel.version = 1;
    tel.seq     = 42;
    ASSERT(telQ.push(tel), "telemetry push");

    TelemetryBlock poppedTel{};
    n = telQ.pop(&poppedTel, 1);
    ASSERT(n == 1, "telemetry pop returns 1");
    ASSERT(poppedTel.seq == 42, "telemetry seq matches");

    if (passed) {
        std::printf("[PASS] All test_command_queue tests passed.\n");
        return 0;
    } else {
        std::fprintf(stderr, "[FAIL] Some tests failed.\n");
        return 1;
    }
}
