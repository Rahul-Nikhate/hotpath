// Single-writer, multi-reader seqlock. The writer never waits for readers;
// readers retry if they overlap a write. Used for the control block (risk
// limits + kill flag) that the control process writes and the fast path reads.
//
// Protocol (fence-free variant):
//   writer: seq = odd (relaxed) ; each data word stored with RELEASE ; seq = even (release)
//   reader: s1 = seq (acquire) ; each data word loaded with ACQUIRE ; s2 = seq (relaxed) ;
//           retry if s1 is odd or s1 != s2
// Why it is correct: if the reader observes any word from a newer write, that
// acquire load synchronizes with the word's release store, so the writer's
// earlier "seq = odd" store is visible and s2 != s1 forces a retry. Acquire
// loads also keep the s2 load from moving above the data loads.
//
// The classic version uses standalone atomic_thread_fence calls instead;
// ThreadSanitizer cannot model those (GCC rejects them under -fsanitize=thread),
// so per-word ordering is used. On x86 acquire/release are plain MOVs: same cost.
// The data is stored as 64-bit atomics rather than copied with memcpy: a plain
// memcpy that overlaps a write is formally a data race (UB) even though the
// torn copy is thrown away.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "common/clock.h"

namespace hp {

template <class T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;

public:
    // Single writer only.
    void store(const T& v) {
        uint64_t buf[kWords] = {};
        std::memcpy(buf, &v, sizeof(T));
        const uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        for (size_t i = 0; i < kWords; ++i) words_[i].store(buf[i], std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }

    // One attempt; false if a write was in progress or happened meanwhile.
    bool try_load(T& out) const {
        const uint64_t s1 = seq_.load(std::memory_order_acquire);
        if (s1 & 1) return false;
        uint64_t buf[kWords];
        for (size_t i = 0; i < kWords; ++i) buf[i] = words_[i].load(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) != s1) return false;
        std::memcpy(&out, buf, sizeof(T));
        return true;
    }

    T load() const {
        T v;
        while (!try_load(v)) cpu_relax();
        return v;
    }

    // Cheap change detection: one load. Even values are stable versions.
    uint64_t sequence() const { return seq_.load(std::memory_order_acquire); }

private:
    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) std::atomic<uint64_t> words_[kWords] = {};
};

}  // namespace hp
