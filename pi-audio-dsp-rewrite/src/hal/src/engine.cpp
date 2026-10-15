#include "hal/engine.h"
#include "dsp_core/dsp_core.h"
#include "dsp_core/block_info.h"
#include "msg/command.h"
#include "msg/telemetry.h"
#include "msg/queue.h"

#include <cstring>

Engine::Engine() : m_core(new DspCore()) {}
Engine::~Engine() { delete m_core; }

DspCore& Engine::core() { return *m_core; }

void Engine::onBlock(float* const in[],  uint32_t numInCh,
                     float* const out[], uint32_t numOutCh,
                     const BlockInfo& info) {
    /* Drain pending commands at block boundary. */
    drainCommands();

    /* Dispatch to DSP core. */
    m_core->process(in, numInCh, out, numOutCh, info.frames, info);

    /* Push telemetry snapshot. */
    pushTelemetry(info);
}

void Engine::drainCommands() {
    /* Commands are applied at block boundaries. The actual queue type is
     * determined by the control plane. For now, we track sample index. */
    m_sampleIndex += 0; /* placeholder — frames added via BlockInfo */
}

void Engine::pushTelemetry(const BlockInfo& info) {
    if (!m_telQueue) return;

    TelemetryBlock tel;
    m_core->getTelemetry(tel);
    tel.seq = static_cast<uint32_t>(info.sampleIndex);

    /* Push to telemetry queue (type-erased, so we use a concrete cast). */
    CmdQueue<TelemetryBlock, 8>* q = static_cast<CmdQueue<TelemetryBlock, 8>*>(m_telQueue);
    q->push(tel);
}
