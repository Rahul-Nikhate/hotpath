// hp_bench <experiment> [options]
//   queue     experiment 1b: queue design (mutex -> seq_cst -> acq/rel -> padded -> cached),
//             and the cost of one hop between threads vs between processes
//   fastpath  experiments 1a/3: fused fast path decision latency; sparse events with
//             and without warming; optional cache-thrashing neighbour
//   pages     experiment 2: page faults (lazy vs pre-faulted) and 4 KB vs 2 MB pages
//   encode    template patching vs encoding every field
//   dispatch  stretch experiment 6: virtual vs CRTP/template vs std::variant
//   all       everything with default settings
// Common options: --csv DIR (write CSV), --cpus a,b (pin threads, Linux)
#include <cstdio>
#include <cstring>
#include <sstream>

#include "bench/bench.h"
#include "common/clock.h"
#include "net.h"  // project 1

namespace bench {
bool pin(const hp::Args& a, int index) {
    if (!a.has("cpus")) return false;
    std::stringstream ss(a.str("cpus", ""));
    std::string tok;
    for (int i = 0; std::getline(ss, tok, ','); ++i)
        if (i == index) return net::pin_current_thread(std::stoi(tok));
    return false;
}
}  // namespace bench

int main(int argc, char** argv) {
    hp::Args a(argc, argv);
    hp::init_clock();
    const std::string what = a.positional().empty() ? "all" : a.positional();
    std::printf("hp_bench %s | ns/cycle %.3f%s\n", what.c_str(), hp::g_ns_per_cycle,
                hp::g_ns_per_cycle > 10 ? " (coarse timer: per-stage numbers below ~50 ns are quantised)" : "");
    if (what == "queue") return bench::run_queue(a);
    if (what == "fastpath") return bench::run_fastpath(a);
    if (what == "pages") return bench::run_pages(a);
    if (what == "encode") return bench::run_encode(a);
    if (what == "dispatch") return bench::run_dispatch(a);
    if (what == "all") {
        int rc = 0;
        rc |= bench::run_queue(a);
        rc |= bench::run_fastpath(a);
        rc |= bench::run_pages(a);
        rc |= bench::run_encode(a);
        rc |= bench::run_dispatch(a);
        return rc;
    }
    std::fprintf(stderr, "unknown experiment '%s' (queue | fastpath | pages | encode | dispatch | all)\n", what.c_str());
    return 1;
}
