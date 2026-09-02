#ifndef RUSC_TILDE_SPSC_RING_H
#define RUSC_TILDE_SPSC_RING_H

// Lock-free single-producer / single-consumer ring buffer for audio samples.
//
// The audio thread writes whole DSP vectors with one call; the inference thread
// drains everything available with one call. Compared with a per-sample queue
// this replaces 2 atomic operations per sample by 2 per block.
//
// Capacity is rounded up to a power of two. When the ring is full, the
// remaining samples of a write are dropped (never blocks the audio thread).

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>


template<typename T>
class SpscRing {
public:
    explicit SpscRing(std::size_t min_capacity) {
        std::size_t cap = 1;
        while (cap < min_capacity) cap <<= 1;
        m_buffer.assign(cap, T{});
        m_mask = cap - 1;
    }

    std::size_t capacity() const { return m_buffer.size(); }

    // Producer. Returns the number of samples actually written.
    std::size_t write(const T* src, std::size_t n) {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        const std::size_t free_slots = capacity() - (head - tail);
        n = std::min(n, free_slots);
        if (n == 0) return 0;

        const std::size_t pos   = head & m_mask;
        const std::size_t first = std::min(n, capacity() - pos);
        std::copy(src, src + first, m_buffer.data() + pos);
        std::copy(src + first, src + n, m_buffer.data());

        m_head.store(head + n, std::memory_order_release);
        return n;
    }

    // Consumer. Appends every available sample to `out` and returns how many.
    std::size_t read_all(std::vector<T>& out) {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t head = m_head.load(std::memory_order_acquire);
        const std::size_t n = head - tail;
        if (n == 0) return 0;

        const std::size_t pos   = tail & m_mask;
        const std::size_t first = std::min(n, capacity() - pos);
        out.insert(out.end(), m_buffer.data() + pos, m_buffer.data() + pos + first);
        out.insert(out.end(), m_buffer.data(), m_buffer.data() + (n - first));

        m_tail.store(tail + n, std::memory_order_release);
        return n;
    }

    // Consumer-side view of how many samples are waiting.
    std::size_t available() const {
        return m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_relaxed);
    }

private:
    std::vector<T>           m_buffer;
    std::size_t              m_mask = 0;
    alignas(64) std::atomic<std::size_t> m_head{0};   // written by producer
    alignas(64) std::atomic<std::size_t> m_tail{0};   // written by consumer
};


#endif // RUSC_TILDE_SPSC_RING_H
