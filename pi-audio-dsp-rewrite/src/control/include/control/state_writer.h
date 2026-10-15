#pragma once

/* ------------------------------------------------------------------ */
/*  State writer — periodic engine-state.json writer                  */
/* ------------------------------------------------------------------ */

class TelemetryBlock;

class StateWriter {
public:
    /* Start background thread that polls telemetry and writes state file. */
    void start(const char* statePath, uint32_t intervalMs);

    /* Stop the background thread. */
    void stop();

    /* Push a telemetry snapshot for the writer to persist. */
    void pushTelemetry(const TelemetryBlock& tel);

private:
    const char* m_statePath = nullptr;
    uint32_t m_intervalMs   = 1000;
    bool m_running          = false;
};
