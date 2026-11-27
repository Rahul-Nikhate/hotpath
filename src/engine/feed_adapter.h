// Market-data input for the fast path, built from project 1's pieces:
// PacketReader (parsing), Sequencer (gap detection / reordering), and
// RecoveryClient (TCP retransmission, on its own thread, off the hot path).
//
// Unlike project 1's Handler, which owns its loop, this is poll-based so the
// fast path can interleave market data, order responses and control in one
// thread. Each datagram is timestamped (cycles) the moment recv() returns;
// that is the "tick" in tick-to-trade.
#pragma once

#include <sys/socket.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/clock.h"
#include "handler.h"    // project 1: feed::RecoveryClient
#include "protocol.h"   // project 1
#include "sequencer.h"  // project 1

namespace hp {

struct FeedConfig {
    std::string udp_ip = "127.0.0.1";  // bind address or multicast group
    uint16_t udp_port = 9999;          // 0 = ephemeral
    bool multicast = false;
    std::string iface = "127.0.0.1";
    std::string recovery_ip = "127.0.0.1";
    uint16_t recovery_port = 10000;
    int rcvbuf = 8 << 20;
    uint64_t reorder_wait_ns = 200'000;  // wait for late packets before requesting a resend
    size_t max_pending = 1u << 20;
};

struct FeedStats {
    uint64_t datagrams = 0, msgs = 0, heartbeats = 0, malformed = 0, wrong_session = 0;
    uint64_t recovery_requests = 0, recovery_failures = 0, recovered = 0;
    int rcvbuf_actual = 0;
};

class FeedAdapter {
public:
    explicit FeedAdapter(const FeedConfig& cfg);
    ~FeedAdapter();
    FeedAdapter(const FeedAdapter&) = delete;
    FeedAdapter& operator=(const FeedAdapter&) = delete;

    void start();  // starts the recovery client thread
    void set_recovery_port(uint16_t p) { cfg_.recovery_port = p; }  // before start()
    uint16_t port() const { return port_; }

    // deliver(uint64_t seq, const feed::Message&, uint64_t rx_cycles) for every
    // message that becomes in-order. Returns datagrams read.
    template <class F>
    int poll(uint64_t now, F&& deliver) {
        int n = 0;
        for (; n < kBatch; ++n) {
            const ssize_t r = ::recv(fd_, buf_.data(), buf_.size(), 0);
            if (r < 0) break;
            const uint64_t rx = cycles();
            on_datagram(buf_.data(), size_t(r), rx, deliver);
        }
        handle_recovery(now, deliver);
        return n;
    }

    bool has_gap() const { return seq_.has_gap(); }
    bool done() const { return session_end_ != 0 && seq_.next() >= session_end_; }
    const feed::SeqStats& seq_stats() const { return seq_.stats(); }
    const FeedStats& stats() const { return stats_; }

private:
    static constexpr int kBatch = 16;

    template <class F>
    void on_datagram(const uint8_t* p, size_t len, uint64_t rx, F& deliver) {
        ++stats_.datagrams;
        feed::PacketReader rd;
        if (!rd.init(p, len)) {
            ++stats_.malformed;
            return;
        }
        const feed::PacketHeader& h = rd.header();
        if (!have_session_) {
            std::memcpy(session_, h.session, feed::kSessionLen);
            have_session_ = true;
        } else if (std::memcmp(session_, h.session, feed::kSessionLen) != 0) {
            ++stats_.wrong_session;
            return;
        }
        if (rd.end_of_session()) {
            session_end_ = h.seq;
            seq_.on_known_end(h.seq);
            return;
        }
        if (h.count == 0) {
            ++stats_.heartbeats;
            seq_.on_known_end(h.seq);
            return;
        }
        uint64_t seq = h.seq;
        feed::Message m;
        while (rd.next(m)) {
            seq_.on_message(seq, m, [&](uint64_t s, const feed::Message& msg) { deliver(s, msg, rx); });
            ++seq;
            ++stats_.msgs;
        }
        if (rd.malformed()) ++stats_.malformed;
        seq_.on_known_end(h.seq + h.count);
    }

    template <class F>
    void handle_recovery(uint64_t now, F& deliver) {
        if (!in_flight_ && !seq_.has_gap()) return;  // fast exit: the common case
        feed::RecoveryClient::Response r;
        const uint64_t rx = cycles();
        auto d = [&](uint64_t s, const feed::Message& msg) { deliver(s, msg, rx); };
        while (recovery_.poll(r)) {
            in_flight_ = false;
            if (!r.ok) {
                ++stats_.recovery_failures;
                gap_since_ns_ = now;
                continue;
            }
            if (r.first > seq_.next()) seq_.skip_to(r.first, d);
            for (size_t i = 0; i < r.msgs.size(); ++i)
                if (seq_.on_message(r.first + i, r.msgs[i], d) != feed::Sequencer::Result::Duplicate)
                    ++stats_.recovered;
        }
        if (!seq_.has_gap()) {
            gap_since_ns_ = 0;
            return;
        }
        if (gap_since_ns_ == 0) gap_since_ns_ = now;
        if (in_flight_ || now - gap_since_ns_ < cfg_.reorder_wait_ns) return;
        const uint64_t from = seq_.next();
        recovery_.request(from, std::min<uint64_t>(seq_.gap_end() - from, feed::kMaxRecoveryMsgs));
        in_flight_ = true;
        ++stats_.recovery_requests;
    }

    FeedConfig cfg_;
    int fd_ = -1;
    uint16_t port_ = 0;
    feed::Sequencer seq_;
    feed::RecoveryClient recovery_;
    std::vector<uint8_t> buf_;
    FeedStats stats_;
    bool have_session_ = false;
    char session_[feed::kSessionLen] = {};
    uint64_t session_end_ = 0;
    bool in_flight_ = false;
    uint64_t gap_since_ns_ = 0;
};

}  // namespace hp
