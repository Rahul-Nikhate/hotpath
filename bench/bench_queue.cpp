// Experiment 1b: what each queue design decision buys, one change at a time,
// and what a single hop between cores costs (threads vs processes).
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#include "bench/bench.h"
#include "common/clock.h"
#include "shm/region.h"
#include "shm/spsc_queue.h"

using namespace hp;

namespace {

constexpr size_t kCap = 4096;

// Baseline: a ring protected by a mutex.
class MutexQueue {
public:
    bool try_push(uint64_t v) {
        std::lock_guard<std::mutex> g(m_);
        if (head_ - tail_ == kCap) return false;
        buf_[head_++ & (kCap - 1)] = v;
        return true;
    }
    bool try_pop(uint64_t& v) {
        std::lock_guard<std::mutex> g(m_);
        if (head_ == tail_) return false;
        v = buf_[tail_++ & (kCap - 1)];
        return true;
    }

private:
    std::mutex m_;
    uint64_t head_ = 0, tail_ = 0;
    std::array<uint64_t, kCap> buf_{};
};

template <class Q>
double throughput_mops(const Args& a, uint64_t n) {
    auto q = std::make_unique<Q>();
    std::atomic<bool> go{false};
    std::thread prod([&] {
        bench::pin(a, 1);
        while (!go.load(std::memory_order_acquire)) {
        }
        for (uint64_t i = 1; i <= n; ++i)
            while (!q->try_push(i)) cpu_relax();
    });
    bench::pin(a, 0);
    const uint64_t t0 = now_ns();
    go.store(true, std::memory_order_release);
    uint64_t v = 0, got = 0;
    while (got < n)
        if (q->try_pop(v)) ++got;
    const uint64_t t1 = now_ns();
    prod.join();
    return double(n) / (double(t1 - t0) * 1e-3);  // million ops / s
}

// Ping-pong: A pushes to q1, B echoes into q2, A measures the round trip.
template <class Q>
Histogram round_trip(const Args& a, Q& q1, Q& q2, uint64_t iters) {
    Histogram h;
    uint64_t v = 0;
    for (uint64_t i = 0; i < iters; ++i) {
        const uint64_t t0 = cycles();
        while (!q1.try_push(i)) cpu_relax();
        while (!q2.try_pop(v)) cpu_relax();
        h.record(cycles_to_ns(cycles() - t0));
    }
    (void)a;
    return h;
}

template <class Q>
void echo(Q& q1, Q& q2, uint64_t iters) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < iters; ++i) {
        while (!q1.try_pop(v)) cpu_relax();
        while (!q2.try_push(v)) cpu_relax();
    }
}

}  // namespace

namespace bench {

int run_queue(const Args& a) {
    const uint64_t n = a.u64("items", 20'000'000);
    using SeqCst = SpscQueue<uint64_t, kCap, SpscOptions{.padded = false, .cached = false, .seq_cst = true}>;
    using AcqRel = SpscQueue<uint64_t, kCap, SpscOptions{.padded = false, .cached = false, .seq_cst = false}>;
    using Padded = SpscQueue<uint64_t, kCap, SpscOptions{.padded = true, .cached = false, .seq_cst = false}>;
    using Cached = SpscQueue<uint64_t, kCap, SpscOptions{.padded = true, .cached = true, .seq_cst = false}>;

    std::printf("\n[queue] throughput, %llu items, one producer + one consumer thread\n", (unsigned long long)n);
    Csv csv(a, "queue_throughput", "variant,mops");
    struct Row {
        const char* name;
        double mops;
    } rows[] = {
        {"mutex + ring", throughput_mops<MutexQueue>(a, n / 4)},
        {"spsc seq_cst, unpadded", throughput_mops<SeqCst>(a, n)},
        {"spsc acquire/release", throughput_mops<AcqRel>(a, n)},
        {"  + alignas(64) padding", throughput_mops<Padded>(a, n)},
        {"  + cached indices (final)", throughput_mops<Cached>(a, n)},
    };
    for (const Row& r : rows) {
        std::printf("  %-28s %8.1f M items/s\n", r.name, r.mops);
        csv.row("%s,%.2f\n", r.name, r.mops);
    }

    const uint64_t iters = a.u64("rtt-iters", 1'000'000);
    std::printf("\n[queue] one-way hop = round trip / 2 (%llu ping-pongs)\n", (unsigned long long)iters);
    Csv lat(a, "queue_hop", Histogram::csv_header());
    {   // threads in one process
        auto q1 = std::make_unique<Cached>(), q2 = std::make_unique<Cached>();
        std::thread b([&] {
            bench::pin(a, 1);
            echo(*q1, *q2, iters);
        });
        bench::pin(a, 0);
        const Histogram h = round_trip(a, *q1, *q2, iters);
        b.join();
        h.print("round trip, threads");
        lat.row("%s\n", h.csv_row("rtt_threads").c_str());
    }
    {   // two processes over shared memory
        struct Pair {
            Cached q1, q2;
        };
        const std::string name = "bq" + std::to_string(::getpid());
        Region r = Region::create(name, sizeof(Pair), false, true);
        auto* p = new (r.data()) Pair();
        const pid_t child = ::fork();
        if (child == 0) {
            bench::pin(a, 1);
            Region c = Region::attach(name, false, 1000);
            auto* cp = static_cast<Pair*>(c.data());
            echo(cp->q1, cp->q2, iters);
            ::_exit(0);
        }
        bench::pin(a, 0);
        const Histogram h = round_trip(a, p->q1, p->q2, iters);
        ::waitpid(child, nullptr, 0);
        h.print("round trip, processes");
        lat.row("%s\n", h.csv_row("rtt_processes").c_str());
    }
    return 0;
}

}  // namespace bench
