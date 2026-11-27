// SPSC queue, seqlock and shared-memory region.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "shm/layout.h"
#include "shm/region.h"
#include "shm/seqlock.h"
#include "shm/spsc_queue.h"
#include "test.h"

using namespace hp;

TEST(spsc_basic_fifo_full_empty_wrap) {
    auto q = std::make_unique<SpscQueue<uint64_t, 8>>();
    uint64_t v = 0;
    CHECK(!q->try_pop(v));
    for (uint64_t round = 0; round < 5; ++round) {  // wraps the ring several times
        for (uint64_t i = 0; i < 8; ++i) CHECK(q->try_push(round * 100 + i));
        CHECK(!q->try_push(999));  // full: exactly N items, no wasted slot
        CHECK_EQ(q->size_approx(), 8u);
        for (uint64_t i = 0; i < 8; ++i) {
            CHECK(q->try_pop(v));
            CHECK_EQ(v, round * 100 + i);
        }
        CHECK(!q->try_pop(v));
    }
}

template <SpscOptions O>
static void stress_two_threads(uint64_t n) {
    auto q = std::make_unique<SpscQueue<uint64_t, 1024, O>>();
    std::thread producer([&] {
        for (uint64_t i = 1; i <= n; ++i)
            while (!q->try_push(i)) cpu_relax();
    });
    uint64_t expect = 1, v = 0;
    bool in_order = true;
    while (expect <= n) {
        if (q->try_pop(v)) {
            in_order &= v == expect;
            ++expect;
        }
    }
    producer.join();
    CHECK(in_order);
    CHECK(!q->try_pop(v));
}

TEST(spsc_two_threads_every_variant_in_order) {
    const uint64_t n = HP_TSAN ? 100'000 : 2'000'000;
    stress_two_threads<SpscOptions{}>(n);
    stress_two_threads<SpscOptions{.padded = false, .cached = false, .seq_cst = false}>(n);
    stress_two_threads<SpscOptions{.padded = true, .cached = false, .seq_cst = true}>(n);
}

struct Snapshot {  // seqlock payload: every field must always match
    uint64_t a, b, c, d, e, f, g;
};

TEST(seqlock_readers_never_see_torn_data) {
    auto sl = std::make_unique<Seqlock<Snapshot>>();
    sl->store(Snapshot{0, 0, 0, 0, 0, 0, 0});
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> torn{0}, reads{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r)
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                Snapshot s;
                if (sl->try_load(s)) {
                    reads.fetch_add(1, std::memory_order_relaxed);
                    if (s.a != s.b || s.b != s.c || s.c != s.d || s.d != s.e || s.e != s.f || s.f != s.g)
                        torn.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    const uint64_t writes = HP_TSAN ? 20'000 : 500'000;
    for (uint64_t k = 1; k <= writes; ++k) sl->store(Snapshot{k, k, k, k, k, k, k});
    stop = true;
    for (auto& t : readers) t.join();
    CHECK_EQ(torn.load(), 0u);
    CHECK(reads.load() > 0);
    CHECK_EQ(sl->load().a, writes);
}

TEST(region_create_attach_and_unlink) {
    const std::string name = "t" + std::to_string(::getpid());
    {
        Region r = Region::create(name, 1 << 20, false, true);
        CHECK(r.size() >= (1u << 20));
        CHECK(r.info().prefaulted);
        static_cast<uint64_t*>(r.data())[100] = 0xC0FFEE;
        Region view = Region::attach(name, false, 1000);  // second mapping, same pages
        CHECK_EQ(static_cast<uint64_t*>(view.data())[100], 0xC0FFEEu);
        static_cast<uint64_t*>(view.data())[200] = 7;
        CHECK_EQ(static_cast<uint64_t*>(r.data())[200], 7u);
    }
    bool threw = false;  // creator unlinked the name
    try {
        Region::attach(name, false, 50);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// The real cross-process case: a forked child attaches by name and produces
// into the queue that lives in shared memory; the parent consumes.
TEST(spsc_across_processes_in_shared_memory) {
    if (HP_TSAN) return;  // TSan does not support fork() with running threads well
    using Q = SpscQueue<uint64_t, 4096>;
    const std::string name = "x" + std::to_string(::getpid());
    Region r = Region::create(name, sizeof(Q), false, true);
    auto* q = new (r.data()) Q();
    const uint64_t n = 200'000;
    const pid_t child = ::fork();
    if (child == 0) {
        Region c = Region::attach(name, false, 1000);
        auto* cq = static_cast<Q*>(c.data());
        for (uint64_t i = 1; i <= n; ++i)
            while (!cq->try_push(i)) cpu_relax();
        ::_exit(0);
    }
    uint64_t expect = 1, v = 0;
    bool in_order = true;
    while (expect <= n)
        if (q->try_pop(v)) in_order &= v == expect++;
    int status = 0;
    ::waitpid(child, &status, 0);
    CHECK(in_order);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

TEST(layout_fits_and_hash_is_stable) {
    CHECK_EQ(sizeof(JournalRecord), 64u);
    CHECK(layout_hash() == layout_hash());
    CHECK(sizeof(SharedLayout) < 8u << 20);
}
