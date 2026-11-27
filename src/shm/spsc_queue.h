// Bounded single-producer / single-consumer ring buffer.
//
// Safe to place in shared memory between two processes: it holds no pointers,
// only lock-free 64-bit atomics and trivially copyable slots.
//
// Correctness (the part interviewers probe):
//   * Only the producer writes head_, only the consumer writes tail_, so no CAS
//     is needed: each operation finishes in a bounded number of steps (wait-free).
//   * Producer: write slot, then head_.store(release).  Consumer:
//     head_.load(acquire), then read slot. The release/acquire pair makes the
//     slot write happen-before the slot read. Symmetrically tail_ protects slot
//     reuse. Own-index loads are relaxed (a thread always sees its own writes).
//   * Indices are monotonic 64-bit counters: size = head - tail, no "full vs
//     empty" ambiguity and no wasted slot; wrap-around would take centuries.
//
// The SpscOptions template parameter exists only for the queue experiment
// (bench/bench_queue.cpp): it lets one implementation be built with or without
// padding, cached indices and seq_cst ordering, so the measured differences are
// caused by exactly one change each. Production code uses the defaults.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hp {

struct SpscOptions {
    bool padded = true;    // head/tail on separate cache lines (no false sharing)
    bool cached = true;    // keep a private copy of the other side's index
    bool seq_cst = false;  // use seq_cst instead of acquire/release (for comparison)
};

inline constexpr size_t kCacheLine = 64;

template <class T, size_t N, SpscOptions O = SpscOptions{}>
class SpscQueue {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "slots are copied with plain assignment");
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "needed for cross-process use");

    static constexpr size_t kMask = N - 1;
    static constexpr size_t kAlign = O.padded ? kCacheLine : alignof(std::atomic<uint64_t>);
    static constexpr std::memory_order kAcquire = O.seq_cst ? std::memory_order_seq_cst : std::memory_order_acquire;
    static constexpr std::memory_order kRelease = O.seq_cst ? std::memory_order_seq_cst : std::memory_order_release;

public:
    static constexpr size_t capacity() { return N; }

    // Producer thread only.
    bool try_push(const T& v) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        if constexpr (O.cached) {
            if (h - tail_cache_ == N) {                       // looks full
                tail_cache_ = tail_.load(kAcquire);           // refresh: one cross-core read
                if (h - tail_cache_ == N) return false;
            }
        } else {
            if (h - tail_.load(kAcquire) == N) return false;
        }
        buf_[h & kMask] = v;
        head_.store(h + 1, kRelease);                          // publish
        return true;
    }

    // Consumer thread only.
    bool try_pop(T& out) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        if constexpr (O.cached) {
            if (t == head_cache_) {                            // looks empty
                head_cache_ = head_.load(kAcquire);
                if (t == head_cache_) return false;
            }
        } else {
            if (t == head_.load(kAcquire)) return false;
        }
        out = buf_[t & kMask];
        tail_.store(t + 1, kRelease);                          // free the slot
        return true;
    }

    // Approximate (racy by nature); for monitoring only.
    size_t size_approx() const {
        const uint64_t h = head_.load(std::memory_order_acquire);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        return h >= t ? size_t(h - t) : 0;
    }

private:
    alignas(kAlign) std::atomic<uint64_t> head_{0};  // written by producer
    alignas(kAlign) uint64_t tail_cache_ = 0;        // producer-private
    alignas(kAlign) std::atomic<uint64_t> tail_{0};  // written by consumer
    alignas(kAlign) uint64_t head_cache_ = 0;        // consumer-private
    alignas(kCacheLine) T buf_[N];
};

}  // namespace hp
