// Journal record builders/decoders and the non-blocking writer.
//
// The fast path never blocks on the journal: if the queue is full (the control
// process is not draining fast enough) the record is dropped and counted. A run
// with drops is reported as not replayable rather than slowing trading down.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/risk.h"
#include "engine/strategy.h"
#include "engine/types.h"
#include "proto/order_entry.h"
#include "shm/layout.h"

namespace hp {

using JournalQueue = SpscQueue<JournalRecord, kJournalCapacity>;

class JournalWriter {
public:
    JournalWriter() = default;
    JournalWriter(JournalQueue* q, std::atomic<uint64_t>* shared_drops) : q_(q), shared_drops_(shared_drops) {}

    void put(const JournalRecord& r) {
        if (!q_) return;
        if (q_->try_push(r)) [[likely]] {
            ++written_;
        } else {
            ++dropped_;
            if (shared_drops_) shared_drops_->fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool enabled() const { return q_ != nullptr; }
    uint64_t written() const { return written_; }
    uint64_t dropped() const { return dropped_; }

private:
    JournalQueue* q_ = nullptr;
    std::atomic<uint64_t>* shared_drops_ = nullptr;
    uint64_t written_ = 0, dropped_ = 0;
};

// Gating reasons (bit flags): trading is enabled only when this is 0.
enum GateReason : uint32_t {
    kGateSession = 1,   // order session not logged in / still resyncing
    kGateFeedGap = 2,   // market data has a gap: book may be wrong
    kGateControl = 4,   // control process silent or no limits yet
    kGateStopping = 8,  // feed ended / shutting down
};

namespace jr {

inline constexpr uint8_t kFormatVersion = 1;

inline JournalRecord run_start(const TakerParams& p, uint16_t num_symbols, uint64_t expected_orders) {
    JournalRecord r;
    r.kind = Rec::RunStart;
    r.a = kFormatVersion;
    r.symbol = num_symbols;
    r.qty = p.order_qty;
    r.price = p.threshold_permille;
    r.b = p.max_spread_ticks;
    r.seq = expected_orders;
    r.ts = p.cooldown_events;
    r.c = uint64_t(p.max_inventory);
    return r;
}
inline TakerParams params_from(const JournalRecord& r) {
    TakerParams p;
    p.order_qty = r.qty;
    p.threshold_permille = r.price;
    p.max_spread_ticks = r.b;
    p.cooldown_events = uint32_t(r.ts);
    p.max_inventory = int64_t(r.c);
    return p;
}

inline JournalRecord market(uint64_t seq, const feed::Message& m) {
    JournalRecord r;
    r.kind = Rec::Market;
    r.a = uint8_t(m.type);
    r.b = uint8_t(m.side);
    r.symbol = m.symbol;
    r.qty = m.qty;
    r.price = m.price;
    r.id = m.order_id;
    r.seq = seq;
    r.ts = m.timestamp_ns;
    return r;
}
inline feed::Message market_from(const JournalRecord& r) {
    feed::Message m;
    m.type = feed::MsgType(r.a);
    m.side = feed::Side(uint8_t(r.b));
    m.symbol = r.symbol;
    m.qty = r.qty;
    m.price = r.price;
    m.order_id = r.id;
    m.timestamp_ns = r.ts;
    return m;
}

inline JournalRecord order_sent(uint64_t token, const OrderIntent& o, uint64_t ts, bool send_ok) {
    JournalRecord r;
    r.kind = Rec::OrderSent;
    r.a = uint8_t(o.side);
    r.symbol = o.symbol;
    r.qty = o.qty;
    r.price = o.price;
    r.id = token;
    r.ts = ts;
    r.b = send_ok ? 1 : 0;
    return r;
}

inline JournalRecord event(const proto::OrderEvent& e) {
    JournalRecord r;
    r.kind = Rec::OrderEvent;
    r.a = uint8_t(e.type);
    r.symbol = e.symbol;
    r.qty = e.qty;
    r.price = e.price;
    r.id = e.token;
    r.ts = e.ts;
    r.b = e.reason;
    r.c = e.ref;
    r.d = uint8_t(e.side);
    return r;
}
inline proto::OrderEvent event_from(const JournalRecord& r) {
    proto::OrderEvent e;
    e.type = proto::Msg(r.a);
    e.symbol = r.symbol;
    e.qty = r.qty;
    e.price = r.price;
    e.token = r.id;
    e.ts = r.ts;
    e.reason = uint8_t(r.b);
    e.ref = r.c;
    e.side = feed::Side(uint8_t(r.d));
    return e;
}

inline JournalRecord risk_reject(RiskResult res, const OrderIntent& o, uint64_t ts) {
    JournalRecord r;
    r.kind = Rec::RiskReject;
    r.a = uint8_t(res);
    r.symbol = o.symbol;
    r.qty = o.qty;
    r.price = o.price;
    r.b = uint8_t(o.side);
    r.ts = ts;
    return r;
}

inline JournalRecord gating(bool enabled, uint32_t reasons, uint64_t ts) {
    JournalRecord r;
    r.kind = Rec::Gating;
    r.a = enabled ? 1 : 0;
    r.b = reasons;
    r.ts = ts;
    return r;
}

inline JournalRecord limits(uint64_t version, uint16_t sym, const SymbolLimits& l) {
    JournalRecord r;
    r.kind = Rec::Limits;
    r.id = version;
    r.symbol = sym;
    r.qty = l.max_order_qty;
    r.price = l.max_position;
    r.b = l.band_ticks;
    r.c = l.rate_per_sec;
    r.d = l.burst;
    r.e = l.max_notional;
    return r;
}
inline SymbolLimits limits_from(const JournalRecord& r) {
    SymbolLimits l;
    l.max_order_qty = r.qty;
    l.max_position = r.price;
    l.band_ticks = r.b;
    l.rate_per_sec = uint32_t(r.c);
    l.burst = uint32_t(r.d);
    l.max_notional = r.e;
    return l;
}

inline JournalRecord control_applied(const ControlBlock& cb) {
    JournalRecord r;
    r.kind = Rec::ControlApplied;
    r.id = cb.version;
    r.a = cb.kill ? 1 : 0;
    r.symbol = uint16_t(cb.num_symbols);
    return r;
}

inline JournalRecord run_end(uint64_t orders, uint64_t market_msgs, uint64_t dropped, uint64_t hot_allocs) {
    JournalRecord r;
    r.kind = Rec::RunEnd;
    r.id = orders;
    r.seq = market_msgs;
    r.c = dropped;
    r.d = hot_allocs;
    return r;
}

}  // namespace jr
}  // namespace hp
