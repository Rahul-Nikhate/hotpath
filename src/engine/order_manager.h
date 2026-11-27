// Order state machine, positions and open exposure.
//
//   send ─► PendingNew ─Accepted─► Live ─Executed(all)─► Filled
//              │                    │ └─Canceled(rest)─► Cancelled
//              ├─Rejected─► Rejected└─(cancel sent)─► PendingCancel ─Canceled─► Cancelled
//              └─(never reached the exchange)─► Lost          └─Executed (fill raced the cancel)
//
// Storage is a fixed array indexed by token (tokens are sequential), allocated
// once: no allocation per order. Open quantity per side feeds the risk
// position check, so an order counts against the limit from the moment it is
// sent until it is filled, cancelled, rejected or lost.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "engine/types.h"
#include "proto/order_entry.h"
#include "shm/layout.h"

namespace hp {

enum class OrdState : uint8_t { Free = 0, PendingNew, Live, PendingCancel, Filled, Cancelled, Rejected, Lost };

inline bool is_terminal(OrdState s) {
    return s == OrdState::Free || s == OrdState::Filled || s == OrdState::Cancelled || s == OrdState::Rejected ||
           s == OrdState::Lost;
}

struct OmOrder {
    uint64_t token = 0;
    uint32_t price = 0, qty = 0, leaves = 0, filled = 0;
    uint16_t symbol = 0;
    Side side = Side::Buy;
    OrdState state = OrdState::Free;
};

struct OmStats {
    uint64_t sent = 0, accepted = 0, fills = 0, filled_qty = 0, cancelled = 0, rejected = 0, lost = 0;
    uint64_t unknown_events = 0, bad_transitions = 0, slot_collisions = 0;
};

class OrderManager {
public:
    static constexpr size_t kCapacity = 1u << 16;  // max orders in flight at once

    OrderManager() : slots_(kCapacity) {}

    void on_send(uint64_t token, const OrderIntent& o) {
        OmOrder& s = slots_[token & (kCapacity - 1)];
        if (!is_terminal(s.state)) {  // 64K orders still open: a configuration error
            ++stats_.slot_collisions;
            release(s);
            --live_;
        }
        s = OmOrder{token, o.price, o.qty, o.qty, 0, o.symbol, o.side, OrdState::PendingNew};
        open(s.side, s.symbol) += o.qty;
        ++live_;
        ++stats_.sent;
    }

    void on_cancel_sent(uint64_t token) {
        if (OmOrder* s = find(token); s && (s->state == OrdState::Live || s->state == OrdState::PendingNew))
            s->state = OrdState::PendingCancel;
    }

    void on_event(const proto::OrderEvent& e) {
        OmOrder* s = find(e.token);
        if (!s) {
            ++stats_.unknown_events;
            return;
        }
        if (is_terminal(s->state)) {
            ++stats_.bad_transitions;
            return;
        }
        switch (e.type) {
            case proto::Msg::Accepted:
                ++stats_.accepted;
                if (s->state == OrdState::PendingNew) s->state = OrdState::Live;
                break;
            case proto::Msg::Executed: {
                const uint32_t q = std::min(e.qty, s->leaves);
                s->leaves -= q;
                s->filled += q;
                open(s->side, s->symbol) -= q;
                const int64_t signed_q = s->side == Side::Buy ? int64_t(q) : -int64_t(q);
                position_[s->symbol] += signed_q;
                cash_[s->symbol] -= signed_q * int64_t(e.price);
                ++stats_.fills;
                stats_.filled_qty += q;
                if (s->leaves == 0) finish(*s, OrdState::Filled);
                break;
            }
            case proto::Msg::Canceled: {
                const uint32_t q = e.qty == 0 ? s->leaves : std::min(e.qty, s->leaves);
                s->leaves -= q;
                open(s->side, s->symbol) -= q;
                if (s->leaves == 0) {
                    ++stats_.cancelled;
                    finish(*s, OrdState::Cancelled);
                }
                break;
            }
            case proto::Msg::Rejected:
                ++stats_.rejected;
                release(*s);
                finish(*s, OrdState::Rejected);
                break;
            case proto::Msg::Lost:
                if (s->state != OrdState::PendingNew) {
                    ++stats_.bad_transitions;
                    break;
                }
                ++stats_.lost;
                release(*s);
                finish(*s, OrdState::Lost);
                break;
            default:
                ++stats_.unknown_events;
        }
    }

    // Visits every order still waiting for its first exchange response.
    template <class F>
    void for_each_pending_new(F&& f) const {
        for (const OmOrder& s : slots_)
            if (s.state == OrdState::PendingNew) f(s);
    }

    const OmOrder* get(uint64_t token) const {
        const OmOrder& s = slots_[token & (kCapacity - 1)];
        return s.token == token && s.state != OrdState::Free ? &s : nullptr;
    }

    int64_t position(uint16_t sym) const { return position_[sym & (kMaxSymbols - 1)]; }
    int64_t cash(uint16_t sym) const { return cash_[sym & (kMaxSymbols - 1)]; }
    uint64_t open_buy(uint16_t sym) const { return open_buy_[sym & (kMaxSymbols - 1)]; }
    uint64_t open_sell(uint16_t sym) const { return open_sell_[sym & (kMaxSymbols - 1)]; }
    uint64_t live_orders() const { return live_; }
    const OmStats& stats() const { return stats_; }

private:
    OmOrder* find(uint64_t token) {
        OmOrder& s = slots_[token & (kCapacity - 1)];
        return s.token == token && s.state != OrdState::Free ? &s : nullptr;
    }
    uint64_t& open(Side side, uint16_t sym) {
        return side == Side::Buy ? open_buy_[sym & (kMaxSymbols - 1)] : open_sell_[sym & (kMaxSymbols - 1)];
    }
    void release(OmOrder& s) {  // drop remaining open quantity
        open(s.side, s.symbol) -= s.leaves;
        s.leaves = 0;
    }
    void finish(OmOrder& s, OrdState st) {
        s.state = st;
        --live_;
    }

    std::vector<OmOrder> slots_;
    std::array<int64_t, kMaxSymbols> position_{}, cash_{};
    std::array<uint64_t, kMaxSymbols> open_buy_{}, open_sell_{};
    uint64_t live_ = 0;
    OmStats stats_;
};

}  // namespace hp
