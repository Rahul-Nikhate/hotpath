#include "common/alloc_counter.h"

#include <cstdlib>
#include <new>

#ifndef HP_NO_ALLOC_HOOK

namespace {
thread_local uint64_t t_allocs = 0;

void* counted_alloc(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* counted_aligned(std::size_t n, std::size_t align) {
    ++t_allocs;
    void* p = nullptr;
    if (align < sizeof(void*)) align = sizeof(void*);
    if (posix_memalign(&p, align, n ? n : 1) != 0) throw std::bad_alloc();
    return p;
}
}  // namespace

namespace hp {
uint64_t thread_allocations() noexcept { return t_allocs; }
bool alloc_counting_enabled() noexcept { return true; }
}  // namespace hp

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return counted_alloc(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a) { return counted_aligned(n, std::size_t(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_aligned(n, std::size_t(a)); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#else

namespace hp {
uint64_t thread_allocations() noexcept { return 0; }
bool alloc_counting_enabled() noexcept { return false; }
}  // namespace hp

#endif
