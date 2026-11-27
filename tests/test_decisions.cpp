// Strategy, risk (incl. a reference-model fuzz), and the order state machine.
#include <algorithm>

#include "engine/defaults.h"
#include "engine/order_manager.h"
#include "engine/risk.h"
#include "engine/strategy.h"
#include "feed_generator.h"  // project 1
#include "test.h"

using namespace hp;
using proto::Msg;
using proto::OrderEvent;

// ---------------------------------------------------------------- strategy

static feed::OrderBook book_with(uint64_t bid_qty, uint64_t ask_qty, uint32_t bid = 100, uint32_t ask = 101) {
    feed::OrderBook b;
    b.add(1, Side::Buy, bid, uint32_t(bid_qty));
    b.add(2, Side::Sell, ask, uint32_t(ask_qty));
    return b;
}

TEST(strategy_buys_thin_ask_sells_thin_bid) {
    TakerParams p;
    p.cooldown_events = 0;
    ImbalanceTaker s(p);
    auto buy = s.on_book(0, book_with(900, 100), 0);  // imbalance +0.8
    CHECK(buy.has_value());
    CHECK(buy->side == Side::Buy);
    CHECK_EQ(buy->price, 101u);  // lifts the ask
    CHECK_EQ(buy->qty, 100u);
    auto sell = s.on_book(0, book_with(100, 900), 0);
    CHECK(sell.has_value());
    CHECK(sell->side == Side::Sell);
    CHECK_EQ(sell->price, 100u);
    CHECK(!s.on_book(0, book_with(500, 500), 0).has_value());  // balanced
}

TEST(strategy_respects_spread_inventory_and_cooldown) {
    TakerParams p;
    p.cooldown_events = 3;
    p.max_spread_ticks = 2;
    p.max_inventory = 150;
    ImbalanceTaker s(p);
    const auto lopsided = book_with(900, 100);
    CHECK(!s.on_book(0, book_with(900, 100, 100, 105), 0).has_value());  // spread 5 > 2 (cooldown 1/3)
    CHECK(!s.on_book(0, lopsided, 0).has_value());                         // cooldown 2/3
    CHECK(!s.on_book(0, lopsided, 0).has_value());                         // cooldown 3/3
    CHECK(s.on_book(0, lopsided, 0).has_value());                          // fires
    CHECK(!s.on_book(0, lopsided, 0).has_value());                         // cooling down again
    TakerParams q;
    q.cooldown_events = 0;
    q.max_inventory = 150;
    ImbalanceTaker capped(q);
    CHECK(!capped.on_book(0, lopsided, 100).has_value());  // 100 + 100 > 150
}

TEST(strategy_peek_never_changes_state) {
    TakerParams p;
    p.cooldown_events = 2;
    ImbalanceTaker s(p);
    const auto b = book_with(900, 100);
    for (int i = 0; i < 10; ++i) CHECK(s.peek(0, b, 0).has_value());  // peek ignores and keeps cooldown
    CHECK(!s.on_book(0, b, 0).has_value());
    CHECK(!s.on_book(0, b, 0).has_value());
    CHECK(s.on_book(0, b, 0).has_value());
}

// ---------------------------------------------------------------- risk

static ControlBlock limits_for_test() {
    ControlBlock cb = default_control(4);
    for (auto& l : cb.limits) {
        l.max_order_qty = 500;
        l.max_position = 1000;
        l.band_ticks = 10;
        l.rate_per_sec = 10;
        l.burst = 3;
        l.max_notional = 500 * 200;
    }
    return cb;
}

TEST(risk_each_check_fires) {
    Risk r;
    r.apply(limits_for_test());
    const uint64_t t = 1'000'000'000;
    CHECK(r.check({0, Side::Buy, 100, 100}, 100, 0, 0, 0, t) == RiskResult::Ok);
    CHECK(r.check({9, Side::Buy, 100, 100}, 100, 0, 0, 0, t) == RiskResult::BadSymbol);
    CHECK(r.check({0, Side::Buy, 100, 0}, 100, 0, 0, 0, t) == RiskResult::MaxQty);
    CHECK(r.check({0, Side::Buy, 100, 501}, 100, 0, 0, 0, t) == RiskResult::MaxQty);
    CHECK(r.check({0, Side::Buy, 250, 500}, 250, 0, 0, 0, t) == RiskResult::MaxNotional);
    CHECK(r.check({0, Side::Buy, 111, 100}, 100, 0, 0, 0, t) == RiskResult::PriceBand);
    CHECK(r.check({0, Side::Buy, 100, 100}, 0, 0, 0, 0, t) == RiskResult::PriceBand);  // no reference
    // Position counts open orders as if they fill: 800 held + 150 open + 100 > 1000.
    CHECK(r.check({1, Side::Buy, 100, 100}, 100, 800, 150, 0, t) == RiskResult::Position);
    CHECK(r.check({1, Side::Sell, 100, 100}, 100, -800, 0, 150, t) == RiskResult::Position);
    CHECK(r.check({1, Side::Sell, 100, 100}, 100, 800, 150, 0, t) == RiskResult::Ok);  // reduces risk
    r.set_killed(true);
    CHECK(r.check({0, Side::Buy, 100, 100}, 100, 0, 0, 0, t) == RiskResult::Killed);
}

TEST(risk_token_bucket_burst_then_refill) {
    Risk r;
    r.apply(limits_for_test());  // burst 3, 10 per second
    uint64_t t = 5'000'000'000;
    const OrderIntent o{2, Side::Buy, 100, 10};
    for (int i = 0; i < 3; ++i) CHECK(r.check(o, 100, 0, 0, 0, t) == RiskResult::Ok);
    CHECK(r.check(o, 100, 0, 0, 0, t) == RiskResult::Rate);
    CHECK(r.check(o, 100, 0, 0, 0, t, /*consume=*/false) == RiskResult::Rate);
    t += 50'000'000;  // 50 ms = half a token
    CHECK(r.check(o, 100, 0, 0, 0, t) == RiskResult::Rate);
    t += 50'000'000;  // now a full token (fractions are kept, not rounded away)
    CHECK(r.check(o, 100, 0, 0, 0, t) == RiskResult::Ok);
    CHECK(r.check(o, 100, 0, 0, 0, t) == RiskResult::Rate);
}

// Reference model: an obviously-correct, slow restatement of the rules. Random
// intents through both must always agree, and accepted orders must never take
// the position beyond the limit.
namespace {
struct RefRisk {
    ControlBlock cb;
    double tokens[kMaxSymbols];
    double last[kMaxSymbols];
    bool init[kMaxSymbols] = {};
    RiskResult check(const OrderIntent& o, uint32_t ref, int64_t pos, uint64_t ob, uint64_t os, uint64_t now) {
        if (cb.kill) return RiskResult::Killed;
        if (o.symbol >= cb.num_symbols) return RiskResult::BadSymbol;
        const SymbolLimits& l = cb.limits[o.symbol];
        if (o.qty < 1 || o.qty > l.max_order_qty) return RiskResult::MaxQty;
        if (double(o.qty) * o.price > double(l.max_notional)) return RiskResult::MaxNotional;
        if (ref == 0 || std::abs(int64_t(o.price) - int64_t(ref)) > int64_t(l.band_ticks)) return RiskResult::PriceBand;
        const int64_t worst = o.side == Side::Buy ? pos + int64_t(ob) + o.qty : pos - int64_t(os) - o.qty;
        if (std::abs(worst) > int64_t(l.max_position) && (o.side == Side::Buy ? worst > 0 : worst < 0))
            return RiskResult::Position;
        double& tk = tokens[o.symbol];
        if (!init[o.symbol]) {
            tk = l.burst;
            last[o.symbol] = double(now);
            init[o.symbol] = true;
        } else if (double(now) > last[o.symbol]) {
            const double dt = std::min(double(now) - last[o.symbol], 10e9);
            tk = std::min<double>(l.burst, tk + dt * 1e-9 * l.rate_per_sec);
            last[o.symbol] = double(now);
        }
        if (tk < 1.0 - 1e-9) return RiskResult::Rate;
        tk -= 1.0;
        return RiskResult::Ok;
    }
};
}  // namespace

TEST(risk_matches_reference_model_on_random_intents) {
    feed::SplitMix64 rng(99);
    ControlBlock cb = limits_for_test();
    Risk fast;
    fast.apply(cb);
    RefRisk ref{cb, {}, {}};
    int64_t pos[4] = {};
    uint64_t now = 1'000'000'000, mismatches = 0, accepted = 0;
    for (int i = 0; i < 200'000; ++i) {
        now += rng.below(40'000'000);  // up to 40 ms between orders
        const OrderIntent o{uint16_t(rng.below(5)), rng.below(2) ? Side::Sell : Side::Buy,
                            uint32_t(90 + rng.below(25)), uint32_t(rng.below(600))};
        const uint32_t refp = rng.below(50) == 0 ? 0 : 100;
        const int64_t p = o.symbol < 4 ? pos[o.symbol] : 0;
        const uint64_t ob = rng.below(300), os = rng.below(300);
        const RiskResult a = fast.check(o, refp, p, ob, os, now);
        const RiskResult b = ref.check(o, refp, p, ob, os, now);
        if (a != b) ++mismatches;
        if (a == RiskResult::Ok) {
            ++accepted;
            const int64_t worst = o.side == Side::Buy ? p + int64_t(ob) + o.qty : p - int64_t(os) - o.qty;
            CHECK(worst <= 1000 && worst >= -1000);
            if (rng.below(2)) pos[o.symbol] += o.side == Side::Buy ? o.qty : -int64_t(o.qty);  // some fill
        }
    }
    CHECK_EQ(mismatches, 0u);
    CHECK(accepted > 1000);
}

// ---------------------------------------------------------------- order manager

static OrderEvent ev(Msg t, uint64_t token, uint32_t qty = 0, uint32_t price = 0) {
    OrderEvent e;
    e.type = t;
    e.token = token;
    e.qty = qty;
    e.price = price;
    return e;
}

TEST(om_ioc_partial_fill_then_cancel) {
    OrderManager om;
    om.on_send(1, {3, Side::Buy, 100, 300});
    CHECK_EQ(om.open_buy(3), 300u);
    CHECK_EQ(om.live_orders(), 1u);
    om.on_event(ev(Msg::Accepted, 1));
    CHECK(om.get(1)->state == OrdState::Live);
    om.on_event(ev(Msg::Executed, 1, 100, 100));
    CHECK_EQ(om.position(3), 100);
    CHECK_EQ(om.cash(3), -10000);
    CHECK_EQ(om.open_buy(3), 200u);
    om.on_event(ev(Msg::Canceled, 1, 200));
    CHECK(om.get(1)->state == OrdState::Cancelled);
    CHECK_EQ(om.open_buy(3), 0u);
    CHECK_EQ(om.live_orders(), 0u);
}

TEST(om_fill_racing_a_cancel_is_honoured) {
    OrderManager om;
    om.on_send(5, {0, Side::Sell, 100, 50});
    om.on_event(ev(Msg::Accepted, 5));
    om.on_cancel_sent(5);
    CHECK(om.get(5)->state == OrdState::PendingCancel);
    om.on_event(ev(Msg::Executed, 5, 50, 100));  // exchange filled it before our cancel arrived
    CHECK(om.get(5)->state == OrdState::Filled);
    CHECK_EQ(om.position(0), -50);
    om.on_event(ev(Msg::Canceled, 5, 50));       // a late "too late" cancel must not double-count
    CHECK_EQ(om.stats().bad_transitions, 1u);
    CHECK_EQ(om.open_sell(0), 0u);
}

TEST(om_reject_lost_unknown) {
    OrderManager om;
    om.on_send(7, {1, Side::Buy, 100, 10});
    om.on_send(8, {1, Side::Buy, 100, 20});
    CHECK_EQ(om.open_buy(1), 30u);
    om.on_event(ev(Msg::Rejected, 7));
    CHECK(om.get(7)->state == OrdState::Rejected);
    om.on_event(ev(Msg::Lost, 8));
    CHECK(om.get(8)->state == OrdState::Lost);
    CHECK_EQ(om.open_buy(1), 0u);
    CHECK_EQ(om.live_orders(), 0u);
    om.on_event(ev(Msg::Accepted, 999));
    CHECK_EQ(om.stats().unknown_events, 1u);
    om.on_send(9, {1, Side::Buy, 100, 10});
    om.on_event(ev(Msg::Accepted, 9));
    om.on_event(ev(Msg::Lost, 9));  // only PendingNew can be Lost
    CHECK(om.get(9)->state == OrdState::Live);
}
