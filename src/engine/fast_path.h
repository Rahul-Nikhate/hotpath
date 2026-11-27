// The fused fast path: one thread, busy-polling, no queue hops on the critical
// path. Each loop iteration:
//
//   feed.poll  -> for each in-order message: Core::on_market (book, strategy,
//                 risk) -> on_order: gateway.send_order (patch template, send)
//   gw.poll    -> order responses -> Core::on_order_event (positions, states)
//   every ~100 us (cold): control seqlock, heartbeats, gating, reconnects,
//                 hot-path warming
//
// Journaling happens after the send, never before it: the market record for a
// message is held back until the core has decided, so writing the journal is
// not on the tick-to-order path, yet the journal order (market, then order) is
// exactly the order the core saw. Feed, Gw are template parameters so tests and
// benchmarks can substitute in-memory versions with identical hot-path code.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "common/alloc_counter.h"
#include "common/clock.h"
#include "common/histogram.h"
#include "engine/core.h"
#include "engine/journal.h"
#include "engine/types.h"
#include "shm/layout.h"

namespace hp {

struct FastPathConfig {
    uint64_t warm_interval_ns = 0;            // 0 = no warming
    uint64_t control_timeout_ns = 1'000'000'000;
    bool require_control = true;              // false: use `default_control` (tests, benches)
    ControlBlock default_control{};
    uint64_t warmup_msgs = 10'000;            // latency stats + allocation baseline start after this
    uint64_t maintain_interval_ns = 100'000;
    uint64_t drain_grace_ns = 2'000'000'000;  // after feed end, wait this long for open orders
    uint64_t expected_orders = 1u << 16;      // journaled for replay
};

enum class Metric : uint8_t {
    Decision = 0,  // packet received -> order bytes ready     (includes waiting behind earlier
                   //                                           messages of the same datagram)
    DecisionCode,  // message delivered -> order bytes ready   (our code only)
    ToWire,        // packet received -> send() returned       (includes kernel TCP)
    Feed,          // packet received -> message delivered     (parse + sequencing)
    BookApply,     // delivered -> book updated (every message)
    StrategyStage, // book updated -> decision
    RiskStage,     // decision -> risk passed
    EncodeStage,   // risk passed -> template patched
    SendStage,     // template patched -> send() returned
    Count
};

inline const char* metric_name(Metric m) {
    static const char* names[] = {"decision (rx->bytes)", "decision (msg->bytes)", "to-wire (rx->send)", "feed (rx->delivered)",
                                  "book apply", "strategy", "risk", "encode (template)", "send() syscall"};
    return names[size_t(m)];
}

struct FastPathStats {
    uint64_t iterations = 0, orders = 0, send_failures = 0, risk_rejects = 0, lost = 0;
    uint64_t gating_changes = 0, control_versions = 0, warms = 0, events = 0;
    uint64_t hot_allocs = 0;  // allocations on this thread after warm-up
    bool finished_clean = false;
};

struct CycleProbe {
    std::array<uint64_t, size_t(Stage::Count)> t{};
    void mark(Stage s) { t[size_t(s)] = cycles(); }
    uint64_t ns(Stage a, Stage b) const { return t[size_t(b)] > t[size_t(a)] ? cycles_to_ns(t[size_t(b)] - t[size_t(a)]) : 0; }
};

template <Strategy S, class Feed, class Gw>
class FastPath {
public:
    FastPath(Core<S>& core, Feed& feed, Gw& gw, JournalWriter journal, SharedLayout* shm, const FastPathConfig& cfg)
        : core_(core), feed_(feed), gw_(gw), jr_(journal), shm_(shm), cfg_(cfg) {
        lost_scratch_.reserve(1024);
        jr_.put(jr::run_start(core_.strategy().params(), core_.num_symbols(), cfg_.expected_orders));
        if (!cfg_.require_control) apply_control(cfg_.default_control);
        if (shm_) shm_->hdr.engine_state.store(uint32_t(EngineState::Running), std::memory_order_release);
    }

    // One iteration. Returns false once the run is over.
    bool step() {
        ++stats_.iterations;
        // The loop only needs wall time for maintenance (every ~100 us), so it
        // checks the CPU cycle counter (one register read) and reads the real
        // clock only when maintenance is due. Profiling with callgrind showed
        // the previous per-iteration steady_clock::now() at ~7% of the engine's
        // instructions. Between ticks, I/O sees a timestamp at most one
        // maintenance interval old, which only feeds coarse timeouts.
        const uint64_t c = cycles();
        const bool due = c >= next_maintain_cycles_;
        if (due) [[unlikely]] {
            now_ = now_ns();
            next_maintain_cycles_ = c + maintain_interval_cycles_;
        }

        feed_.poll(now_, [this](uint64_t seq, const feed::Message& m, uint64_t rx) { on_market(seq, m, rx); });
        gw_.poll(now_, [this](const proto::OrderEvent& e) { on_event(e); });

        return due ? maintain(now_) : true;
    }

    void run(const std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_relaxed) && step()) {
        }
        finish();
    }

    void finish() {
        if (finished_) return;
        finished_ = true;
        if (alloc_base_set_) stats_.hot_allocs = thread_allocations() - alloc_base_;
        update_gating(now_ns(), kGateStopping);
        jr_.put(jr::run_end(core_.stats().orders, core_.stats().market_msgs, jr_.dropped(), stats_.hot_allocs));
        if (shm_) shm_->hdr.engine_state.store(uint32_t(EngineState::Finished), std::memory_order_release);
    }

    // ---- Out interface for Core (called from inside on_market) -------------
    void on_order(uint64_t token, const OrderIntent& o, uint64_t ts) {
        const bool ok = gw_.send_order(token, o, probe_);  // marks Encoded and Sent
        if (record_latency_) {
            h(Metric::Decision).record(probe_.ns(Stage::Rx, Stage::Encoded));
            h(Metric::DecisionCode).record(probe_.ns(Stage::Delivered, Stage::Encoded));
            h(Metric::ToWire).record(probe_.ns(Stage::Rx, Stage::Sent));
            h(Metric::StrategyStage).record(probe_.ns(Stage::Book, Stage::Decision));
            h(Metric::RiskStage).record(probe_.ns(Stage::Decision, Stage::Risk));
            h(Metric::EncodeStage).record(probe_.ns(Stage::Risk, Stage::Encoded));
            h(Metric::SendStage).record(probe_.ns(Stage::Encoded, Stage::Sent));
        }
        flush_market();
        jr_.put(jr::order_sent(token, o, ts, ok));
        ++stats_.orders;
        if (!ok) ++stats_.send_failures;
    }
    void on_risk_reject(RiskResult r, const OrderIntent& o, uint64_t ts) {
        flush_market();
        jr_.put(jr::risk_reject(r, o, ts));
        ++stats_.risk_rejects;
    }
    void on_warm(const OrderIntent& o) {
        gw_.encode_dry(o);
        ++stats_.warms;
    }

    const Histogram& hist(Metric m) const { return hists_[size_t(m)]; }
    const FastPathStats& stats() const { return stats_; }
    const JournalWriter& journal() const { return jr_; }

private:
    Histogram& h(Metric m) { return hists_[size_t(m)]; }

    void on_market(uint64_t seq, const feed::Message& m, uint64_t rx) {
        probe_.t[size_t(Stage::Rx)] = rx;
        probe_.mark(Stage::Delivered);
        pending_market_ = jr::market(seq, m);
        has_pending_market_ = true;
        ++delivered_;
        record_latency_ = delivered_ > cfg_.warmup_msgs;
        if (delivered_ == cfg_.warmup_msgs + 1) {
            alloc_base_ = thread_allocations();
            alloc_base_set_ = true;
        }

        core_.on_market(m, probe_, *this);

        if (record_latency_) {
            h(Metric::Feed).record(probe_.ns(Stage::Rx, Stage::Delivered));
            h(Metric::BookApply).record(probe_.ns(Stage::Delivered, Stage::Book));
        }
        flush_market();
        last_activity_ns_ = 0;  // re-armed at the next maintenance tick
    }

    void flush_market() {
        if (!has_pending_market_) return;
        has_pending_market_ = false;
        jr_.put(pending_market_);
    }

    void on_event(const proto::OrderEvent& e) {
        jr_.put(jr::event(e));
        core_.on_order_event(e);
        ++stats_.events;
    }

    void apply_control(const ControlBlock& cb) {
        for (uint16_t s = 0; s < cb.num_symbols && s < kMaxSymbols; ++s) jr_.put(jr::limits(cb.version, s, cb.limits[s]));
        core_.apply_control(cb);
        jr_.put(jr::control_applied(cb));
        control_version_ = cb.version;
        have_control_ = true;
        ++stats_.control_versions;
    }

    void poll_control() {
        if (!shm_ || !cfg_.require_control) return;
        const uint64_t seq = shm_->control.sequence();
        if (seq == last_control_seq_ || (seq & 1)) return;
        last_control_seq_ = seq;
        const ControlBlock cb = shm_->control.load();
        if (cb.version != 0 && (!have_control_ || cb.version != control_version_)) apply_control(cb);
    }

    // After a reconnect's replay: orders still unacknowledged never arrived.
    void mark_lost_orders(uint64_t now) {
        lost_scratch_.clear();
        core_.om().for_each_pending_new([this](const OmOrder& o) { lost_scratch_.push_back(o.token); });
        for (uint64_t token : lost_scratch_) {
            proto::OrderEvent e;
            e.type = proto::Msg::Lost;
            e.token = token;
            e.ts = now;
            on_event(e);
            ++stats_.lost;
        }
    }

    void update_gating(uint64_t now, uint32_t extra = 0) {
        uint32_t reasons = extra;
        if (!gw_.ready()) reasons |= kGateSession;
        if (feed_.has_gap()) reasons |= kGateFeedGap;
        if (cfg_.require_control) {
            const uint64_t hb = shm_ ? shm_->hdr.control_heartbeat_ns.load(std::memory_order_relaxed) : 0;
            if (!have_control_ || hb == 0 || now - hb > cfg_.control_timeout_ns) reasons |= kGateControl;
        }
        if (feed_.done()) reasons |= kGateStopping;
        const bool enable = reasons == 0;
        if (enable != core_.enabled()) {
            core_.set_enabled(enable);
            jr_.put(jr::gating(enable, reasons, now));
            ++stats_.gating_changes;
        }
    }

    bool maintain(uint64_t now) {
        gw_.maintain(now);
        if (gw_.take_resynced() && gw_.stats().logins > 1) mark_lost_orders(now);
        poll_control();
        update_gating(now);
        if (shm_) {
            shm_->hdr.engine_heartbeat_ns.store(now, std::memory_order_relaxed);
            shm_->hdr.engine_orders.store(core_.stats().orders, std::memory_order_relaxed);
            shm_->hdr.engine_market_msgs.store(core_.stats().market_msgs, std::memory_order_relaxed);
        }
        if (cfg_.warm_interval_ns && core_.enabled()) {
            if (last_activity_ns_ == 0) {
                last_activity_ns_ = now;
            } else if (now - last_activity_ns_ >= cfg_.warm_interval_ns) {
                core_.warm(warm_symbol_, *this);
                warm_symbol_ = uint16_t((warm_symbol_ + 1) % core_.num_symbols());
                last_activity_ns_ = now;
            }
        }
        // Done: the feed ended and every order reached a terminal state (or the
        // grace period expired, e.g. the exchange went away).
        if (feed_.done()) {
            if (feed_done_ns_ == 0) feed_done_ns_ = now;
            if (core_.om().live_orders() == 0) {
                stats_.finished_clean = true;
                return false;
            }
            if (now - feed_done_ns_ > cfg_.drain_grace_ns) return false;
        }
        return true;
    }

    Core<S>& core_;
    Feed& feed_;
    Gw& gw_;
    JournalWriter jr_;
    SharedLayout* shm_;
    FastPathConfig cfg_;
    FastPathStats stats_;
    CycleProbe probe_;
    std::array<Histogram, size_t(Metric::Count)> hists_{};
    JournalRecord pending_market_{};
    bool has_pending_market_ = false;
    bool record_latency_ = false;
    uint64_t delivered_ = 0;
    uint64_t alloc_base_ = 0;
    bool alloc_base_set_ = false;
    uint64_t now_ = now_ns();  // refreshed at each maintenance tick
    uint64_t next_maintain_cycles_ = 0;
    uint64_t maintain_interval_cycles_ =
        uint64_t(double(cfg_.maintain_interval_ns) / (g_ns_per_cycle > 0 ? g_ns_per_cycle : 1.0));
    uint64_t last_activity_ns_ = 0, feed_done_ns_ = 0;
    uint64_t last_control_seq_ = UINT64_MAX, control_version_ = 0;
    bool have_control_ = false;
    bool finished_ = false;
    uint16_t warm_symbol_ = 0;
    std::vector<uint64_t> lost_scratch_;
};

}  // namespace hp
