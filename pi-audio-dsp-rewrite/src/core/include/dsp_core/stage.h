#pragma once
#include <cstdint>

/* ------------------------------------------------------------------ */
/*  Stage interface — no std::function, no heap (A4)                  */
/* ------------------------------------------------------------------ */

struct BlockInfo; /* forward */

struct Stage {
    virtual ~Stage() = default;

    /* Process in-place on planar float buffer. No allocation allowed. */
    virtual void process(float* const channels[], uint32_t numChannels,
                         uint32_t frames, const BlockInfo& info) = 0;

    /* Declared group delay in samples (F4). */
    virtual uint32_t latencySamples() const = 0;
};

/* Opaque handle for double-buffered stage swap (I2 prep, C3). */
struct StageSet {
    static constexpr uint8_t kMaxStages = 8;

    Stage* m_active[kMaxStages]   = {};
    Stage* m_pending[kMaxStages]  = {};
    uint8_t m_count               = 0;
    uint8_t m_pendingCount        = 0;

    void addPending(Stage* s) {
        if (m_pendingCount < kMaxStages)
            m_pending[m_pendingCount++] = s;
    }

    /* Called in Engine::onBlock() before core.process(). */
    void swap() {
        for (uint8_t i = 0; i < m_pendingCount; ++i)
            m_active[i] = m_pending[i];
        m_count = m_pendingCount;
        m_pendingCount = 0;
    }
};
