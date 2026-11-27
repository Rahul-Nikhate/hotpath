// Experiments 1a and 3 on the real FastPath code with an in-memory feed and
// gateway (no sockets): the cost of the decision path itself, and what
// happens to it when events are sparse and a neighbour thrashes the cache,
// with and without hot-path warming.
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "bench/bench.h"
#include "common/clock.h"
#include "engine/core.h"
#include "engine/defaults.h"
#include "engine/fakes.h"
#include "engine/fast_path.h"

using namespace hp;

namespace {

struct Result {
    Histogram decision, strategy, risk, encode;
    uint64_t orders = 0, warms = 0;
};

Result run_once(const Args& a, uint64_t msgs, uint64_t interval_ns, uint64_t warm_ns, bool thrash) {
    ControlBlock cb = default_control(8);
    for (auto& l : cb.limits) {  // the benchmark measures latency, not the rate limiter
        l.rate_per_sec = 10'000'000;
        l.burst = 10'000'000;
    }
    TakerParams tp;
    tp.cooldown_events = 5;
    Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker(tp));
    FakeFeed feed(11, 8, msgs, interval_ns);
    FakeGateway gw(8, /*fill=*/false);  // IOCs cancel: inventory stays flat, the strategy keeps firing
    FastPathConfig cfg;
    cfg.require_control = false;
    cfg.default_control = cb;
    cfg.warmup_msgs = std::min<uint64_t>(msgs / 10, 20'000);
    cfg.warm_interval_ns = warm_ns;
    cfg.maintain_interval_ns = 10'000;
    FastPath<ImbalanceTaker, FakeFeed, FakeGateway> fp(core, feed, gw, JournalWriter(), nullptr, cfg);

    std::atomic<bool> stop{false};
    std::thread noisy;
    if (thrash) {  // walks 64 MB, evicting the fast path's data from the shared cache
        noisy = std::thread([&] {
            bench::pin(a, 1);
            std::vector<uint64_t> big(8u << 20);
            uint64_t x = 1;
            while (!stop.load(std::memory_order_relaxed))
                for (size_t i = 0; i < big.size(); i += 8) big[i] += ++x;
        });
    }
    bench::pin(a, 0);
    while (fp.step()) {
    }
    fp.finish();
    stop = true;
    if (noisy.joinable()) noisy.join();

    Result r;
    r.decision = fp.hist(Metric::DecisionCode);
    r.strategy = fp.hist(Metric::StrategyStage);
    r.risk = fp.hist(Metric::RiskStage);
    r.encode = fp.hist(Metric::EncodeStage);
    r.orders = core.stats().orders;
    r.warms = fp.stats().warms;
    return r;
}

}  // namespace

namespace bench {

int run_fastpath(const Args& a) {
    const uint64_t dense = a.u64("msgs", 2'000'000);
    std::printf("\n[fastpath] dense events (%llu msgs back to back), decision path = delivered -> bytes ready\n",
                (unsigned long long)dense);
    Csv csv(a, "fastpath", (std::string("scenario,") + Histogram::csv_header()).c_str());
    {
        Result r = run_once(a, dense, 0, 0, false);
        r.decision.print("decision (msg->bytes)");
        r.strategy.print("  strategy");
        r.risk.print("  risk");
        r.encode.print("  encode (template)");
        csv.row("dense,%s\n", r.decision.csv_row("decision").c_str());
        std::printf("  orders %llu\n", (unsigned long long)r.orders);
    }

    const uint64_t sparse_us = a.u64("sparse-us", 200);
    const uint64_t sparse_msgs = a.u64("sparse-msgs", 20'000);
    const bool thrash = !a.has("no-thrash");
    std::printf("\n[fastpath] sparse events: one every %llu us, %s cache-thrashing neighbour\n",
                (unsigned long long)sparse_us, thrash ? "with a" : "no");
    for (uint64_t warm_us : {uint64_t(0), uint64_t(20)}) {
        Result r = run_once(a, sparse_msgs, sparse_us * 1000, warm_us * 1000, thrash);
        char label[64];
        std::snprintf(label, sizeof(label), "%s", warm_us ? "warmed every 20 us" : "no warming");
        r.decision.print(label);
        std::printf("  orders %llu warms %llu\n", (unsigned long long)r.orders, (unsigned long long)r.warms);
        csv.row("%s,%s\n", warm_us ? "sparse_warm" : "sparse_cold", r.decision.csv_row("decision").c_str());
    }
    return 0;
}

}  // namespace bench
