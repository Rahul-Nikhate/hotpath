// Minimal self-registering test framework (no dependencies), same as project 1.
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    void (*fn)();
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline long& checks() {
    static long c = 0;
    return c;
}
struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void check(bool ok, const char* expr, const char* file, int line) {
    ++checks();
    if (!ok) {
        ++failures();
        std::printf("    FAIL %s:%d: %s\n", file, line, expr);
    }
}

}  // namespace t

#define TEST(name)                                          \
    static void name();                                     \
    static const t::Registrar reg_##name(#name, &name);     \
    static void name()

#define CHECK(cond) t::check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) t::check((a) == (b), #a " == " #b, __FILE__, __LINE__)

// Sanitizer detection, so slow or fork-based tests can scale themselves down.
#if defined(__SANITIZE_THREAD__)
#define HP_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define HP_TSAN 1
#endif
#endif
#ifndef HP_TSAN
#define HP_TSAN 0
#endif
