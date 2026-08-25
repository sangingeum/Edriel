/**
 * @file EdrielRing.hpp
 * @brief Bounded single-producer/single-consumer ring with LIFO-style
 *        overwrite-oldest (LMO) semantics, for the ADR-003 sharded receive
 *        pipeline.
 *
 * Topology (ADR-003 §2): each worker owns exactly one ring. The multicast
 * receive thread is the ONE producer; the owning worker thread is the ONE
 * consumer — a strict SPSC pair. The queue is bounded (`rx_ring_slots` slots,
 * a power of two).
 *
 * Overflow policy (ADR-003 owner decision #4): when the ring is full the
 * producer overwrites the OLDEST unconsumed slot (drop-oldest, "latest wins")
 * and bumps an observable drop counter; it never blocks and never writes past
 * the consumer's read position. This is explicitly NOT strict FIFO once full —
 * older frames are evicted, retained frames stay in order — and is suitable
 * for best-effort QoS.
 *
 * Thread-safety: the fast path (push/pop of a non-full/non-empty ring) and the
 * overflow path are serialized by a small per-ring mutex + condition variable,
 * so the LMO-overwrite cannot race a concurrent consumer read of the same
 * slot. This trades a few ns of uncontended mutex for provable correctness
 * (TSan-clean) and a simple, correctly-implemented overwrite; the mutex is
 * dominated by the downstream parse/dispatch cost on the worker. The SPSC
 * topology still guarantees the per-(publisher,topic) ordering contract for
 * non-dropped frames.
 *
 * The element type `T` is stored via std::unique_ptr<T>, so the queue owns each
 * payload and hands it to the consumer by move.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace edriel {

template <typename T>
class SpscRing {
public:
    /// @param capacity Number of slots; a power of two is expected by callers
    ///                  (ADR-003 config `rx_ring_slots`), but any positive
    ///                  value is handled arithmetically.
    explicit SpscRing(std::size_t capacity)
        : capacity_(capacity), slots_(capacity) {}

    /// Producer (receiver thread): enqueue `item`. If the ring is full, drop
    /// the oldest slot (overwrite it with the new item), count the drop, and
    /// return true. Anything already closed is silently dropped.
    void publish(std::unique_ptr<T> item) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (closed_ || !item) {
            return;
        }
        const bool wasEmpty = (head_ == tail_);
        if (head_ - tail_ < capacity_) {
            slots_[index(head_)] = std::move(item);
            ++head_;
        } else {
            // Full: overwrite the OLDEST unconsumed slot (at the read index).
            // Latest-wins per ADR-003 decision #4; indices unchanged so the
            // consumer still drains this slot (now the newest frame) next.
            slots_[index(tail_)] = std::move(item);
            ++dropped_;
        }
        if (wasEmpty) {
            cv_.notify_one();
        }
    }

    /// Consumer (worker thread): block until an item is available, then return
    /// it (moving it out). Returns nullptr once the ring is closed AND drained.
    std::unique_ptr<T> pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return head_ != tail_ || closed_; });
        if (head_ == tail_) {
            return nullptr;  // closed (and drained)
        }
        std::unique_ptr<T> item = std::move(slots_[index(tail_)]);
        ++tail_;
        return item;
    }

    /// Non-blocking variant; returns nullptr when empty. Used for shutdown
    /// drains/telemetry only.
    std::unique_ptr<T> tryPop() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (head_ == tail_) {
            return nullptr;
        }
        std::unique_ptr<T> item = std::move(slots_[index(tail_)]);
        ++tail_;
        return item;
    }

    /// Permanently close the ring: no further publishes enqueue, and every
    /// blocked/future pop() returns nullptr after the backlog is drained.
    void close() {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

    /// Number of items physically waiting to be consumed.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return static_cast<std::size_t>(head_ - tail_);
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return head_ == tail_;
    }

    /// Capacity this ring was constructed with.
    std::size_t capacity() const { return capacity_; }

    /// Cumulative count of oldest-ever-dropped frames (never silent).
    std::uint64_t dropped() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return dropped_;
    }

private:
    std::size_t index(std::uint64_t position) const {
        return static_cast<std::size_t>(position % capacity_);
    }

    std::size_t capacity_;
    std::vector<std::unique_ptr<T>> slots_;

    // Guards all state. The fast path is a cheap uncontended lock; the LMO
    // overflow path mutates the consumer's slot and must not race a read.
    mutable std::mutex mtx_;
    std::condition_variable cv_;

    std::uint64_t head_ = 0;   ///< producer insert position (next free slot)
    std::uint64_t tail_ = 0;   ///< consumer remove position (next read)
    std::uint64_t dropped_ = 0;
    bool closed_ = false;
};

}  // namespace edriel