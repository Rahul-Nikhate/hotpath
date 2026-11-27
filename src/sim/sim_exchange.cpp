#include "sim/sim_exchange.h"

#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "common/clock.h"
#include "net.h"  // project 1

namespace hp {
namespace {
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void set_nonblocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }
}  // namespace

SimExchange::SimExchange(const SimConfig& cfg)
    : cfg_(cfg),
      gen_(cfg.seed, cfg.symbols, cfg.max_live),
      book_(cfg.symbols, cfg.max_live),
      drop_rng_(cfg.seed ^ 0xD5A61266F0C9392CULL),
      history_(std::max<size_t>(cfg.history, 1)),
      rx_(64 * 1024) {
    cfg_.batch = std::clamp<size_t>(cfg_.batch, 1, feed::kMaxMsgsPerDatagram);
    feed::set_session(session_, "HPSIM00001");
    udp_fd_ = net::udp_sender(cfg_.feed_ip, cfg_.feed_port, cfg_.multicast, cfg_.iface, dest_);
    rec_listen_ = net::tcp_listen(cfg_.bind_ip, cfg_.recovery_port);
    ord_listen_ = net::tcp_listen(cfg_.bind_ip, cfg_.order_port);
    set_nonblocking(rec_listen_);
    set_nonblocking(ord_listen_);
    recovery_port_ = net::local_port(rec_listen_);
    order_port_ = net::local_port(ord_listen_);
    tx_pending_.reserve(1 << 16);
    log_.reserve(1 << 20);
    log_offsets_.reserve(1 << 16);
}

SimExchange::~SimExchange() {
    for (int fd : {udp_fd_, rec_listen_, rec_client_, ord_listen_, client_})
        if (fd >= 0) ::close(fd);
}

void SimExchange::run(const std::atomic<bool>* stop) {
    const uint64_t created = now_ns();
    uint64_t start = 0, sent = 0, eos_sent = 0, feed_done_ns = 0;
    for (;;) {
        if (stop && stop->load(std::memory_order_relaxed)) break;
        const uint64_t now = now_ns();
        poll_orders(now);
        poll_recovery();

        if (start == 0) {  // let the engine log in before the first tick
            if (ever_logged_in_ || double(now - created) * 1e-9 > cfg_.login_wait_s) start = now;
            else continue;
        }
        if (sent < cfg_.count) {
            uint64_t due = cfg_.count;
            if (cfg_.rate) due = std::min<uint64_t>(cfg_.count, uint64_t(double(now - start) * 1e-9 * double(cfg_.rate)) + 1);
            if (due > sent) {
                const size_t n = size_t(std::min<uint64_t>(cfg_.batch, due - sent));
                publish(n, now);
                sent += n;
                continue;
            }
            if (now - last_feed_send_ns_ > 100'000'000) send_control(0, now);  // feed heartbeat
            continue;
        }
        // End of session: send it a few times, 1 ms apart (UDP can drop it).
        if (eos_sent < 3) {
            if (now - last_feed_send_ns_ >= 1'000'000) {
                send_control(feed::kEndOfSession, now);
                ++eos_sent;
                if (feed_done_ns == 0) feed_done_ns = now;
            }
            continue;
        }
        const uint64_t quiet_since = std::max(feed_done_ns, last_order_activity_ns_);
        if (double(now - quiet_since) * 1e-9 > cfg_.linger_s) break;
    }
    if (client_ >= 0) {
        uint8_t buf[proto::kFrameHdr];
        proto::encode_empty(buf, proto::Pkt::EndOfSession);
        send_bytes(buf, sizeof(buf));
        flush_client();
    }
}

// ---------------------------------------------------------------- market data

void SimExchange::publish(size_t n, uint64_t now) {
    pkt_.start(session_, next_seq_);
    for (size_t i = 0; i < n; ++i) {
        feed::Message m = gen_.next();
        m.timestamp_ns = now;
        pkt_.add(m);
        book_.apply(m);
        history_[next_seq_ % history_.size()] = m;
        ++next_seq_;
    }
    hist_end_ = next_seq_;
    stats_.messages += n;
    ++stats_.datagrams;
    last_feed_send_ns_ = now;
    if (cfg_.drop_rate > 0 && drop_rng_.uniform() < cfg_.drop_rate) {
        ++stats_.dropped_sim;
        return;
    }
    const ssize_t r = ::sendto(udp_fd_, pkt_.data(), pkt_.size(), 0, reinterpret_cast<const sockaddr*>(&dest_),
                               sizeof(dest_));
    if (r != ssize_t(pkt_.size())) ++stats_.send_errors;
}

void SimExchange::send_control(uint16_t count, uint64_t now) {
    uint8_t buf[feed::kHeaderLen];
    feed::PacketHeader h;
    std::memcpy(h.session, session_, feed::kSessionLen);
    h.seq = next_seq_;
    h.count = count;
    feed::encode_header(h, buf);
    ::sendto(udp_fd_, buf, sizeof(buf), 0, reinterpret_cast<const sockaddr*>(&dest_), sizeof(dest_));
    last_feed_send_ns_ = now;
}

// ---------------------------------------------------------------- recovery

void SimExchange::poll_recovery() {
    if (rec_client_ < 0) {
        const int c = ::accept(rec_listen_, nullptr, nullptr);
        if (c < 0) return;
        int one = 1;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        rec_client_ = c;
    }
    if (net::wait_readable(rec_client_, 0)) serve_recovery_request();
}

// Same wire protocol as project 1's exchange:
//   request  [u16 len=20][header: session, from_seq, count]
//   response [u16 len][header: session, first_available_seq, n][n message blocks]
void SimExchange::serve_recovery_request() {
    uint8_t req[2 + feed::kHeaderLen];
    if (!net::recv_all(rec_client_, req, sizeof(req)) || feed::be::get16(req) != feed::kHeaderLen) {
        ::close(rec_client_);
        rec_client_ = -1;
        return;
    }
    feed::PacketHeader h;
    feed::decode_header(req + 2, feed::kHeaderLen, h);
    ++stats_.retrans_requests;
    const uint64_t from = h.seq;
    const uint64_t end = from + std::min<uint64_t>(h.count, feed::kMaxRecoveryMsgs);
    const uint64_t oldest = hist_end_ > history_.size() ? hist_end_ - history_.size() : 1;
    const uint64_t first = std::min(std::max(from, oldest), end);
    const uint64_t last = std::max(first, std::min(end, hist_end_));
    feed::PacketBuilder reply(feed::kHeaderLen + feed::kMaxRecoveryMsgs * (2 + feed::kMaxMsgLen));
    reply.start(session_, first);
    for (uint64_t s = first; s < last; ++s) reply.add(history_[s % history_.size()]);
    stats_.retrans_messages += reply.count();
    std::vector<uint8_t> framed(2 + reply.size());
    feed::be::put16(framed.data(), uint16_t(reply.size()));
    std::copy(reply.data(), reply.data() + reply.size(), framed.begin() + 2);
    if (!net::send_all(rec_client_, framed.data(), framed.size())) {
        ::close(rec_client_);
        rec_client_ = -1;
    }
}

// ---------------------------------------------------------------- order entry

void SimExchange::poll_orders(uint64_t now) {
    accept_client(now);
    if (client_ < 0) return;
    if (!read_client(now)) return;
    size_t off = 0;
    while (client_ >= 0 && off < rx_len_) {
        proto::Frame f;
        size_t used = 0;
        const proto::Parse p = proto::parse_frame(rx_.data() + off, rx_len_ - off, f, used);
        if (p == proto::Parse::NeedMore) break;
        if (p == proto::Parse::Bad) {
            close_client("bad frame");
            return;
        }
        off += used;
        handle_frame(f, now);
    }
    if (client_ < 0) return;
    if (off) {
        std::memmove(rx_.data(), rx_.data() + off, rx_len_ - off);
        rx_len_ -= off;
    }
    flush_client();
    if (client_ >= 0 && logged_in_ && now - last_client_tx_ns_ > 1'000'000'000) {
        uint8_t buf[proto::kFrameHdr];
        proto::encode_empty(buf, proto::Pkt::ServerHeartbeat);
        send_bytes(buf, sizeof(buf));
        last_client_tx_ns_ = now;
    }
    if (client_ >= 0 && now - last_client_rx_ns_ > 3'000'000'000ULL) {
        ++stats_.client_timeouts;
        close_client("client heartbeat timeout");
    }
}

void SimExchange::accept_client(uint64_t now) {
    const int c = ::accept(ord_listen_, nullptr, nullptr);
    if (c < 0) return;
    if (client_ >= 0) close_client("replaced by a new connection");
    int one = 1;
    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    set_nonblocking(c);
    client_ = c;
    rx_len_ = 0;
    tx_pending_.clear();
    logged_in_ = false;
    last_client_rx_ns_ = last_client_tx_ns_ = now;
}

bool SimExchange::read_client(uint64_t now) {
    for (;;) {
        if (rx_len_ == rx_.size()) return true;
        const ssize_t r = ::recv(client_, rx_.data() + rx_len_, rx_.size() - rx_len_, 0);
        if (r > 0) {
            rx_len_ += size_t(r);
            last_client_rx_ns_ = now;
            continue;
        }
        if (r == 0) {
            close_client("client closed");
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        close_client("recv error");
        return false;
    }
}

void SimExchange::handle_frame(const proto::Frame& f, uint64_t now) {
    switch (f.type) {
        case proto::Pkt::LoginRequest: {
            proto::LoginRequest r;
            if (!proto::decode_login(f.payload, f.len, r)) {
                close_client("bad login");
                return;
            }
            handle_login(r, now);
            return;
        }
        case proto::Pkt::Unsequenced: {
            if (!logged_in_) {
                close_client("order before login");
                return;
            }
            proto::ClientMsg m;
            if (!proto::decode_client_msg(f.payload, f.len, m)) {
                close_client("bad order message");
                return;
            }
            if (m.type == proto::Msg::EnterOrder) handle_enter(m.enter, now);
            // Cancels: IOC orders never rest, so there is never anything to cancel.
            return;
        }
        case proto::Pkt::ClientHeartbeat:
            return;
        case proto::Pkt::Logout:
            close_client("logout");
            return;
        default:
            close_client("unexpected packet");
            return;
    }
}

void SimExchange::handle_login(const proto::LoginRequest& r, uint64_t now) {
    const uint64_t end = log_offsets_.size() + 1;
    proto::LoginAccepted a;
    std::memcpy(a.session, session_, sizeof(a.session));
    a.first_seq = std::clamp<uint64_t>(r.requested_seq, 1, end);
    a.end_seq = end;
    uint8_t buf[proto::kMaxFrame];
    send_bytes(buf, proto::encode_login_accepted(buf, a));
    for (uint64_t s = a.first_seq; s < end; ++s) {  // replay what the client missed
        const uint32_t off = log_offsets_[s - 1];
        const uint32_t stop = s < log_offsets_.size() ? log_offsets_[s] : uint32_t(log_.size());
        send_bytes(log_.data() + off, stop - off);
        ++stats_.replayed;
    }
    logged_in_ = ever_logged_in_ = true;
    ++stats_.logins;
    last_client_tx_ns_ = now;
    if (!cfg_.quiet)
        std::fprintf(stderr, "[sim] login: replaying %llu..%llu\n", (unsigned long long)a.first_seq,
                     (unsigned long long)(end - 1));
}

void SimExchange::handle_enter(const proto::EnterOrder& o, uint64_t now) {
    ++stats_.orders;
    last_order_activity_ns_ = now;
    const bool inject = cfg_.disconnect_after_orders && !fault_done_ && stats_.orders == cfg_.disconnect_after_orders;
    suppress_send_ = inject;  // responses go to the log only, then we hang up

    proto::OrderEvent e;
    e.token = o.token;
    e.ts = now;
    if (!tokens_.insert(o.token).second) {
        ++stats_.duplicate_tokens;
        e.type = proto::Msg::Rejected;
        e.reason = proto::kRejDuplicate;
        ++stats_.rejects;
        emit(e);
    } else if (o.symbol >= cfg_.symbols || o.qty == 0 || o.tif != proto::Tif::Ioc) {
        e.type = proto::Msg::Rejected;
        e.reason = o.symbol >= cfg_.symbols ? proto::kRejSymbol : o.qty == 0 ? proto::kRejQty : proto::kRejUnsupported;
        ++stats_.rejects;
        emit(e);
    } else {
        e.type = proto::Msg::Accepted;
        e.side = o.side;
        e.qty = o.qty;
        e.symbol = o.symbol;
        e.price = o.price;
        e.ref = ++order_ref_;
        emit(e);
        // Fill against the opposite top of book if the limit crosses it.
        const feed::OrderBook& b = book_.book(o.symbol);
        const feed::Quote q = o.side == feed::Side::Buy ? b.best_ask() : b.best_bid();
        const bool crosses = q.valid && (o.side == feed::Side::Buy ? o.price >= q.price : o.price <= q.price);
        uint32_t filled = 0;
        if (crosses) {
            filled = uint32_t(std::min<uint64_t>(o.qty, q.qty));
            proto::OrderEvent x;
            x.type = proto::Msg::Executed;
            x.token = o.token;
            x.ts = now;
            x.qty = filled;
            x.price = q.price;
            x.ref = ++match_id_;
            emit(x);
            ++stats_.fills;
            stats_.filled_qty += filled;
        }
        if (filled < o.qty) {
            proto::OrderEvent c;
            c.type = proto::Msg::Canceled;
            c.token = o.token;
            c.ts = now;
            c.qty = o.qty - filled;
            c.reason = proto::kReasonIoc;
            emit(c);
            ++stats_.ioc_cancels;
        }
    }
    if (inject) {
        fault_done_ = true;
        suppress_send_ = false;
        ++stats_.injected_disconnects;
        close_client("fault injection");
    }
}

void SimExchange::emit(const proto::OrderEvent& e) {
    uint8_t buf[proto::kMaxFrame];
    const size_t n = proto::encode_sequenced(buf, e);
    log_offsets_.push_back(uint32_t(log_.size()));
    log_.insert(log_.end(), buf, buf + n);
    if (!suppress_send_) send_bytes(buf, n);
}

void SimExchange::send_bytes(const uint8_t* p, size_t n) {
    if (client_ < 0) return;
    if (!tx_pending_.empty()) {
        tx_pending_.insert(tx_pending_.end(), p, p + n);
        return;
    }
    const ssize_t r = ::send(client_, p, n, kSendFlags);
    if (r == ssize_t(n)) {
        last_client_tx_ns_ = now_ns();
        return;
    }
    if (r >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        const size_t sent = r > 0 ? size_t(r) : 0;
        tx_pending_.insert(tx_pending_.end(), p + sent, p + n);
        return;
    }
    close_client("send error");
}

void SimExchange::flush_client() {
    while (client_ >= 0 && !tx_pending_.empty()) {
        const ssize_t r = ::send(client_, tx_pending_.data(), tx_pending_.size(), kSendFlags);
        if (r > 0) {
            tx_pending_.erase(tx_pending_.begin(), tx_pending_.begin() + r);
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            close_client("send error");
        }
    }
}

void SimExchange::close_client(const char* why) {
    if (client_ < 0) return;
    ::close(client_);
    client_ = -1;
    logged_in_ = false;
    rx_len_ = 0;
    tx_pending_.clear();
    // Cancel-on-disconnect would cancel resting orders here; IOC orders never rest.
    if (!cfg_.quiet) std::fprintf(stderr, "[sim] order session closed: %s\n", why);
}

}  // namespace hp
