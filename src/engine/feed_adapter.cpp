#include "engine/feed_adapter.h"

#include <unistd.h>

#include "net.h"  // project 1

namespace hp {

FeedAdapter::FeedAdapter(const FeedConfig& cfg) : cfg_(cfg), seq_(cfg.max_pending), buf_(2048) {
    fd_ = net::udp_receiver(cfg_.udp_ip, cfg_.udp_port, cfg_.multicast, cfg_.iface, cfg_.rcvbuf,
                            stats_.rcvbuf_actual);
    port_ = net::local_port(fd_);
}

FeedAdapter::~FeedAdapter() {
    recovery_.stop();
    ::close(fd_);
}

void FeedAdapter::start() { recovery_.start(cfg_.recovery_ip, cfg_.recovery_port); }

}  // namespace hp
