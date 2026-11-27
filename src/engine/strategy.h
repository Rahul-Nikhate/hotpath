// Strategy interface (a C++20 concept) and the one strategy we ship.
//
// The strategy is deliberately trivial: the project is about the latency and
// correctness of the infrastructure, not about alpha. ImbalanceTaker sends an
// IOC at the touch when the top of book is lopsided (queue imbalance), the
// spread is tight, and inventory allows. All integer arithmetic, so replay is
// bit-for-bit deterministic.
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <optional>

#include "engine/types.h"
#include "order_book.h"  // project 1
#include "shm/layout.h"

namespace hp {

// on_book: called for every market message on `symbol` while trading is
// enabled; may update internal state (e.g. cooldown counters).
// peek: same decision without changing state; used to warm the hot path.
template <class S>
concept Strategy = requires(S s, const S cs, uint16_t symbol, const feed::OrderBook& book, int64_t position) {
    { s.on_book(symbol, book, position) } -> std::same_as<std::optional<OrderIntent>>;
    { cs.peek(symbol, book, position) } -> std::same_as<std::optional<OrderIntent>>;
};

struct TakerParams {
    uint32_t threshold_permille = 300;  // |imbalance| >= 0.3
    uint32_t max_spread_ticks = 2;
    uint32_t order_qty = 100;
    int64_t max_inventory = 2000;       // strategy's own cap (risk has the hard limit)
    uint32_t cooldown_events = 20;      // min book events on a symbol between orders
};

class ImbalanceTaker {
public:
    explicit ImbalanceTaker(const TakerParams& p = {}) : p_(p) {}

    std::optional<OrderIntent> on_book(uint16_t symbol, const feed::OrderBook& book, int64_t position) {
        uint32_t& since = since_[symbol & (kMaxSymbols - 1)];
        if (since < p_.cooldown_events) {
            ++since;
            return std::nullopt;
        }
        auto intent = decide(symbol, book, position);
        if (intent) since = 0;
        return intent;
    }

    std::optional<OrderIntent> peek(uint16_t symbol, const feed::OrderBook& book, int64_t position) const {
        return decide(symbol, book, position);
    }

    const TakerParams& params() const { return p_; }

    // Imbalance in permille: +1000 = all size on the bid, -1000 = all on the ask.
    static int64_t imbalance_permille(uint64_t bid_qty, uint64_t ask_qty) {
        const uint64_t total = bid_qty + ask_qty;
        return total ? (int64_t(bid_qty) - int64_t(ask_qty)) * 1000 / int64_t(total) : 0;
    }

private:
    std::optional<OrderIntent> decide(uint16_t symbol, const feed::OrderBook& book, int64_t position) const {
        const feed::Quote bid = book.best_bid();
        const feed::Quote ask = book.best_ask();
        if (!bid.valid || !ask.valid || ask.price <= bid.price) return std::nullopt;
        if (ask.price - bid.price > p_.max_spread_ticks) return std::nullopt;
        const int64_t imb = imbalance_permille(bid.qty, ask.qty);
        const int64_t thr = int64_t(p_.threshold_permille);
        if (imb >= thr && position + int64_t(p_.order_qty) <= p_.max_inventory)  // ask side is thin: lift it
            return OrderIntent{symbol, Side::Buy, ask.price, uint32_t(std::min<uint64_t>(p_.order_qty, ask.qty))};
        if (imb <= -thr && position - int64_t(p_.order_qty) >= -p_.max_inventory)  // bid side is thin: hit it
            return OrderIntent{symbol, Side::Sell, bid.price, uint32_t(std::min<uint64_t>(p_.order_qty, bid.qty))};
        return std::nullopt;
    }

    TakerParams p_;
    std::array<uint32_t, kMaxSymbols> since_{};
};

static_assert(Strategy<ImbalanceTaker>);

}  // namespace hp
