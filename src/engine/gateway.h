// Order-entry gateway: a non-blocking, busy-polled TCP session client.
//
// Session rules (SoupBinTCP-style, see docs/PROTOCOL.md):
//   * Login carries the next sequenced message number we expect. The server
//     replays everything from there, then continues live. The gateway is
//     Ready only once the replay has caught up (LoginAccepted.end_seq).
//   * Heartbeats both ways every second; 3 s of silence = connection dead.
//   * We never resend an order automatically. After a reconnect, any order
//     still PendingNew once the replay is complete provably never reached the
//     exchange (the exchange acknowledges every order it receives, and the
//     replay contains every acknowledgement), so the engine marks it Lost.
//     That is how a reconnect produces no duplicate and no phantom orders.
//
// The send path patches a pre-encoded template (proto/order_template.h) and
// issues one send(): TCP_NODELAY is set, so the 24-byte frame goes out at once.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/clock.h"
#include "engine/types.h"
#include "proto/order_entry.h"
#include "proto/order_template.h"

namespace hp {

struct GatewayConfig {
    std::string ip = "127.0.0.1";
    uint16_t port = 10100;
    std::string user = "HPATH1";
    std::string pass = "password";
    uint16_t num_symbols = 8;
    uint64_t heartbeat_ns = 1'000'000'000;
    uint64_t timeout_ns = 3'000'000'000;
    uint64_t reconnect_ns = 100'000'000;
    bool auto_reconnect = true;
};

enum class GwState : uint8_t { Disconnected, LoggingIn, Resyncing, Ready };

struct GwStats {
    uint64_t logins = 0, disconnects = 0, connect_failures = 0, login_rejects = 0;
    uint64_t orders_sent = 0, send_failures = 0, partial_sends = 0, cancels_sent = 0;
    uint64_t frames_in = 0, events_in = 0, heartbeats_in = 0, heartbeats_out = 0, bad_frames = 0;
};

class Gateway {
public:
    explicit Gateway(const GatewayConfig& cfg);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // One connection attempt (blocking TCP connect: cold path), then sends the
    // login request. Returns false if the server is not reachable.
    bool connect(uint64_t now);

    GwState state() const { return state_; }
    bool ready() const { return state_ == GwState::Ready; }
    bool session_over() const { return session_over_; }

    // Hot path.
    template <class Probe>
    bool send_order(uint64_t token, const OrderIntent& o, Probe& probe) {
        const uint8_t* f = templates_.patch(o.symbol, o.side, token, o.qty, o.price);
        probe.mark(Stage::Encoded);
        const bool ok = state_ == GwState::Ready && write(f, proto::kEnterFrame);
        probe.mark(Stage::Sent);
        if (ok) ++stats_.orders_sent;
        else ++stats_.send_failures;
        return ok;
    }
    // Touches the same template and code without sending (hot-path warming).
    void encode_dry(const OrderIntent& o) { (void)templates_.patch(o.symbol, o.side, 0, o.qty, o.price); }

    bool send_cancel(uint64_t token, uint32_t qty);

    // Reads whatever has arrived and calls on_event(const proto::OrderEvent&)
    // for each sequenced order response, in order.
    template <class F>
    void poll(uint64_t now, F&& on_event) {
        if (fd_ < 0) return;
        if (!read_available(now)) return;
        size_t off = 0;
        while (off < rx_len_) {
            proto::Frame f;
            size_t used = 0;
            const proto::Parse p = proto::parse_frame(rx_.data() + off, rx_len_ - off, f, used);
            if (p == proto::Parse::NeedMore) break;
            if (p == proto::Parse::Bad) {
                ++stats_.bad_frames;
                drop("bad frame");
                return;
            }
            off += used;
            ++stats_.frames_in;
            if (f.type == proto::Pkt::Sequenced) {
                proto::OrderEvent e;
                ++next_seq_;
                if (proto::decode_event(f.payload, f.len, e)) {
                    ++stats_.events_in;
                    on_event(e);
                } else {
                    ++stats_.bad_frames;
                }
                if (state_ == GwState::Resyncing && next_seq_ >= end_seq_) become_ready();
            } else if (!handle_session_frame(f, now)) {
                return;  // connection dropped inside
            }
        }
        compact(off);
    }

    // Heartbeats, timeout detection, reconnects, flushing partial writes.
    void maintain(uint64_t now);

    // True exactly once after each login has finished its replay.
    bool take_resynced() {
        const bool r = resynced_;
        resynced_ = false;
        return r;
    }

    void logout();
    uint64_t next_seq() const { return next_seq_; }
    const GwStats& stats() const { return stats_; }

private:
    bool read_available(uint64_t now);  // false if the connection dropped
    bool handle_session_frame(const proto::Frame& f, uint64_t now);
    bool write(const uint8_t* p, size_t n);
    void flush();
    void compact(size_t consumed);
    void become_ready();
    void drop(const char* why);

    GatewayConfig cfg_;
    proto::OrderTemplates templates_;
    int fd_ = -1;
    GwState state_ = GwState::Disconnected;
    bool session_over_ = false;
    bool resynced_ = false;
    bool sent_since_maintain_ = false;
    uint64_t next_seq_ = 1;  // next sequenced server message we expect
    uint64_t end_seq_ = 1;
    uint64_t last_rx_ns_ = 0, last_tx_ns_ = 0, last_attempt_ns_ = 0;
    std::vector<uint8_t> rx_;
    size_t rx_len_ = 0;
    std::vector<uint8_t> tx_pending_;  // bytes a partial send() left behind
    GwStats stats_;
};

}  // namespace hp
