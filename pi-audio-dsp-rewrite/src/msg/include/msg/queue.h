#pragma once
#include <atomic>
#include <cstddef>

/* ------------------------------------------------------------------ */
/*  Lock-free SPSC ring queue (A3 transport)                          */
/*  Push from control plane, pop in Engine::onBlock()                 */
/* ------------------------------------------------------------------ */

template<typename T, size_t Capacity>
class CmdQueue {
public:
    CmdQueue() = default;
    CmdQueue(const CmdQueue&) = delete;
    CmdQueue& operator=(const CmdQueue&) = delete;

    /* Returns false if full (control plane drops or retries). */
    bool push(const T& cmd) {
        size_t head = m_head.load(std::memory_order_relaxed);
        size_t next = (head + 1) % Capacity;
        if (next == m_tail.load(std::memory_order_acquire))
            return false; /* full */
        m_buf[head] = cmd;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    /* Drains up to max commands. Returns actual count popped. */
    size_t pop(T* out, size_t max) {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);
        if (tail == head)
            return 0; /* empty */
        size_t avail = (head >= tail) ? (head - tail) : (Capacity - tail + head);
        size_t count = (max < avail) ? max : avail;
        for (size_t i = 0; i < count; ++i) {
            out[i] = m_buf[tail];
            tail = (tail + 1) % Capacity;
        }
        m_tail.store(tail, std::memory_order_release);
        return count;
    }

    /* Non-blocking peek at pending item count. */
    size_t pending() const {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        size_t head = m_head.load(std::memory_order_acquire);
        return (head >= tail) ? (head - tail) : (Capacity - tail + head);
    }

private:
    alignas(64) std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    T m_buf[Capacity];
};
