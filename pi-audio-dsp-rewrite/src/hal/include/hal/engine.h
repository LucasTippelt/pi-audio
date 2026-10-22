#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>

/* ------------------------------------------------------------------ */
/*  Engine — HAL-side orchestrator (A1)                               */
/*  Owns DSP core, command/telemetry queues, onBlock() callback       */
/* ------------------------------------------------------------------ */

class DspCore;
class GainStage;
#include "stages/crossover_stage.h"
#include "stages/eq_chain_stage.h"
#include "stages/limiter_stage.h"
#include "stages/trim_stage.h"
class AudioHal;
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

    /* C1: Register test command queue for CmdSetTestMode. */
    template<typename T, size_t C>
    void setTestCommandQueue(CmdQueue<T, C>* testCmdQ) { m_testCmdQueue = testCmdQ; }

    /* Control command queue for real-time gain/mute commands. */
    template<typename T, size_t C>
    void setControlCommandQueue(CmdQueue<T, C>* ctrlQ) { m_ctrlCmdQueue = ctrlQ; }

    /* Gain stage access for command routing (O(1), RT-safe). */
    void setGainStages(void* tweeterGain, void* wooferGain);

    /* Get core reference for stage chain access. */
    DspCore& core();

    /* B4: Set HAL reference for xrun counter telemetry plumbing. */
    void setHal(AudioHal* hal) { m_hal = hal; }

    /* C1: Test mode — atomic, read by DSP core in onBlock().
     * 0=none, 1=woofer, 2=tweeter, 3=pgm-l */
    uint8_t getTestMode() const { return m_testMode.load(); }

    /* P4: Crossover stage access for testing. */
    CrossoverStage& crossoverWoofer() { return m_crossoverWoofer; }
    CrossoverStage& crossoverTweeter() { return m_crossoverTweeter; }
    bool isCrossoverActive() const { return m_crossoverActive; }

    /* EQ stage access. */
    EqChainStage& eqWoofer() { return m_eqWoofer; }
    EqChainStage& eqTweeter() { return m_eqTweeter; }

    /* Limiter stage access. */
    LimiterStage& limiterWoofer() { return m_limiterWoofer; }
    LimiterStage& limiterTweeter() { return m_limiterTweeter; }

    /* Trim stage access (includes phase inversion). */
    TrimStage& trimWoofer() { return m_trimWoofer; }
    TrimStage& trimTweeter() { return m_trimTweeter; }

private:
    DspCore* m_core = nullptr;
    AudioHal* m_hal  = nullptr; /* B4: optional, for xrun counters in telemetry */
    void* m_cmdQueue   = nullptr; /* opaque pointer to CmdQueue<CmdLoadParams> */
    void* m_telQueue   = nullptr; /* opaque pointer to CmdQueue<TelemetryBlock> */
    void* m_testCmdQueue = nullptr; /* C1: opaque pointer to CmdQueue<CmdSetTestMode> */
    void* m_ctrlCmdQueue = nullptr; /* Control queue for gain/mute commands */

    GainStage* m_tweeterGain = nullptr; /* Direct pointers for O(1) command routing */
    GainStage* m_wooferGain  = nullptr;

    /* P4: Crossover stages — one per output chain. */
    CrossoverStage m_crossoverWoofer;
    CrossoverStage m_crossoverTweeter;
    bool m_crossoverActive = false;

    /* EQ stages — one per output chain, before crossover in the DSP chain. */
    EqChainStage m_eqWoofer;
    EqChainStage m_eqTweeter;

    /* Limiter stages — one per output chain, after crossover in the DSP chain. */
    LimiterStage m_limiterWoofer;
    LimiterStage m_limiterTweeter;

    /* Trim stages — one per output chain (gain + phase inversion). */
    TrimStage m_trimWoofer;
    TrimStage m_trimTweeter;

    uint64_t m_sampleIndex = 0;

    /* C1: Atomic test mode — set via CmdSetTestMode, read by DSP core. */
    std::atomic<uint8_t> m_testMode{0};

    void drainCommands();     /* apply pending commands at block boundary */
    void pushTelemetry(const BlockInfo& info);
    void applyCrossoverParams(const uint8_t* data, uint32_t len);
};
