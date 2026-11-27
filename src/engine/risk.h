// Pre-trade risk checks. Run on every order, on the critical path, so:
// flat arrays indexed by symbol id, integer comparisons, no locks, no
// allocation, no string lookups. Limits come from the control process via a
// seqlock and are copied in here when their version changes.
//
// Checks, in order: kill switch, symbol, max order qty, max notional, price
// band around the reference price, position limit (counting open orders as if
// they fill), and a token-bucket message rate limit.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "engine/types.h"
#include "shm/layout.h"

namespace hp {

enum class RiskResult : uint8_t { Ok = 0, Killed, BadSymbol, MaxQty, MaxNotional, PriceBand, Position, Rate, Count };

inline const char* to_string(RiskResult r) {
    switch (r) {
        case RiskResult::Ok: return "ok";
        case RiskResult::Killed: return "killed";
        case RiskResult::BadSymbol: return "bad_symbol";
        case RiskResult::MaxQty: return "max_qty";
        case RiskResult::MaxNotional: return "max_notional";
        case RiskResult::PriceBand: return "price_band";
        case RiskResult::Position: return "position";
        case RiskResult::Rate: return "rate";
        default: return "?";
    }
}

class Risk {
public:
    void apply(const ControlBlock& cb) {
        killed_ = cb.kill != 0;
        num_symbols_ = std::min<uint32_t>(cb.num_symbols, kMaxSymbols);
        for (uint32_t i = 0; i < kMaxSymbols; ++i) limits_[i] = cb.limits[i];
    }
    void set_killed(bool k) { killed_ = k; }
    bool killed() const { return killed_; }
    const SymbolLimits& limits(uint16_t s) const { return limits_[s & (kMaxSymbols - 1)]; }

    // ref_price: reference for the price band (mid); 0 = no reference -> reject.
    // position/open_*: current position and quantity on still-open orders.
    // now_ns: event time (from the market data, so replay is deterministic).
    // consume=false checks without spending a rate-limit token (hot-path warming).
    RiskResult check(const OrderIntent& o, uint32_t ref_price, int64_t position, uint64_t open_buy,
                     uint64_t open_sell, uint64_t now_ns, bool consume = true) {
        if (killed_) [[unlikely]] return RiskResult::Killed;
        if (o.symbol >= num_symbols_) [[unlikely]] return RiskResult::BadSymbol;
        const SymbolLimits& l = limits_[o.symbol];
        if (o.qty == 0 || o.qty > l.max_order_qty) return RiskResult::MaxQty;
        if (uint64_t(o.qty) * o.price > l.max_notional) return RiskResult::MaxNotional;
        const uint32_t dist = o.price > ref_price ? o.price - ref_price : ref_price - o.price;
        if (ref_price == 0 || dist > l.band_ticks) return RiskResult::PriceBand;
        const int64_t max_pos = int64_t(l.max_position);
        if (o.side == Side::Buy) {
            if (position + int64_t(open_buy) + int64_t(o.qty) > max_pos) return RiskResult::Position;
        } else {
            if (position - int64_t(open_sell) - int64_t(o.qty) < -max_pos) return RiskResult::Position;
        }
        if (!take_token(o.symbol, l, now_ns, consume)) return RiskResult::Rate;
        return RiskResult::Ok;
    }

private:
    // Token bucket in nano-tokens (1 token = 1e9) so slow refill rates never
    // lose fractional tokens to integer division.
    struct Bucket {
        uint64_t nano_tokens = 0;
        uint64_t last_ns = 0;
        bool init = false;
    };
    static constexpr uint64_t kToken = 1'000'000'000ULL;

    bool take_token(uint16_t s, const SymbolLimits& l, uint64_t now, bool consume) {
        Bucket& b = buckets_[s];
        const uint64_t cap = uint64_t(l.burst) * kToken;
        uint64_t tokens = b.init ? b.nano_tokens : cap;
        if (b.init && now > b.last_ns) {
            const uint64_t dt = std::min<uint64_t>(now - b.last_ns, 10'000'000'000ULL);  // cap: no overflow
            tokens = std::min(cap, tokens + dt * l.rate_per_sec);
        }
        if (tokens < kToken) {
            if (consume) {
                b.nano_tokens = tokens;
                b.last_ns = std::max(b.last_ns, now);
                b.init = true;
            }
            return false;
        }
        if (consume) {
            b.nano_tokens = tokens - kToken;
            b.last_ns = std::max(b.last_ns, now);
            b.init = true;
        }
        return true;
    }

    std::array<SymbolLimits, kMaxSymbols> limits_{};
    std::array<Bucket, kMaxSymbols> buckets_{};
    uint32_t num_symbols_ = 0;
    bool killed_ = false;
};

}  // namespace hp
