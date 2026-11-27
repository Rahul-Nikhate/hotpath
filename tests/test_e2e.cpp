// Real sockets over loopback: the simulator, the order-entry session and the
// whole engine, including packet loss, gap recovery, an injected disconnect,
// and deterministic replay of the resulting journal.
#include <atomic>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "engine/core.h"
#include "engine/defaults.h"
#include "engine/fast_path.h"
#include "engine/feed_adapter.h"
#include "engine/gateway.h"
#include "replay/replay.h"
#include "sim/sim_exchange.h"
#include "test.h"

using namespace hp;

namespace {
SimConfig sim_cfg() {
    SimConfig c;
    c.recovery_port = 0;  // ephemeral ports: tests can run in parallel with a demo
    c.order_port = 0;
    c.quiet = true;
    return c;
}
}  // namespace

// The session survives a disconnect without duplicating or losing track of
// any order: every order ends either acknowledged by the exchange or Lost
// (provably never received), and the exchange never sees a token twice.
TEST(session_reconnect_no_duplicates_no_phantoms) {
    SimConfig sc = sim_cfg();
    sc.count = 0;  // no market data needed: IOCs into an empty book are cancelled
    sc.feed_port = 9;  // discard
    sc.disconnect_after_orders = 5;
    sc.linger_s = 30;
    SimExchange sim(sc);
    std::atomic<bool> stop{false};
    std::thread t([&] { sim.run(&stop); });

    GatewayConfig gc;
    gc.port = sim.order_port();
    gc.reconnect_ns = 20'000'000;
    Gateway gw(gc);
    OrderManager om;
    NullProbe probe;
    auto pump = [&](uint64_t ms, auto&& until) {
        const uint64_t end = now_ns() + ms * 1'000'000;
        while (now_ns() < end && !until()) {
            const uint64_t now = now_ns();
            gw.poll(now, [&](const proto::OrderEvent& e) { om.on_event(e); });
            gw.maintain(now);
            if (gw.take_resynced() && gw.stats().logins > 1) {
                std::vector<uint64_t> lost;
                om.for_each_pending_new([&](const OmOrder& o) { lost.push_back(o.token); });
                for (uint64_t tok : lost) {
                    proto::OrderEvent e;
                    e.type = proto::Msg::Lost;
                    e.token = tok;
                    om.on_event(e);
                }
            }
        }
    };
    CHECK(gw.connect(now_ns()));
    pump(2000, [&] { return gw.ready(); });
    CHECK(gw.ready());

    uint64_t token = 0;
    for (int i = 0; i < 10; ++i) {  // back to back: #5 triggers the server-side drop
        const OrderIntent o{0, Side::Buy, 100, 10};
        ++token;
        om.on_send(token, o);
        gw.send_order(token, o, probe);
    }
    pump(3000, [&] { return gw.stats().logins >= 2 && gw.ready() && om.live_orders() == 0; });
    CHECK(gw.stats().logins >= 2);
    CHECK_EQ(om.live_orders(), 0u);

    for (int i = 0; i < 5; ++i) {  // trading continues normally after the reconnect
        const OrderIntent o{0, Side::Sell, 100, 10};
        ++token;
        om.on_send(token, o);
        CHECK(gw.send_order(token, o, probe));
    }
    pump(2000, [&] { return om.live_orders() == 0; });
    gw.logout();
    stop = true;
    t.join();

    const OmStats& s = om.stats();
    CHECK_EQ(om.live_orders(), 0u);
    CHECK_EQ(sim.stats().duplicate_tokens, 0u);
    CHECK_EQ(sim.stats().injected_disconnects, 1u);
    CHECK(sim.stats().replayed > 0);                       // responses to order #5 came via replay
    CHECK_EQ(s.sent, token);
    CHECK_EQ(s.cancelled + s.lost, s.sent);                 // empty book: every order is cancelled or lost
    CHECK_EQ(sim.unique_tokens(), s.sent - s.lost);         // exchange saw exactly the non-lost orders
    CHECK(s.lost < s.sent);
}

// Whole system: lossy feed with gap recovery, an injected order-session drop,
// journal drained concurrently, then replay must reproduce every decision.
TEST(end_to_end_loss_disconnect_and_replay) {
    FeedConfig fc;
    fc.udp_port = 0;
    FeedAdapter feed(fc);

    SimConfig sc = sim_cfg();
    sc.feed_port = feed.port();
    sc.count = HP_TSAN ? 40'000 : 200'000;
    sc.rate = 200'000;
    sc.drop_rate = 0.01;
    sc.disconnect_after_orders = 40;
    sc.linger_s = 0.5;
    SimExchange sim(sc);
    feed.set_recovery_port(sim.recovery_port());
    feed.start();

    std::atomic<bool> stop{false};
    std::thread simt([&] { sim.run(&stop); });

    GatewayConfig gc;
    gc.port = sim.order_port();
    gc.reconnect_ns = 20'000'000;
    Gateway gw(gc);
    for (int i = 0; i < 50 && !gw.connect(now_ns()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto shm = std::make_unique<SharedLayout>();
    std::atomic<bool> engine_done{false};
    std::vector<JournalRecord> recs;
    std::thread drainer([&] {  // stands in for the control process
        JournalRecord r;
        for (;;) {
            bool got = false;
            while (shm->journal.try_pop(r)) {
                recs.push_back(r);
                got = true;
            }
            if (!got && engine_done.load(std::memory_order_acquire)) {
                while (shm->journal.try_pop(r)) recs.push_back(r);
                return;
            }
            if (!got) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker{});
    FastPathConfig cfg;
    cfg.require_control = false;
    cfg.default_control = default_control(8);
    cfg.warmup_msgs = 5'000;
    cfg.warm_interval_ns = 100'000;
    FastPath<ImbalanceTaker, FeedAdapter, Gateway> fp(core, feed, gw,
                                                      JournalWriter(&shm->journal, &shm->hdr.journal_dropped),
                                                      shm.get(), cfg);
    std::atomic<bool> never{false};
    fp.run(never);
    gw.logout();
    engine_done.store(true, std::memory_order_release);
    drainer.join();
    stop = true;
    simt.join();

    const OmStats& os = core.om().stats();
    CHECK(fp.stats().finished_clean);
    CHECK_EQ(feed.seq_stats().delivered, sc.count);              // every message, despite 1% loss
    CHECK(feed.stats().recovered > 0);                             // ...thanks to recovery
    CHECK(core.books().hash() == sim.reference_hash());            // book identical to the exchange's
    CHECK(core.stats().orders > 40);
    CHECK_EQ(core.om().live_orders(), 0u);
    CHECK_EQ(sim.stats().duplicate_tokens, 0u);
    CHECK_EQ(sim.stats().injected_disconnects, 1u);
    CHECK(gw.stats().logins >= 2);
    CHECK_EQ(sim.unique_tokens(), os.sent - os.lost);              // no phantom orders
    CHECK_EQ(os.sent, core.stats().orders);

    const ReplayResult r = replay(recs);
    CHECK(r.ok);
    CHECK(r.match);
    CHECK_EQ(r.replay_orders, core.stats().orders);
    CHECK_EQ(r.journal_dropped, 0u);
}
