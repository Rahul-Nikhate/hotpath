// Per-thread heap allocation counter. alloc_hook.cpp replaces global operator
// new so tests and the engine can prove the hot loop allocates nothing after
// warm-up. Disabled under sanitizers (they install their own allocator), in
// which case alloc_counting_enabled() is false and counts stay at 0.
#pragma once

#include <cstdint>

namespace hp {
uint64_t thread_allocations() noexcept;
bool alloc_counting_enabled() noexcept;
}  // namespace hp
