#include "hal/engine.h"
#include "hal/hal.h"
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "msg/command.h"
#include "msg/params.h"
#include "msg/telemetry.h"
#include "msg/queue.h"
#include "stages/gain_stage.h"
#include "stages/crossover_stage.h"
#include "stages/trim_stage.h"

#include <cstdio>
#include <cstring>

Engine::Engine() : m_core(new DspCore()) {
    /* P4: Register crossover stages in output chains. */
    core().stageChain(0).addPending(&m_crossoverWoofer);
    core().stageChain(1).addPending(&m_crossoverTweeter);

    /* Register limiter stages after crossovers in the chain. */
    core().stageChain(0).addPending(&m_limiterWoofer);
    core().stageChain(1).addPending(&m_limiterTweeter);

    /* Register trim stages (gain + phase inversion) at end of each chain,
     * before master gain is applied by DspCore. */
    core().stageChain(0).addPending(&m_trimWoofer);
    core().stageChain(1).addPending(&m_trimTweeter);
}
Engine::~Engine() { delete m_core; }

DspCore& Engine::core() { return *m_core; }

void Engine::setGainStages(void* tweeterGain, void* wooferGain) {
    m_tweeterGain = static_cast<GainStage*>(tweeterGain);
    m_wooferGain  = static_cast<GainStage*>(wooferGain);
}

void Engine::onBlock(float* const in[],  uint32_t numInCh,
                     float* const out[], uint32_t numOutCh,
                     const BlockInfo& info) {
    /* Drain pending commands at block boundary. */
    drainCommands();

    /* Swap pending stages into active chains before processing. */
    for (int c = 0; c < 2; ++c) {
        m_core->stageChain(static_cast<uint8_t>(c)).swap();
    }

    /* C1: Sync test mode to DSP core. */
    m_core->setTestMode(m_testMode.load(std::memory_order_acquire));

    /* Dispatch to DSP core. */
    m_core->process(in, numInCh, out, numOutCh, info.frames, info);

    /* Push telemetry snapshot. */
    pushTelemetry(info);
}

void Engine::drainCommands() {
    /* Commands are applied at block boundaries. The actual queue type is
     * determined by the control plane. For now, we track sample index. */
    m_sampleIndex += 0; /* placeholder — frames added via BlockInfo */

    /* P4: Drain CmdLoadParams from main command queue. */
    if (m_cmdQueue) {
        CmdQueue<CmdLoadParams, 16>* q =
            static_cast<CmdQueue<CmdLoadParams, 16>*>(m_cmdQueue);
        CmdLoadParams cmds[16];
        size_t n = q->pop(cmds, 16);
        for (size_t i = 0; i < n; ++i) {
            if (cmds[i].header.type == CMD_LOAD_PARAMS && cmds[i].payloadSize > 0) {
                applyCrossoverParams(cmds[i].payload, cmds[i].payloadSize);
            }
        }
    }

    /* C1: Drain test mode commands from dedicated queue. */
    if (m_testCmdQueue) {
        CmdQueue<CmdSetTestMode, 16>* q =
            static_cast<CmdQueue<CmdSetTestMode, 16>*>(m_testCmdQueue);
        CmdSetTestMode cmds[16];
        size_t n = q->pop(cmds, 16);
        for (size_t i = 0; i < n; ++i) {
            if (cmds[i].header.type == CMD_SET_TEST_MODE) {
                m_testMode.store(cmds[i].mode, std::memory_order_release);
#ifdef PI_AUDIO_DIAG_LOG
                { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                  if (df) { std::fprintf(df, "[ctrl] CMD_SET_TEST_MODE mode=%u\n", cmds[i].mode); std::fclose(df); } }
#endif
            }
        }
    }

    /* Drain real-time control commands (gain, mute). */
    if (m_ctrlCmdQueue) {
        CmdQueue<CmdGeneric, 32>* q =
            static_cast<CmdQueue<CmdGeneric, 32>*>(m_ctrlCmdQueue);
        CmdGeneric cmds[32];
        size_t n = q->pop(cmds, 32);
        for (size_t i = 0; i < n; ++i) {
            switch (cmds[i].header.type) {
                case CMD_SET_GAIN:
                    if (m_tweeterGain) m_tweeterGain->setGain(cmds[i].gain);
                    if (m_wooferGain)  m_wooferGain->setGain(cmds[i].gain);
#ifdef PI_AUDIO_DIAG_LOG
                    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                      if (df) { std::fprintf(df, "[ctrl] CMD_SET_GAIN gain=%.4f\n", cmds[i].gain); std::fclose(df); } }
#endif
                    break;
                case CMD_SET_MUTE:
                    if (m_tweeterGain) m_tweeterGain->setMute(cmds[i].muted != 0);
                    if (m_wooferGain)  m_wooferGain->setMute(cmds[i].muted != 0);
#ifdef PI_AUDIO_DIAG_LOG
                    { FILE* df = std::fopen("/run/pi-audio/diag.log", "a");
                      if (df) { std::fprintf(df, "[ctrl] CMD_SET_MUTE muted=%u\n", cmds[i].muted); std::fclose(df); } }
#endif
                    break;
                default:
                    break;
            }
        }
    }
}

/* P4: Parse binary parameter block and apply crossover coefficients. */
void Engine::applyCrossoverParams(const uint8_t* data, uint32_t len) {
    if (len < sizeof(ParamBlockHeader)) return;

    const ParamBlockHeader* hdr = reinterpret_cast<const ParamBlockHeader*>(data);
    const uint8_t* entries = data + sizeof(ParamBlockHeader);
    uint32_t remaining = static_cast<uint32_t>(len - sizeof(ParamBlockHeader));

    for (uint32_t i = 0; i < hdr->stageCount && remaining >= 8; ++i) {
        /* Parse compact entry: type(2) + stageSlot(2) + dataLen(4) + data[dataLen] */
        uint16_t type, slot;
        std::memcpy(&type, entries, 2); entries += 2;
        std::memcpy(&slot, entries, 2); entries += 2;
        uint32_t dataLen = 0;
        std::memcpy(&dataLen, entries, 4); entries += 4;

        if (entries + dataLen > data + len) break;
        remaining -= 8 + dataLen;

        if (type == STAGE_PARAM_CROSSOVER) {
            if (dataLen == 0) {
                /* Zero-length entry: disable crossover (pass-through mode). */
                if (slot == 0) m_crossoverWoofer.disable();
                else if (slot == 1) m_crossoverTweeter.disable();
            } else if (dataLen >= 5) {
                /* P4: Crossover coefficient block.
                 * Layout: filterType(1) + topology(1) + numBiquads(1) + hasFirstOrder(1) + padding(1)
                 *         firstOrderCoef(4) + biquad sections[4] × 5 floats */
                uint8_t numBiquads = entries[2];
                uint8_t hasFirstOrder = entries[3];
                float firstOrderCoef = 0.f;
                if (hasFirstOrder && dataLen >= 10) {
                    std::memcpy(&firstOrderCoef, &entries[5], sizeof(float));
                }

                const float* biquadCoeffs = reinterpret_cast<const float*>(&entries[10]);
                uint8_t actualBiquads = (numBiquads > 4) ? 4 : numBiquads;

                if (slot == 0) {
                    /* Woofer chain — lowpass. */
                    m_crossoverWoofer.configure(biquadCoeffs, actualBiquads,
                                                hasFirstOrder != 0, firstOrderCoef);
                    m_crossoverActive = true;
                } else if (slot == 1) {
                    /* Tweeter chain — highpass. */
                    m_crossoverTweeter.configure(biquadCoeffs, actualBiquads,
                                                 hasFirstOrder != 0, firstOrderCoef);
                    m_crossoverActive = true;
                }
            }
        }

        if (type == STAGE_PARAM_LIMITER && dataLen >= 16) {
            /* Limiter params: [thresholdDb(4)] [attackMs(4)] [releaseMs(4)] [ratio(4)] */
            float thresholdDb, attackMs, releaseMs, ratio;
            std::memcpy(&thresholdDb, entries, sizeof(float));
            std::memcpy(&attackMs, entries + 4, sizeof(float));
            std::memcpy(&releaseMs, entries + 8, sizeof(float));
            std::memcpy(&ratio, entries + 12, sizeof(float));

            /* Sentinel check: threshold >= 999 dB means "disable limiter". */
            if (thresholdDb >= 999.f) {
                m_limiterWoofer.disable();
                m_limiterTweeter.disable();
            } else {
                uint32_t sr = core().getSampleRate();
                if (sr > 0) {
                    m_limiterWoofer.configure(thresholdDb, attackMs, releaseMs, ratio, sr);
                    m_limiterTweeter.configure(thresholdDb, attackMs, releaseMs, ratio, sr);
                }
            }
        }

        if (type == STAGE_PARAM_TRIM && dataLen >= 4) {
            /* Trim params: linear gain (negative = phase inverted).
             * slot 0 = woofer chain, slot 1 = tweeter chain. */
            float linearGain;
            std::memcpy(&linearGain, entries, sizeof(float));

            if (slot == 0) {
                m_trimWoofer.setGain(linearGain);
            } else if (slot == 1) {
                m_trimTweeter.setGain(linearGain);
            }
        }

        entries += dataLen;
    }
}

void Engine::pushTelemetry(const BlockInfo& info) {
    if (!m_telQueue) return;

    TelemetryBlock tel;
    m_core->getTelemetry(tel);
    tel.seq = static_cast<uint32_t>(info.sampleIndex);
    tel.clockState = info.clockState;

    /* B4: Plumb xrun counters from HAL into telemetry. */
    if (m_hal) {
        tel.xrunUnderrun = m_hal->xrunUnderrunCount();
        tel.xrunOverrun  = m_hal->xrunOverrunCount();
    }

    /* Push to telemetry queue (type-erased, so we use a concrete cast). */
    CmdQueue<TelemetryBlock, 8>* q = static_cast<CmdQueue<TelemetryBlock, 8>*>(m_telQueue);
    q->push(tel);
}
