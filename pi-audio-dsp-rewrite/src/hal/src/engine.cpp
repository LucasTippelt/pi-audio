#include "hal/engine.h"
#include "hal/hal.h"
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"
#include "stages/gain_stage.h"

#include <cstdio>
#include <cstring>

Engine::Engine() : m_core(new DspCore()) {}
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
