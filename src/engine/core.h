// The deterministic heart of the engine: book -> strategy -> risk -> order
// manager. It has no I/O and reads no clocks: time comes from the market data
// timestamps. The live fast path and the offline replay tool both drive this
// exact class, which is what makes "replay produces byte-identical orders" a
// meaningful test.
//
// Out (supplied by the caller) receives the decisions:
//   out.on_order(token, intent)        -> send it
//   out.on_risk_reject(result, intent) -> record it
//   out.on_warm(intent)                -> exercise the send path without sending
// Probe receives stage timestamps (NullProbe when not timing).
#pragma once

#include <array>
#include <cstdint>

#include "engine/order_manager.h"
#include "engine/risk.h"
#include "engine/strategy.h"
#include "engine/types.h"
#include "order_book.h"  // project 1

namespace hp {

struct CoreStats {
    uint64_t market_msgs = 0, signals = 0, orders = 0, warms = 0;
    std::array<uint64_t, size_t(RiskResult::Count)> rejects{};
};

template <Strategy S>
class Core {
public:
    Core(uint16_t num_symbols, size_t expected_orders, S strategy, uint64_t first_token = 1)
        : books_(num_symbols, expected_orders), strategy_(strategy), num_symbols_(num_symbols),
          next_token_(first_token) {}

    template <class Probe, class Out>
    void on_market(const feed::Message& m, Probe& probe, Out& out) {
        books_.apply(m);
        ++stats_.market_msgs;
        probe.mark(Stage::Book);
        if (!enabled_ || m.symbol >= num_symbols_) return;

        const feed::OrderBook& book = books_.book(m.symbol);
        const std::optional<OrderIntent> intent = strategy_.on_book(m.symbol, book, om_.position(m.symbol));
        probe.mark(Stage::Decision);
        if (!intent) [[likely]] return;
        ++stats_.signals;

        const RiskResult r = risk_.check(*intent, reference_price(book), om_.position(intent->symbol),
                                         om_.open_buy(intent->symbol), om_.open_sell(intent->symbol),
                                         m.timestamp_ns, /*consume=*/true);
        probe.mark(Stage::Risk);
        if (r != RiskResult::Ok) {
            ++stats_.rejects[size_t(r)];
            out.on_risk_reject(r, *intent, m.timestamp_ns);
            return;
        }
        const uint64_t token = next_token_++;
        om_.on_send(token, *intent);
        ++stats_.orders;
        out.on_order(token, *intent, m.timestamp_ns);
    }

    void on_order_event(const proto::OrderEvent& e) { om_.on_event(e); }
    void apply_control(const ControlBlock& cb) { risk_.apply(cb); }
    void set_enabled(bool on) { enabled_ = on; }

    // Runs the decision path for `symbol` without changing any state, so caches
    // and branch predictors stay trained during quiet periods. If the strategy
    // has no signal, a dummy 1-lot intent at the touch keeps the risk and encode
    // code warm too. Nothing is sent and nothing is journaled.
    template <class Out>
    void warm(uint16_t symbol, Out& out) {
        if (symbol >= num_symbols_) return;
        const feed::OrderBook& book = books_.book(symbol);
        std::optional<OrderIntent> intent = strategy_.peek(symbol, book, om_.position(symbol));
        if (!intent) {
            const feed::Quote ask = book.best_ask();
            intent = OrderIntent{symbol, Side::Buy, ask.valid ? ask.price : 1, 1};
        }
        (void)risk_.check(*intent, reference_price(book), om_.position(symbol), om_.open_buy(symbol),
                          om_.open_sell(symbol), 0, /*consume=*/false);
        out.on_warm(*intent);
        ++stats_.warms;
    }

    static uint32_t reference_price(const feed::OrderBook& book) {
        const feed::Quote b = book.best_bid(), a = book.best_ask();
        return b.valid && a.valid ? uint32_t((uint64_t(b.price) + a.price) / 2) : 0;
    }

    const feed::Books& books() const { return books_; }
    const OrderManager& om() const { return om_; }
    const Risk& risk() const { return risk_; }
    const S& strategy() const { return strategy_; }
    const CoreStats& stats() const { return stats_; }
    bool enabled() const { return enabled_; }
    uint16_t num_symbols() const { return num_symbols_; }

private:
    feed::Books books_;
    S strategy_;
    Risk risk_;
    OrderManager om_;
    CoreStats stats_;
    uint16_t num_symbols_;
    uint64_t next_token_;
    bool enabled_ = false;
};

}  // namespace hp
