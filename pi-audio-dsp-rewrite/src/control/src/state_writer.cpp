#include "control/state_writer.h"
#include "msg/telemetry.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>

struct TelemetryBlock; /* forward for pushTelemetry */

class StateWriter {
public:
    void start(const char* statePath, uint32_t intervalMs) {
        m_statePath   = statePath;
        m_intervalMs  = intervalMs;
        m_running     = true;

        m_workerThread = std::thread([this]() {
            while (m_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(m_intervalMs));
                if (!m_running) break;

                TelemetryBlock tel;
                size_t n = m_queue.pop(&tel, 1);
                if (n == 0) continue;

                writeState(tel);
            }
        });
    }

    void stop() {
        m_running = false;
        if (m_workerThread.joinable())
            m_workerThread.join();
    }

    void pushTelemetry(const TelemetryBlock& tel) {
        m_queue.push(tel);
    }

private:
    void writeState(const TelemetryBlock& tel) {
        if (!m_statePath) return;

        FILE* f = std::fopen(m_statePath, "w");
        if (!f) return;

        /* Write minimal JSON state. */
        std::fprintf(f, "{\n");
        std::fprintf(f, "  \"version\": %u,\n", tel.version);
        std::fprintf(f, "  \"seq\": %u,\n", tel.seq);
        std::fprintf(f, "  \"xrunUnderrun\": %u,\n", tel.xrunUnderrun);
        std::fprintf(f, "  \"xrunOverrun\": %u,\n", tel.xrunOverrun);
        std::fprintf(f, "  \"clockState\": %u,\n", tel.clockState);
        std::fprintf(f, "  \"outputLevelDb\": [%.2f, %.2f],\n",
                     tel.outputLevelDb[0], tel.outputLevelDb[1]);
        std::fprintf(f, "  \"latencyFrames\": %u,\n", tel.latencyFrames);
        std::fprintf(f, "  \"sampleIndex\": %llu\n", static_cast<unsigned long long>(tel.sampleIndex));
        std::fprintf(f, "}\n");

        std::fclose(f);
    }

    const char* m_statePath = nullptr;
    uint32_t m_intervalMs   = 1000;
    std::atomic<bool> m_running{false};
    std::thread m_workerThread;

    /* Simple internal queue for telemetry. */
    struct RingBuf {
        static constexpr size_t kCap = 64;
        TelemetryBlock buf[kCap];
        std::atomic<size_t> head{0};
        std::atomic<size_t> tail{0};

        bool push(const TelemetryBlock& t) {
            size_t h = head.load();
            size_t next = (h + 1) % kCap;
            if (next == tail.load()) return false;
            buf[h] = t;
            head.store(next);
            return true;
        }

        size_t pop(TelemetryBlock* out, size_t max) {
            size_t t = tail.load();
            size_t h = head.load();
            if (t == h) return 0;
            size_t avail = (h >= t) ? (h - t) : (kCap - t + h);
            size_t n = (max < avail) ? max : avail;
            for (size_t i = 0; i < n; ++i) {
                out[i] = buf[t];
                t = (t + 1) % kCap;
            }
            tail.store(t);
            return n;
        }
    };
    RingBuf m_queue;
};
