#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Engine — HAL-side orchestrator (A1)                               */
/*  Owns DSP core, command/telemetry queues, onBlock() callback       */
/* ------------------------------------------------------------------ */

class DspCore;
struct BlockInfo;
struct TelemetryBlock;

template<typename T, size_t C> class CmdQueue;

class Engine {
public:
    Engine();
    ~Engine();

    /* Called by HAL once per audio block (A1).
     * This is the ONLY entry point from the HAL into the Engine. */
    void onBlock(float* const in[],  uint32_t numInCh,
                 float* const out[], uint32_t numOutCh,
                 const BlockInfo& info);

    /* Control plane registers its command/telemetry queues here. */
    template<typename T, size_t C>
    void setCommandQueue(CmdQueue<T, C>* cmdQ) { m_cmdQueue = cmdQ; }

    template<typename T, size_t C>
    void setTelemetryQueue(CmdQueue<T, C>* telQ) { m_telQueue = telQ; }

    /* Get core reference for stage chain access. */
    DspCore& core();

private:
    DspCore* m_core = nullptr;
    void* m_cmdQueue   = nullptr; /* opaque pointer to CmdQueue */
    void* m_telQueue   = nullptr; /* opaque pointer to CmdQueue<TelemetryBlock> */

    uint64_t m_sampleIndex = 0;

    void drainCommands();     /* apply pending commands at block boundary */
    void pushTelemetry(const BlockInfo& info);
};
