// Exchange simulator (test harness, not the showcase).
//
// One thread, one busy loop, so there are no locks and its behaviour is easy to
// reason about:
//   * Market data: project 1's deterministic FeedGenerator -> MoldUDP64-style
//     UDP packets (unicast or multicast), with optional simulated packet loss.
//   * Recovery: project 1's TCP retransmission protocol, served from history.
//   * Order entry: one SoupBinTCP-style session. IOC orders are filled against
//     the simulator's own top of book at the moment they arrive (no market
//     impact: the public book is not changed by our fills), the remainder is
//     cancelled. Every response is kept in a sequenced log so a client that
//     reconnects can be replayed exactly what it missed.
//   * Fault injection: close the session once after N orders, *before* sending
//     that order's responses, to force a replay on reconnect.
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "feed_generator.h"  // project 1
#include "order_book.h"      // project 1
#include "proto/order_entry.h"
#include "protocol.h"        // project 1

namespace hp {

struct SimConfig {
    std::string feed_ip = "127.0.0.1";  // unicast destination or multicast group
    uint16_t feed_port = 9999;
    bool multicast = false;
    std::string iface = "127.0.0.1";
    std::string bind_ip = "127.0.0.1";
    uint16_t recovery_port = 10000;     // 0 = ephemeral
    uint16_t order_port = 10100;        // 0 = ephemeral
    uint64_t count = 1'000'000;         // market-data messages to publish
    uint64_t rate = 100'000;            // messages/s (0 = as fast as possible)
    size_t batch = 8;                   // messages per datagram (max)
    double drop_rate = 0.0;             // simulated datagram loss
    uint64_t seed = 42;
    uint16_t symbols = 8;
    size_t max_live = 20000;
    size_t history = 1u << 20;          // messages kept for retransmission
    uint64_t disconnect_after_orders = 0;  // fault: drop the session once after N orders
    double linger_s = 1.0;              // keep serving order entry after the feed ends
    double login_wait_s = 10.0;         // wait this long for a client before publishing anyway
    bool quiet = false;
};

struct SimStats {
    uint64_t messages = 0, datagrams = 0, dropped_sim = 0, send_errors = 0;
    uint64_t retrans_requests = 0, retrans_messages = 0;
    uint64_t logins = 0, orders = 0, fills = 0, filled_qty = 0, ioc_cancels = 0, rejects = 0;
    uint64_t duplicate_tokens = 0, injected_disconnects = 0, replayed = 0, client_timeouts = 0;
};

class SimExchange {
public:
    explicit SimExchange(const SimConfig& cfg);  // binds all sockets
    ~SimExchange();
    SimExchange(const SimExchange&) = delete;
    SimExchange& operator=(const SimExchange&) = delete;

    // Runs until the feed is published, end-of-session sent and the linger
    // period has passed (or *stop becomes true).
    void run(const std::atomic<bool>* stop = nullptr);

    uint16_t recovery_port() const { return recovery_port_; }
    uint16_t order_port() const { return order_port_; }
    uint64_t reference_hash() const { return book_.hash(); }  // after run()
    const SimStats& stats() const { return stats_; }
    size_t unique_tokens() const { return tokens_.size(); }

private:
    // market data
    void publish(size_t n, uint64_t now);
    void send_control(uint16_t count, uint64_t now);
    // recovery
    void poll_recovery();
    void serve_recovery_request();
    // order entry
    void poll_orders(uint64_t now);
    void accept_client(uint64_t now);
    bool read_client(uint64_t now);
    void handle_frame(const proto::Frame& f, uint64_t now);
    void handle_login(const proto::LoginRequest& r, uint64_t now);
    void handle_enter(const proto::EnterOrder& e, uint64_t now);
    void emit(const proto::OrderEvent& e);  // log + send
    void send_bytes(const uint8_t* p, size_t n);
    void flush_client();
    void close_client(const char* why);

    SimConfig cfg_;
    char session_[feed::kSessionLen];
    feed::FeedGenerator gen_;
    feed::Books book_;
    feed::SplitMix64 drop_rng_;
    feed::PacketBuilder pkt_;
    int udp_fd_ = -1, rec_listen_ = -1, rec_client_ = -1, ord_listen_ = -1, client_ = -1;
    uint16_t recovery_port_ = 0, order_port_ = 0;
    sockaddr_in dest_{};
    uint64_t next_seq_ = 1, last_feed_send_ns_ = 0;
    std::vector<feed::Message> history_;
    uint64_t hist_end_ = 1;

    // order session
    bool logged_in_ = false, ever_logged_in_ = false, fault_done_ = false;
    std::vector<uint8_t> rx_;
    size_t rx_len_ = 0;
    std::vector<uint8_t> tx_pending_;
    std::vector<uint8_t> log_;            // sequenced responses, framed
    std::vector<uint32_t> log_offsets_;   // start of message i (seq = i + 1)
    std::unordered_set<uint64_t> tokens_;
    uint64_t order_ref_ = 0, match_id_ = 0;
    uint64_t last_client_rx_ns_ = 0, last_client_tx_ns_ = 0, last_order_activity_ns_ = 0;
    bool suppress_send_ = false;          // fault injection in progress

    SimStats stats_;
};

}  // namespace hp
