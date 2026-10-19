#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  DriftController — PI controller for clock-ratio estimation (B3)   */
/*  Estimates source/dest clock ratio from ring-buffer fill level.     */
/*  Slew-limited to avoid audible pitch modulation.                    */
/* ------------------------------------------------------------------ */

class DriftController {
public:
    DriftController();

    /* Call once per block with current ring state. */
    void update(uint32_t ringFill, uint32_t ringCapacity);

    /* Returns current clock-ratio estimate (1.0 ± ppm), slew-limited. */
    float getRatio() const;

    /* Reset to unity ratio and clear integrator. */
    void reset();

private:
    float m_ratio;          /* Current output ratio (starts at 1.0) */
    float m_integrator;     /* PI integral term */
    float m_prevError;      /* Previous fill-level error for anti-windup */
    uint32_t m_count;       /* Block counter for slew limiting */

    /* PI gains — tuned slow: convergence in ~5s for +30 ppm step. */
    static constexpr float Kp = 0.01f;
    static constexpr float Ki = 0.0001f;

    /* Slew limit: max ratio change per block (ppm). Prevents pitch modulation. */
    static constexpr float kSlewLimitPpm = 50e-6f;
};
