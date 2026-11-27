// Runs every registered test (or only those whose name contains argv[1]).
#include <cstring>

#include "common/clock.h"
#include "net.h"  // project 1
#include "test.h"

int main(int argc, char** argv) {
    net::ignore_sigpipe();
    hp::init_clock();
    int run = 0, failed = 0;
    for (const auto& c : t::registry()) {
        if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
        const int before = t::failures();
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        c.fn();
        const bool ok = t::failures() == before;
        std::printf("[ %s ] %s\n", ok ? " OK " : "FAIL", c.name);
        ++run;
        failed += ok ? 0 : 1;
    }
    std::printf("\n%d tests, %ld checks, %d failed tests, %d failed checks\n", run, t::checks(), failed,
                t::failures());
    return failed == 0 && run > 0 ? 0 : 1;
}
