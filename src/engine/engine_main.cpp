// hp_engine: the trading process. Creates the shared-memory region, connects
// to the exchange, and runs the fused fast path on (optionally) a pinned core.
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <new>
#include <thread>

#include "common/args.h"
#include "common/clock.h"
#include "engine/defaults.h"
#include "engine/fast_path.h"
#include "engine/feed_adapter.h"
#include "engine/gateway.h"
#include "net.h"  // project 1
#include "shm/region.h"

using namespace hp;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
    std::puts(
        "hp_engine [options]\n"
        "  --name N           shared-memory instance name (default demo)\n"
        "  --symbols N        instruments (default 8, max 64)\n"
        "  --udp-ip IP --udp-port P [--multicast --iface IP]   market data (default 127.0.0.1:9999)\n"
        "  --recovery-port P  retransmission server (default 10000)\n"
        "  --order-port P     order entry (default 10100)\n"
        "  --cpu N            pin the fast path to core N (Linux)\n"
        "  --huge             shared region on 2 MB huge pages (Linux, needs reserved pages)\n"
        "  --no-prefault      don't touch the shared region's pages at startup\n"
        "  --no-control       run without the control process (built-in default limits, no journal drain)\n"
        "  --warm-us N        warm the hot path after N us without market data (0 = off, default 100)\n"
        "  --threshold P      imbalance threshold, permille (default 300)\n"
        "  --max-spread T     max spread in ticks (default 2)\n"
        "  --qty Q            order size (default 100)\n"
        "  --max-inv Q        strategy inventory cap (default 2000)\n"
        "  --cooldown N       min book events per symbol between orders (default 20)\n"
        "  --warmup N         ignore the first N messages in latency stats (default 10000)\n"
        "  --csv FILE         write latency percentiles as CSV\n");
}
}  // namespace

int main(int argc, char** argv) {
    Args a(argc, argv);
    if (a.has("help")) {
        usage();
        return 0;
    }
    init_clock();
    net::ignore_sigpipe();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    const std::string name = a.str("name", "demo");
    const uint16_t symbols = uint16_t(std::min<uint64_t>(a.u64("symbols", 8), kMaxSymbols));
    const bool no_control = a.has("no-control");

    // 1. Shared memory: control block + journal queue.
    Region region = Region::create(name, sizeof(SharedLayout), a.has("huge"), !a.has("no-prefault"));
    auto* shm = new (region.data()) SharedLayout();
    shm->hdr.layout_hash = layout_hash();
    shm->hdr.engine_pid = int64_t(::getpid());
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<std::atomic<uint64_t>*>(&shm->hdr.magic)->store(kMagic, std::memory_order_release);

    // 2. Market data in, order entry out.
    FeedConfig fc;
    fc.udp_ip = a.str("udp-ip", "127.0.0.1");
    fc.udp_port = uint16_t(a.u64("udp-port", 9999));
    fc.multicast = a.has("multicast");
    fc.iface = a.str("iface", "127.0.0.1");
    fc.recovery_ip = a.str("exchange-ip", "127.0.0.1");
    fc.recovery_port = uint16_t(a.u64("recovery-port", 10000));
    FeedAdapter feed(fc);
    feed.start();

    GatewayConfig gc;
    gc.ip = a.str("exchange-ip", "127.0.0.1");
    gc.port = uint16_t(a.u64("order-port", 10100));
    gc.num_symbols = symbols;
    Gateway gw(gc);
    for (int i = 0; i < 100 && !gw.connect(now_ns()) && !g_stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 3. Decision core.
    TakerParams tp;
    tp.threshold_permille = uint32_t(a.u64("threshold", tp.threshold_permille));
    tp.max_spread_ticks = uint32_t(a.u64("max-spread", tp.max_spread_ticks));
    tp.order_qty = uint32_t(a.u64("qty", tp.order_qty));
    tp.max_inventory = a.i64("max-inv", tp.max_inventory);
    tp.cooldown_events = uint32_t(a.u64("cooldown", tp.cooldown_events));
    FastPathConfig cfg;
    cfg.require_control = !no_control;
    cfg.default_control = default_control(symbols, &a);
    cfg.warm_interval_ns = a.u64("warm-us", 100) * 1000;
    cfg.warmup_msgs = a.u64("warmup", 10'000);
    Core<ImbalanceTaker> core(symbols, cfg.expected_orders, ImbalanceTaker(tp));

    // 4. Pin, lock memory, run.
    const int64_t cpu = a.i64("cpu", -1);
    const bool pinned = cpu >= 0 && net::pin_current_thread(int(cpu));
#ifdef __linux__
    const bool locked = ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#else
    const bool locked = false;
#endif
    std::fprintf(stderr,
                 "[engine] region %s: %zu KB, huge=%d prefaulted=%d mlock=%d | pinned=%d mlockall=%d | "
                 "udp port %u | ns/cycle %.3f\n",
                 name.c_str(), region.size() / 1024, region.info().huge, region.info().prefaulted,
                 region.info().locked, pinned, locked, feed.port(), g_ns_per_cycle);

    JournalWriter jw = no_control ? JournalWriter() : JournalWriter(&shm->journal, &shm->hdr.journal_dropped);
    FastPath<ImbalanceTaker, FeedAdapter, Gateway> fp(core, feed, gw, jw, shm, cfg);
    fp.run(g_stop);
    gw.logout();

    // 5. Report.
    const CoreStats& cs = core.stats();
    const OmStats& os = core.om().stats();
    const FastPathStats& fs = fp.stats();
    std::printf("\n== hp_engine ==\n");
    std::printf("market msgs %llu | datagrams %llu | gaps %llu recovered %llu | signals %llu | orders %llu\n",
                (unsigned long long)cs.market_msgs, (unsigned long long)feed.stats().datagrams,
                (unsigned long long)feed.seq_stats().gaps, (unsigned long long)feed.stats().recovered,
                (unsigned long long)cs.signals, (unsigned long long)cs.orders);
    std::printf("risk rejects:");
    for (size_t i = 1; i < size_t(RiskResult::Count); ++i)
        if (cs.rejects[i]) std::printf(" %s=%llu", to_string(RiskResult(i)), (unsigned long long)cs.rejects[i]);
    std::printf("\norders: accepted %llu fills %llu (qty %llu) cancelled %llu rejected %llu lost %llu | live at end %llu\n",
                (unsigned long long)os.accepted, (unsigned long long)os.fills, (unsigned long long)os.filled_qty,
                (unsigned long long)os.cancelled, (unsigned long long)os.rejected, (unsigned long long)os.lost,
                (unsigned long long)core.om().live_orders());
    std::printf("session: logins %llu disconnects %llu | warms %llu | journal written %llu dropped %llu\n",
                (unsigned long long)gw.stats().logins, (unsigned long long)gw.stats().disconnects,
                (unsigned long long)fs.warms, (unsigned long long)fp.journal().written(),
                (unsigned long long)fp.journal().dropped());
    std::printf("book hash %016llx (compare with the simulator's reference book hash)\n",
                (unsigned long long)core.books().hash());
    if (alloc_counting_enabled())
        std::printf("hot-thread heap allocations after warm-up: %llu  (project 1's std::map/unordered_map book\n"
                    "  allocates a node per new order/level; Hotpath's own stages allocate none: see tests)\n",
                    (unsigned long long)fs.hot_allocs);
    std::printf("\nlatency (warm-up of %llu messages excluded):\n", (unsigned long long)cfg.warmup_msgs);
    for (size_t m = 0; m < size_t(Metric::Count); ++m) fp.hist(Metric(m)).print(metric_name(Metric(m)));

    if (a.has("csv")) {
        if (FILE* f = std::fopen(a.str("csv", "latency.csv").c_str(), "w")) {
            std::fprintf(f, "%s\n", Histogram::csv_header());
            for (size_t m = 0; m < size_t(Metric::Count); ++m)
                std::fprintf(f, "%s\n", fp.hist(Metric(m)).csv_row(metric_name(Metric(m))).c_str());
            std::fclose(f);
        }
    }

    // Give the control process time to drain the journal before the region goes away.
    if (!no_control) {
        const uint64_t deadline = now_ns() + 5'000'000'000ULL;
        while (shm->journal.size_approx() > 0 && now_ns() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return fs.finished_clean ? 0 : 2;
}
