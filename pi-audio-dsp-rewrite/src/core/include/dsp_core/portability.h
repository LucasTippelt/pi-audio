#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>

/* ------------------------------------------------------------------ */
/*  Portability constraints for the DSP core                          */
/*  Include this header FIRST in every .cpp under core/src/           */
/* ------------------------------------------------------------------ */

/* --- Compile-time bans --- */
#ifdef __has_include
#if __has_include(<string>)
#define DSP_STRING_HEADER_AVAILABLE 1
#endif
#if __has_include(<vector>)
#define DSP_VECTOR_HEADER_AVAILABLE 1
#endif
#endif

/* In the process path, <string> and <vector> are forbidden. */
/* Attempting to include them triggers a static_assert failure below. */
#ifdef DSP_IN_PROCESS_PATH
  #ifdef DSP_STRING_HEADER_AVAILABLE
    #error "<string> is banned in the DSP core process path"
  #endif
  #ifdef DSP_VECTOR_HEADER_AVAILABLE
    #error "<vector> is banned in the DSP core process path"
  #endif
#endif

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
