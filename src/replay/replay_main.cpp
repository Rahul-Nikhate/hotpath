// hp_replay journal.bin [--runs N]: re-run the decision core on a recorded
// journal and check it reproduces the live decisions exactly.
#include <cstdio>

#include "common/args.h"
#include "replay/replay.h"

using namespace hp;

int main(int argc, char** argv) {
    Args a(argc, argv);
    const std::string path = a.positional().empty() ? a.str("journal", "journal.bin") : a.positional();
    const uint64_t runs = a.u64("runs", 1);
    const auto recs = read_journal(path);
    if (recs.empty()) {
        std::fprintf(stderr, "cannot read journal %s\n", path.c_str());
        return 1;
    }
    bool all = true;
    uint64_t first_hash = 0;
    for (uint64_t i = 0; i < runs; ++i) {
        const ReplayResult r = replay(recs);
        if (i == 0) {
            first_hash = r.replay_hash;
            std::printf("journal %s: %llu records, %llu market msgs\n", path.c_str(), (unsigned long long)r.records,
                        (unsigned long long)r.market_msgs);
            std::printf("live   : %llu orders, %llu risk rejects, hash %016llx\n", (unsigned long long)r.live_orders,
                        (unsigned long long)r.live_rejects, (unsigned long long)r.live_hash);
            std::printf("replay : %llu orders, %llu risk rejects, hash %016llx\n", (unsigned long long)r.replay_orders,
                        (unsigned long long)r.replay_rejects, (unsigned long long)r.replay_hash);
        }
        const bool ok = r.match && r.replay_hash == first_hash;
        if (!ok) std::printf("run %llu: MISMATCH %s\n", (unsigned long long)i + 1, r.error.c_str());
        all = all && ok;
    }
    std::printf("%s across %llu run(s)\n", all ? "IDENTICAL" : "DIFFERENT", (unsigned long long)runs);
    return all ? 0 : 1;
}
