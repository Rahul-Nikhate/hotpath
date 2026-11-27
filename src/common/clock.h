// Timing primitives for the hot path.
//
// cycles() reads the cheapest monotonic counter the CPU offers:
//   x86-64  : RDTSC (invariant TSC on any modern server CPU; ~7 ns to read)
//   AArch64 : CNTVCT_EL0 (generic timer; 24 MHz on Apple Silicon, so ~42 ns
//             resolution - fine for development, too coarse for sub-50 ns stages)
//   other   : steady_clock
// RDTSC is deliberately not fenced: a stage timestamp may drift by a few cycles
// under out-of-order execution, which is far below what we report. Benchmarks
// that time tiny regions in isolation use the fenced variant.
//
// now_ns() is steady_clock: CLOCK_MONOTONIC on Linux, mach_absolute_time on
// macOS. Both are host-wide, so timestamps from different processes compare.
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace hp {

inline uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

inline uint64_t cycles() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#else
    return now_ns();
#endif
}

// Serialised read for timing very small regions (bench only).
inline uint64_t cycles_fenced() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_lfence();
    const uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
#elif defined(__aarch64__)
    uint64_t v;
    asm volatile("isb; mrs %0, cntvct_el0; isb" : "=r"(v)::"memory");
    return v;
#else
    return now_ns();
#endif
}

inline void cpu_relax() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#endif
}

// Nanoseconds per cycle, measured once at startup (init_clock) so converting
// on the hot path is a single multiply with no static-init guard.
inline double g_ns_per_cycle = 1.0;

inline double calibrate_ns_per_cycle(int ms = 20) {
    const uint64_t c0 = cycles(), t0 = now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    const uint64_t c1 = cycles(), t1 = now_ns();
    return c1 > c0 ? double(t1 - t0) / double(c1 - c0) : 1.0;
}

inline void init_clock() { g_ns_per_cycle = calibrate_ns_per_cycle(); }

inline uint64_t cycles_to_ns(uint64_t c) { return uint64_t(double(c) * g_ns_per_cycle + 0.5); }

}  // namespace hp
