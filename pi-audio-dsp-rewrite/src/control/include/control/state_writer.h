#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include "msg/telemetry.h"

/* ------------------------------------------------------------------ */
/*  State writer — periodic engine-state.json writer                  */
/*  Reports actual applied DSP state, not config-file intent.         */
/* ------------------------------------------------------------------ */

static constexpr size_t kMaxPcmName = 128;
static constexpr size_t kMaxChains    = 2;
static constexpr size_t kMaxDriverName = 32;
static constexpr size_t kMaxDrivers    = 8;

struct AppliedCrossover {
    float frequencyHz;
    char  topology[32];
    int   slopeDbOct;
    bool  enabled;
};

struct AppliedLimiter {
    float thresholdDb;
    float attackMs;
    float releaseMs;
    float ratio;
    bool  enabled;
};

struct AppliedDriver {
    char name[kMaxDriverName];
    uint32_t delaySamples;
    float trimDb;
    bool phaseInverted;
};

class StateWriter {
public:
    /* Start background thread that polls telemetry and writes state file. */
    void start(const char* statePath, uint32_t intervalMs);

    /* Stop the background thread. */
    void stop();

    /* Push a telemetry snapshot for the writer to persist (latest-wins). */
    void pushTelemetry(const TelemetryBlock& tel);

    /* C5: Ingest a telemetry block from the consumer thread (latest-wins). */
    void ingestTelemetry(const TelemetryBlock& tel);

    /* Set audio config for status-adapter-compatible JSON output. */
    void setAudioConfig(uint32_t sampleRate, uint32_t bufferSize);

    /* Record what was actually applied to the DSP engine (called by reload thread).
     * These are the real active values — NOT read from current.json on every cycle. */
    void setAppliedVolume(int volume);
    void setAppliedMuted(bool muted);
    void setAppliedDevices(const char* input, const char* output);
    void setAppliedRcDevices(const char* rcInput, const char* rcOutput);

    /* Per-chain applied DSP state setters. */
    void setAppliedCrossover(uint8_t chain, const AppliedCrossover& cx);
    void setAppliedLimiter(uint8_t chain, const AppliedLimiter& lim);
    void setAppliedDriver(const char* name, uint32_t delaySamples, float trimDb, bool phaseInvert);

private:
    void writeState(const TelemetryBlock& tel);

    const char* m_statePath = nullptr;
    uint32_t m_intervalMs   = 1000;
    uint32_t m_sampleRate   = 48000;
    uint32_t m_bufferSize   = 128;
    std::atomic<bool> m_running{false};
    std::thread m_workerThread;

    /* Applied config state — updated only when reload thread confirms changes. */
    int m_appliedVolume = -1;       /* -1 = unknown/not yet applied */
    bool m_appliedMuted = false;
    char m_appliedInput[kMaxPcmName]  = "";
    char m_appliedOutput[kMaxPcmName] = "";
    char m_appliedRcInput[kMaxPcmName]  = "";
    char m_appliedRcOutput[kMaxPcmName] = "";

    /* Per-chain DSP applied state. */
    AppliedCrossover m_crossover[kMaxChains] = {};
    AppliedLimiter   m_limiter[kMaxChains]   = {};
    AppliedDriver    m_drivers[kMaxDrivers];
    uint32_t         m_driverCount = 0;

    /* Latest-snapshot: writer always reads the freshest block.
     * Atomic version tag prevents torn reads at 100Hz production rate. */
    struct Snapshot {
        TelemetryBlock data{};
        std::atomic<uint32_t> version{0};

        void store(const TelemetryBlock& t);
        bool load(TelemetryBlock* out) const;
    };
    Snapshot m_snapshot;
};
