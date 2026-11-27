// Shared helpers for hp_bench subcommands. Every subcommand prints a table and,
// with --csv DIR, writes DIR/<experiment>.csv for scripts/plot.py.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/args.h"
#include "common/histogram.h"

namespace bench {

struct Csv {
    FILE* f = nullptr;
    Csv(const hp::Args& a, const std::string& name, const char* header) {
        if (!a.has("csv")) return;
        const std::string dir = a.str("csv", "results");
        std::string cmd = "mkdir -p '" + dir + "'";
        if (std::system(cmd.c_str()) != 0) return;
        f = std::fopen((dir + "/" + name + ".csv").c_str(), "w");
        if (f) std::fprintf(f, "%s\n", header);
    }
    ~Csv() {
        if (f) std::fclose(f);
    }
    template <class... T>
    void row(const char* fmt, T... v) {
        if (f) std::fprintf(f, fmt, v...);
    }
};

// Pin the calling thread if --cpus lists cores (Linux only; no-op elsewhere).
bool pin(const hp::Args& a, int index);

int run_queue(const hp::Args& a);
int run_fastpath(const hp::Args& a);
int run_encode(const hp::Args& a);
int run_dispatch(const hp::Args& a);
int run_pages(const hp::Args& a);

}  // namespace bench
