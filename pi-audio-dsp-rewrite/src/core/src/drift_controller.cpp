#include "dsp_core/drift_controller.h"

#include <cmath>

DriftController::DriftController()
    : m_ratio(1.0f), m_integrator(0.0f), m_prevError(0.0f), m_count(0) {}

void DriftController::update(uint32_t ringFill, uint32_t ringCapacity) {
    if (ringCapacity == 0) return;

    /* Target fill is half capacity — steady state when clocks match. */
    float targetFill = static_cast<float>(ringCapacity) * 0.5f;
    float error = static_cast<float>(ringFill) - targetFill;
    float normError = error / static_cast<float>(ringCapacity);

    /* Anti-windup: clamp integrator contribution. */
    float iTerm = m_integrator + normError;
    if (iTerm > 1.0f) iTerm = 1.0f;
    if (iTerm < -1.0f) iTerm = -1.0f;

    /* PI correction — small adjustment to ratio. */
    float correction = Kp * normError + Ki * iTerm;

    /* Slew limit: constrain per-block change to avoid audible pitch shift. */
    m_count++;
    float maxDelta = kSlewLimitPpm * static_cast<float>(m_count);
    if (correction > maxDelta) correction = maxDelta;
    if (correction < -maxDelta) correction = -maxDelta;

    m_ratio = 1.0f + correction;
    m_integrator = iTerm;
}

float DriftController::getRatio() const {
    return m_ratio;
}

void DriftController::reset() {
    m_ratio = 1.0f;
    m_integrator = 0.0f;
    m_prevError = 0.0f;
    m_count = 0;
}
