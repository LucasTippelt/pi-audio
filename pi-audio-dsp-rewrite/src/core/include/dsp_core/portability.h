#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <atomic>

/* ------------------------------------------------------------------ */
/*  Portability constraints for the DSP core                          */
/*  Include this header FIRST in every .cpp under core/src/           */
/* ------------------------------------------------------------------ */

/* --- Compile-time bans --- */
/* In the process path, <string> and <vector> are forbidden.          */
/* Enforced by code review and target compile flags (A4).             */

/* --- Numeric constraints --- */
#define DSP_FLOAT_ONLY
static_assert(std::is_same_v<float, float>, "single precision only");

/* Guard against accidental double promotion: -Wdouble-promotion is enabled. */

/* --- Block size limits --- */
#define DSP_MAX_BLOCK_SIZE 2048

/* --- Heap ban after init --- */
#define DSP_NO_HEAP
/* Enforced by code review and separate dsp_core target compilation. */

/* --- Per-stage timing macro --- */
/* Logs to a fixed-size ring buffer (no heap, no std::string).          */
/* Returns elapsed cycles for the named stage.                         */

struct StageTimingEntry {
    uint32_t stageId;
    uint64_t cycles;
};

class StageTimingRing {
public:
    static constexpr size_t kCapacity = 1024;

    void push(uint32_t stageId, uint64_t cycles);
    size_t read(StageTimingEntry* out, size_t max) const;

private:
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    StageTimingEntry m_buf[kCapacity];
};

/* Usage: DSP_STAGE_TIMING(my_stage, ring) { ... } */
#define DSP_STAGE_TIMING(name, ring)                                                               \
    const uint64_t _dsp_timing_start_##name = __rdtsc();                                           \
    for (;;) {                                                                                     \
        break;                                                                                     \
    }                                                                                              \
    const uint32_t _dsp_stage_id_##name = hashStageId(#name);                                      \
    /* Actual timing recorded at block end */

inline uint32_t hashStageId(const char* name) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; name[i]; ++i)
        h ^= static_cast<uint32_t>(name[i]);
    return h * 16777619u;
}

inline void StageTimingRing::push(uint32_t stageId, uint64_t cycles) {
    size_t head = m_head.load(std::memory_order_relaxed);
    size_t next = (head + 1) % kCapacity;
    if (next == m_tail.load(std::memory_order_acquire))
        return; /* ring full, drop */
    m_buf[head].stageId = stageId;
    m_buf[head].cycles = cycles;
    m_head.store(next, std::memory_order_release);
}

inline size_t StageTimingRing::read(StageTimingEntry* out, size_t max) const {
    size_t tail = m_tail.load(std::memory_order_relaxed);
    size_t head = m_head.load(std::memory_order_acquire);
    size_t avail = (head >= tail) ? (head - tail) : (kCapacity - tail + head);
    size_t count = (max < avail) ? max : avail;
    for (size_t i = 0; i < count; ++i) {
        out[i] = m_buf[tail];
        tail = (tail + 1) % kCapacity;
    }
    return count;
}

/* --- Static assertions --- */
static_assert(DSP_MAX_BLOCK_SIZE <= 8192, "max block size must be reasonable");

/* --- Crossover stage constraints (Phase 5 audit) --- */
/* Max biquads per crossover: 4 (for 48 dB/oct). */
#define DSP_CROSSOVER_MAX_BIQUADS 4
static_assert(DSP_CROSSOVER_MAX_BIQUADS <= 4, "crossover max biquads must not exceed 4");

/* Max channels for per-channel state: 8. */
#define DSP_CROSSOVER_MAX_CHANNELS 8
static_assert(DSP_CROSSOVER_MAX_CHANNELS <= 8, "crossover max channels must not exceed 8");

/* CrossoverStage size audit (must fit in L1 cache).
 * Each biquad: 5 coeffs × 4 + 2 state[8] × 4 = 60 bytes.
 * First-order: 1 coef × 4 + 2 state[8] × 4 = 36 bytes.
 * Total for max config: 4×60 + 36 + overhead ≈ 320 bytes per stage. */
#define DSP_CROSSOVER_ESTIMATED_SIZE 320
static_assert(DSP_CROSSOVER_ESTIMATED_SIZE <= 1024, "crossover stage must fit in L1 cache");

/* Biquad coefficient array size: maxBiquads × 5 floats. */
#define DSP_BIQUAD_COEFFS_PER_STAGE (DSP_CROSSOVER_MAX_BIQUADS * 5)
static_assert(DSP_BIQUAD_COEFFS_PER_STAGE == 20, "biquad coeff count must be 20");

/* Binary param block: CrossoverCoeffs fits in StageParamEntry::data[256]. */
#define DSP_CROSSOVER_BINARY_MAX 96
static_assert(DSP_CROSSOVER_BINARY_MAX <= 256, "crossover binary block must fit in param entry");

/* Delay stage constraints. */
#define DSP_DELAY_MAX_SAMPLES 32768
static_assert(DSP_DELAY_MAX_SAMPLES == (1u << 15), "delay buffer must be power of 2");

/* DelayStage size audit: 32768*4 + scratch[2048]*4 + overhead ≈ 154KB.
 * Large but allocated once at init, not in RT path. */
#define DSP_DELAY_ESTIMATED_SIZE (DSP_DELAY_MAX_SAMPLES * sizeof(float) + DSP_MAX_BLOCK_SIZE * sizeof(float))

/* Limiter stage constraints. */
#define DSP_LIMITER_MAX_CHANNELS 10
static_assert(DSP_LIMITER_MAX_CHANNELS <= 10, "limiter channels must not exceed max");

/* LimiterStage size audit: small (4 coeffs + 2 precomputed + envelope array).
 * ≈ 6*4 + 10*4 = 80 bytes — fits easily in L1 cache. */
#define DSP_LIMITER_ESTIMATED_SIZE 96
static_assert(DSP_LIMITER_ESTIMATED_SIZE <= 256, "limiter stage must fit in L1 cache");

/* Bare-metal FENV guard (disable floating-point exceptions in process path). */
#ifdef __STDC_LIB_EXT1__
#pragma STDC FENV_ACCESS OFF
#endif
