// In-memory stand-ins for FeedAdapter and Gateway, used by tests and the
// fast-path benchmark. They expose the same interface, so FastPath runs the
// identical strategy/risk/template code; only the socket I/O is removed.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "common/clock.h"
#include "engine/gateway.h"  // GwStats
#include "engine/types.h"
#include "feed_generator.h"  // project 1
#include "proto/order_entry.h"
#include "proto/order_template.h"

namespace hp {

// Deterministic synthetic market data (project 1's generator). interval_ns > 0
// spaces messages out in time (sparse-event experiments).
class FakeFeed {
public:
    FakeFeed(uint64_t seed, uint16_t symbols, uint64_t count, uint64_t interval_ns = 0)
        : gen_(seed, symbols, 20000), count_(count), interval_ns_(interval_ns) {}

    template <class F>
    int poll(uint64_t now, F&& deliver) {
        if (sent_ >= count_) return 0;
        if (interval_ns_ && now < next_due_) return 0;
        feed::Message m = gen_.next();
        m.timestamp_ns = ++logical_ns_ * 1000;  // deterministic event time (1 us apart)
        ++sent_;
        next_due_ = now + interval_ns_;
        deliver(sent_, m, cycles());
        return 1;
    }
    bool has_gap() const { return false; }
    bool done() const { return sent_ >= count_; }
    uint64_t sent() const { return sent_; }

private:
    feed::FeedGenerator gen_;
    uint64_t count_, interval_ns_;
    uint64_t sent_ = 0, next_due_ = 0, logical_ns_ = 0;
};

// Accepts every order and answers on the next poll: Accepted, then either a
// full fill (fill=true) or an IOC cancel. Fixed-size event ring, no allocation.
class FakeGateway {
public:
    explicit FakeGateway(uint16_t symbols, bool fill = false) : tpl_(symbols), fill_(fill) { stats_.logins = 1; }

    template <class Probe>
    bool send_order(uint64_t token, const OrderIntent& o, Probe& probe) {
        const uint8_t* f = tpl_.patch(o.symbol, o.side, token, o.qty, o.price);
        probe.mark(Stage::Encoded);
        std::memcpy(wire_.data(), f, proto::kEnterFrame);  // stands in for send()
        probe.mark(Stage::Sent);
        proto::OrderEvent a;
        a.type = proto::Msg::Accepted;
        a.token = token;
        a.qty = o.qty;
        a.price = o.price;
        a.symbol = o.symbol;
        a.side = o.side;
        push(a);
        proto::OrderEvent r;
        r.type = fill_ ? proto::Msg::Executed : proto::Msg::Canceled;
        r.token = token;
        r.qty = o.qty;
        r.price = o.price;
        push(r);
        ++stats_.orders_sent;
        return ready_;
    }
    void encode_dry(const OrderIntent& o) { (void)tpl_.patch(o.symbol, o.side, 0, o.qty, o.price); }

    template <class F>
    void poll(uint64_t, F&& on_event) {
        while (tail_ != head_) on_event(ring_[tail_++ & (kRing - 1)]);
    }
    void maintain(uint64_t) {}
    bool ready() const { return ready_; }
    void set_ready(bool r) { ready_ = r; }
    bool take_resynced() { return false; }
    const GwStats& stats() const { return stats_; }

private:
    static constexpr size_t kRing = 4096;
    void push(const proto::OrderEvent& e) { ring_[head_++ & (kRing - 1)] = e; }

    proto::OrderTemplates tpl_;
    bool fill_;
    bool ready_ = true;
    std::array<uint8_t, proto::kEnterFrame> wire_{};
    std::array<proto::OrderEvent, kRing> ring_{};
    uint64_t head_ = 0, tail_ = 0;
    GwStats stats_;
};

}  // namespace hp
