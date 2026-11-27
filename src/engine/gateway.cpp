#include "engine/gateway.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "net.h"  // project 1

namespace hp {
namespace {
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS: SO_NOSIGPIPE is set on the socket by net::tcp_connect
#endif
}  // namespace

Gateway::Gateway(const GatewayConfig& cfg) : cfg_(cfg), templates_(cfg.num_symbols), rx_(64 * 1024) {
    tx_pending_.reserve(64 * 1024);
}

Gateway::~Gateway() {
    if (fd_ >= 0) ::close(fd_);
}

bool Gateway::connect(uint64_t now) {
    last_attempt_ns_ = now;
    if (fd_ >= 0 || session_over_) return fd_ >= 0;
    fd_ = net::tcp_connect(cfg_.ip, cfg_.port, 1000);
    if (fd_ < 0) {
        ++stats_.connect_failures;
        return false;
    }
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL, 0) | O_NONBLOCK);
    rx_len_ = 0;
    tx_pending_.clear();
    proto::LoginRequest req;
    proto::copy_padded(req.user, sizeof(req.user), cfg_.user.c_str());
    proto::copy_padded(req.pass, sizeof(req.pass), cfg_.pass.c_str());
    req.requested_seq = next_seq_;
    uint8_t buf[proto::kMaxFrame];
    const size_t n = proto::encode_login(buf, req);
    state_ = GwState::LoggingIn;
    last_rx_ns_ = last_tx_ns_ = now;
    if (!write(buf, n)) {
        drop("login send failed");
        return false;
    }
    return true;
}

bool Gateway::send_cancel(uint64_t token, uint32_t qty) {
    if (state_ != GwState::Ready) return false;
    uint8_t buf[proto::kMaxFrame];
    const size_t n = proto::encode_cancel(buf, proto::CancelOrder{token, qty});
    const bool ok = write(buf, n);
    if (ok) ++stats_.cancels_sent;
    return ok;
}

bool Gateway::read_available(uint64_t now) {
    for (;;) {
        if (rx_len_ == rx_.size()) return true;  // parse first, then read more
        const ssize_t r = ::recv(fd_, rx_.data() + rx_len_, rx_.size() - rx_len_, 0);
        if (r > 0) {
            rx_len_ += size_t(r);
            last_rx_ns_ = now;
            continue;
        }
        if (r == 0) {
            drop("closed by peer");
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        drop("recv error");
        return false;
    }
}

bool Gateway::handle_session_frame(const proto::Frame& f, uint64_t now) {
    switch (f.type) {
        case proto::Pkt::LoginAccepted: {
            proto::LoginAccepted a;
            if (state_ != GwState::LoggingIn || !proto::decode_login_accepted(f.payload, f.len, a)) {
                ++stats_.bad_frames;
                drop("unexpected login response");
                return false;
            }
            ++stats_.logins;
            next_seq_ = a.first_seq;
            end_seq_ = a.end_seq;
            if (next_seq_ >= end_seq_) become_ready();
            else state_ = GwState::Resyncing;
            return true;
        }
        case proto::Pkt::LoginRejected:
            ++stats_.login_rejects;
            session_over_ = true;
            drop("login rejected");
            return false;
        case proto::Pkt::ServerHeartbeat:
            ++stats_.heartbeats_in;
            return true;
        case proto::Pkt::EndOfSession:
            session_over_ = true;
            drop("end of session");
            return false;
        default:
            ++stats_.bad_frames;
            (void)now;
            return true;
    }
}

void Gateway::become_ready() {
    state_ = GwState::Ready;
    resynced_ = true;
}

bool Gateway::write(const uint8_t* p, size_t n) {
    if (fd_ < 0) return false;
    if (!tx_pending_.empty()) {  // keep byte order: queue behind what is pending
        tx_pending_.insert(tx_pending_.end(), p, p + n);
        flush();
        return fd_ >= 0;
    }
    const ssize_t r = ::send(fd_, p, n, kSendFlags);
    if (r == ssize_t(n)) [[likely]] {
        sent_since_maintain_ = true;  // no clock read on the hot path
        return true;
    }
    if (r >= 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        const size_t sent = r > 0 ? size_t(r) : 0;
        ++stats_.partial_sends;
        tx_pending_.insert(tx_pending_.end(), p + sent, p + n);
        return true;
    }
    drop("send error");
    return false;
}

void Gateway::flush() {
    while (fd_ >= 0 && !tx_pending_.empty()) {
        const ssize_t r = ::send(fd_, tx_pending_.data(), tx_pending_.size(), kSendFlags);
        if (r > 0) {
            tx_pending_.erase(tx_pending_.begin(), tx_pending_.begin() + r);
            sent_since_maintain_ = true;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            drop("send error");
        }
    }
}

void Gateway::compact(size_t consumed) {
    if (consumed == 0) return;
    if (consumed < rx_len_) std::memmove(rx_.data(), rx_.data() + consumed, rx_len_ - consumed);
    rx_len_ -= consumed;
}

void Gateway::maintain(uint64_t now) {
    if (fd_ < 0) {
        if (cfg_.auto_reconnect && !session_over_ && now - last_attempt_ns_ >= cfg_.reconnect_ns) connect(now);
        return;
    }
    flush();
    if (fd_ < 0) return;
    if (sent_since_maintain_) {
        last_tx_ns_ = now;
        sent_since_maintain_ = false;
    }
    if (now - last_rx_ns_ > cfg_.timeout_ns) {
        drop("heartbeat timeout");
        return;
    }
    if (now - last_tx_ns_ >= cfg_.heartbeat_ns) {
        uint8_t buf[proto::kFrameHdr];
        proto::encode_empty(buf, proto::Pkt::ClientHeartbeat);
        if (write(buf, sizeof(buf))) ++stats_.heartbeats_out;
    }
}

void Gateway::logout() {
    if (fd_ < 0) return;
    uint8_t buf[proto::kFrameHdr];
    proto::encode_empty(buf, proto::Pkt::Logout);
    write(buf, sizeof(buf));
    flush();
    session_over_ = true;
    ::close(fd_);
    fd_ = -1;
    state_ = GwState::Disconnected;
}

void Gateway::drop(const char* why) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    if (state_ != GwState::Disconnected) {
        ++stats_.disconnects;
        std::fprintf(stderr, "[gateway] disconnected: %s (next_seq=%llu)\n", why, (unsigned long long)next_seq_);
    }
    state_ = GwState::Disconnected;
    rx_len_ = 0;
    tx_pending_.clear();
}

}  // namespace hp
