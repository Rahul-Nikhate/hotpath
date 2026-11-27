// Fast path with in-memory feed/gateway: allocation discipline, warming,
// gating on the control process, kill switch, and replay determinism.
#include <memory>
#include <new>
#include <vector>

#include "common/alloc_counter.h"
#include "engine/core.h"
#include "engine/defaults.h"
#include "engine/fakes.h"
#include "engine/fast_path.h"
#include "engine/journal.h"
#include "proto/order_template.h"
#include "replay/replay.h"
#include "test.h"

using namespace hp;

namespace {
struct Harness {
    std::unique_ptr<SharedLayout> shm = std::make_unique<SharedLayout>();
    std::vector<JournalRecord> drain() {
        std::vector<JournalRecord> v;
        JournalRecord r;
        while (shm->journal.try_pop(r)) v.push_back(r);
        return v;
    }
};

FastPathConfig quick_cfg() {
    FastPathConfig c;
    c.require_control = false;
    c.default_control = default_control(8);
    c.warmup_msgs = 1000;
    c.maintain_interval_ns = 0;  // maintenance every iteration (deterministic in tests)
    return c;
}

template <class FP>
void run_to_end(FP& fp) {
    while (fp.step()) {
    }
    fp.finish();
}
}  // namespace

// Hotpath's own stages (strategy, risk, order manager, template encode,
// journal) must not allocate. The reused project-1 book is excluded on
// purpose: its std::map/unordered_map allocate per new order (documented).
TEST(hot_stages_allocate_nothing) {
    if (!alloc_counting_enabled()) return;  // sanitizer build
    Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker{});
    core.apply_control(default_control(8));
    FakeFeed feed(1, 8, 20'000);
    struct Sink {
        void on_order(uint64_t, const OrderIntent&, uint64_t) {}
        void on_risk_reject(RiskResult, const OrderIntent&, uint64_t) {}
        void on_warm(const OrderIntent&) {}
    } sink;
    NullProbe probe;
    while (!feed.done()) feed.poll(0, [&](uint64_t, const feed::Message& m, uint64_t) { core.on_market(m, probe, sink); });

    auto q = std::make_unique<JournalQueue>();
    JournalWriter jw(q.get(), nullptr);
    proto::OrderTemplates tpl(8);
    ImbalanceTaker strat(TakerParams{});
    Risk risk;
    risk.apply(default_control(8));
    OrderManager om;
    JournalRecord sink_rec;
    const uint64_t before = thread_allocations();
    for (uint64_t i = 1; i <= 100'000; ++i) {
        const uint16_t s = uint16_t(i % 8);
        const auto& book = core.books().book(s);
        auto intent = strat.peek(s, book, 0);
        const OrderIntent o = intent ? *intent : OrderIntent{s, Side::Buy, 10000, 100};
        (void)risk.check(o, Core<ImbalanceTaker>::reference_price(book), 0, 0, 0, i * 1'000'000);
        om.on_send(i, o);
        (void)tpl.patch(o.symbol, o.side, i, o.qty, o.price);
        proto::OrderEvent e;
        e.token = i;
        e.type = proto::Msg::Accepted;
        om.on_event(e);
        e.type = proto::Msg::Canceled;
        e.qty = o.qty;
        om.on_event(e);
        jw.put(jr::order_sent(i, o, i, true));
        (void)q->try_pop(sink_rec);
    }
    CHECK_EQ(thread_allocations() - before, 0u);
    CHECK_EQ(om.live_orders(), 0u);
}

TEST(fastpath_trades_and_replay_is_identical) {
    Harness h;
    Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker{});
    FakeFeed feed(3, 8, 30'000);
    FakeGateway gw(8, /*fill=*/true);
    FastPath<ImbalanceTaker, FakeFeed, FakeGateway> fp(core, feed, gw, JournalWriter(&h.shm->journal, nullptr), nullptr,
                                                       quick_cfg());
    std::vector<JournalRecord> recs;
    while (fp.step()) {
        auto part = h.drain();
        recs.insert(recs.end(), part.begin(), part.end());
    }
    fp.finish();
    auto tail = h.drain();
    recs.insert(recs.end(), tail.begin(), tail.end());

    CHECK(core.stats().orders > 50);
    CHECK(fp.stats().finished_clean);
    CHECK_EQ(core.om().live_orders(), 0u);
    CHECK(fp.hist(Metric::Decision).count() > 0);
    const ReplayResult r = replay(recs);
    CHECK(r.ok);
    CHECK(r.match);
    CHECK_EQ(r.replay_orders, core.stats().orders);
}

// Warming runs the decision path on quiet ticks; it must never change what the
// engine decides. Same feed with and without warming -> identical decisions.
TEST(warming_does_not_change_decisions) {
    auto decisions = [](uint64_t warm_ns, uint64_t& warms) {
        Harness h;
        Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker{});
        FakeFeed feed(5, 8, 3'000, /*interval_ns=*/20'000);  // sparse: 20 us apart
        FakeGateway gw(8, true);
        FastPathConfig c = quick_cfg();
        c.warm_interval_ns = warm_ns;
        FastPath<ImbalanceTaker, FakeFeed, FakeGateway> fp(core, feed, gw, JournalWriter(&h.shm->journal, nullptr),
                                                           nullptr, c);
        std::vector<JournalRecord> out;
        while (fp.step())
            for (auto& r : h.drain())
                if (r.kind == Rec::OrderSent || r.kind == Rec::RiskReject) out.push_back(r);
        fp.finish();
        warms = fp.stats().warms;
        return out;
    };
    uint64_t w0 = 0, w1 = 0;
    const auto cold = decisions(0, w0);
    const auto warm = decisions(2'000, w1);
    CHECK_EQ(w0, 0u);
    CHECK(w1 > 0);
    CHECK_EQ(cold.size(), warm.size());
    bool same = cold.size() == warm.size();
    for (size_t i = 0; same && i < cold.size(); ++i)
        same = cold[i].id == warm[i].id && cold[i].price == warm[i].price && cold[i].qty == warm[i].qty &&
               cold[i].a == warm[i].a && cold[i].symbol == warm[i].symbol;
    CHECK(same);
}

TEST(no_trading_without_a_live_control_process) {
    Harness h;
    Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker{});
    FakeFeed feed(3, 8, 1'000'000);  // long enough that the feed never ends during the test
    FakeGateway gw(8);
    FastPathConfig c = quick_cfg();
    c.require_control = true;  // limits must come from the control process
    FastPath<ImbalanceTaker, FakeFeed, FakeGateway> fp(core, feed, gw, JournalWriter(), h.shm.get(), c);
    for (int i = 0; i < 20'000; ++i) fp.step();
    CHECK_EQ(core.stats().orders, 0u);  // no limits, no heartbeat: gated off

    h.shm->control.store(default_control(8));  // control publishes limits...
    h.shm->hdr.control_heartbeat_ns.store(now_ns());  // ...and heartbeats
    for (int i = 0; i < 20'000; ++i) fp.step();
    CHECK(core.stats().orders > 0);
    CHECK(core.enabled());

    h.shm->hdr.control_heartbeat_ns.store(1);  // control goes silent -> stop trading
    fp.step();
    CHECK(!core.enabled());
}

TEST(kill_switch_blocks_every_order) {
    Core<ImbalanceTaker> core(8, 1 << 16, ImbalanceTaker{});
    FakeFeed feed(3, 8, 20'000);
    FakeGateway gw(8);
    FastPathConfig c = quick_cfg();
    c.default_control.kill = 1;
    FastPath<ImbalanceTaker, FakeFeed, FakeGateway> fp(core, feed, gw, JournalWriter(), nullptr, c);
    run_to_end(fp);
    CHECK_EQ(core.stats().orders, 0u);
    CHECK(core.stats().rejects[size_t(RiskResult::Killed)] > 0);
    CHECK_EQ(gw.stats().orders_sent, 0u);
}
