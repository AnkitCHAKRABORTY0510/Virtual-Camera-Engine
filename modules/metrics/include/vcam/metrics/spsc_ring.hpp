// =============================================================================
// spsc_ring.hpp — lock-free single-producer / single-consumer queue
//
// The pacer thread (producer) hands one timing record per frame to the
// control thread (consumer) without ever taking a lock or allocating memory.
//
// How it works: a fixed array plus two counters. Only the producer writes
// `head_`, only the consumer writes `tail_`. The release store of a counter
// publishes the element written before it; the matching acquire load on the
// other side makes that element visible. If the queue is full, push() returns
// false instead of waiting (the pacer must never block).
// =============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace vcam {

template <typename T>
class SpscRing {
public:
    // Capacity is rounded up to a power of two so `index & mask` replaces `%`.
    explicit SpscRing(size_t capacity) {
        size_t size = 1;
        while (size < capacity) {
            size <<= 1;
        }
        items_.resize(size);
        mask_ = size - 1;
    }

    // Producer only. False when full (the item is dropped).
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail > mask_) {
            return false;
        }
        items_[head & mask_] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. False when empty.
    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = items_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t capacity() const { return mask_ + 1; }

private:
    std::vector<T> items_;
    size_t mask_ = 0;
    // alignas(64): keep the two counters on different cache lines so the two
    // threads do not slow each other down ("false sharing").
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace vcam
