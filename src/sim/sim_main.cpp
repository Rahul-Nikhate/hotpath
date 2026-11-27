// hp_sim: exchange simulator (market data + recovery + order entry).
#include <atomic>
#include <csignal>
#include <cstdio>

#include "common/args.h"
#include "net.h"  // project 1
#include "sim/sim_exchange.h"

using namespace hp;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }
}  // namespace

int main(int argc, char** argv) {
    Args a(argc, argv);
    if (a.has("help")) {
        std::puts(
            "hp_sim [options]\n"
            "  --udp-ip IP --udp-port P [--multicast --iface IP]  feed destination (default 127.0.0.1:9999)\n"
            "  --recovery-port P   retransmission server (default 10000)\n"
            "  --order-port P      order entry (default 10100)\n"
            "  --count N           messages to publish (default 1000000)\n"
            "  --rate R            messages per second, 0 = max (default 100000)\n"
            "  --batch B           messages per datagram (default 8)\n"
            "  --drop P            simulated datagram loss probability (default 0)\n"
            "  --symbols N --seed S\n"
            "  --disconnect-after N  drop the order session once after N orders (fault injection)\n"
            "  --linger S          keep order entry open S seconds after the feed ends (default 1)\n");
        return 0;
    }
    net::ignore_sigpipe();
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    SimConfig c;
    c.feed_ip = a.str("udp-ip", "127.0.0.1");
    c.feed_port = uint16_t(a.u64("udp-port", 9999));
    c.multicast = a.has("multicast");
    c.iface = a.str("iface", "127.0.0.1");
    c.recovery_port = uint16_t(a.u64("recovery-port", 10000));
    c.order_port = uint16_t(a.u64("order-port", 10100));
    c.count = a.u64("count", c.count);
    c.rate = a.u64("rate", c.rate);
    c.batch = size_t(a.u64("batch", c.batch));
    c.drop_rate = a.f64("drop", 0);
    c.symbols = uint16_t(a.u64("symbols", 8));
    c.seed = a.u64("seed", 42);
    c.disconnect_after_orders = a.u64("disconnect-after", 0);
    c.linger_s = a.f64("linger", 1.0);
    c.quiet = a.has("quiet");

    SimExchange sim(c);
    std::fprintf(stderr, "[sim] feed -> %s:%u | recovery :%u | orders :%u | %llu msgs @ %llu/s\n",
                 c.feed_ip.c_str(), c.feed_port, sim.recovery_port(), sim.order_port(),
                 (unsigned long long)c.count, (unsigned long long)c.rate);
    sim.run(&g_stop);

    const SimStats& s = sim.stats();
    std::printf("\n== hp_sim ==\nmessages %llu datagrams %llu dropped(sim) %llu send_errors %llu | retrans requests %llu msgs %llu\n",
                (unsigned long long)s.messages, (unsigned long long)s.datagrams, (unsigned long long)s.dropped_sim,
                (unsigned long long)s.send_errors, (unsigned long long)s.retrans_requests,
                (unsigned long long)s.retrans_messages);
    std::printf("orders %llu fills %llu (qty %llu) ioc_cancels %llu rejects %llu duplicate_tokens %llu | logins %llu replayed %llu injected_disconnects %llu\n",
                (unsigned long long)s.orders, (unsigned long long)s.fills, (unsigned long long)s.filled_qty,
                (unsigned long long)s.ioc_cancels, (unsigned long long)s.rejects,
                (unsigned long long)s.duplicate_tokens, (unsigned long long)s.logins, (unsigned long long)s.replayed,
                (unsigned long long)s.injected_disconnects);
    std::printf("reference book hash %016llx\n", (unsigned long long)sim.reference_hash());
    return s.duplicate_tokens == 0 ? 0 : 1;
}
