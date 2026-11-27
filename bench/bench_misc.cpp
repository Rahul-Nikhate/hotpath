// Smaller experiments: page faults and page size (exp. 2), template encoding,
// and dispatch cost (stretch exp. 6).
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "bench/bench.h"
#include "common/clock.h"
#include "engine/strategy.h"
#include "feed_generator.h"  // project 1
#include "proto/order_entry.h"
#include "proto/order_template.h"
#include "shm/region.h"

using namespace hp;

namespace {

long minor_faults() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_minflt;
}

template <class T>
inline void do_not_optimize(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// ------------------------------------------------------------- pages
// Writes 64-byte records sequentially into a region (like the journal) and
// then reads it randomly (TLB pressure), timing both and counting faults.
struct PageResult {
    double first_pass_ns, random_read_ns;
    long faults;
};

PageResult touch_region(void* base, size_t bytes, uint64_t seed) {
    auto* p = static_cast<uint8_t*>(base);
    const long f0 = minor_faults();
    uint64_t t0 = now_ns();
    for (size_t off = 0; off + 64 <= bytes; off += 64) std::memset(p + off, int(off), 64);
    const double first = double(now_ns() - t0) / double(bytes / 64);
    const long faults = minor_faults() - f0;
    feed::SplitMix64 rng(seed);
    const size_t n = 4'000'000;
    uint64_t sum = 0;
    t0 = now_ns();
    for (size_t i = 0; i < n; ++i) sum += p[(rng.next() % (bytes / 64)) * 64];
    do_not_optimize(sum);
    return {first, double(now_ns() - t0) / double(n), faults};
}

// ------------------------------------------------------------- dispatch
struct IStrategy {
    virtual ~IStrategy() = default;
    virtual std::optional<OrderIntent> on_book(uint16_t, const feed::OrderBook&, int64_t) = 0;
};
struct VirtualTaker final : IStrategy {
    ImbalanceTaker s;
    explicit VirtualTaker(const TakerParams& p) : s(p) {}
    std::optional<OrderIntent> on_book(uint16_t sym, const feed::OrderBook& b, int64_t pos) override {
        return s.peek(sym, b, pos);
    }
};
struct OtherStrategy {  // a second type, so dispatch has something to choose between
    std::optional<OrderIntent> peek(uint16_t sym, const feed::OrderBook& b, int64_t) const {
        const feed::Quote q = b.best_bid();
        if (q.valid && (q.qty & 1)) return OrderIntent{sym, Side::Sell, q.price, 1};
        return std::nullopt;
    }
};
struct VirtualOther final : IStrategy {
    OtherStrategy s;
    std::optional<OrderIntent> on_book(uint16_t sym, const feed::OrderBook& b, int64_t pos) override {
        return s.peek(sym, b, pos);
    }
};

}  // namespace

namespace bench {

int run_pages(const Args& a) {
    const size_t bytes = size_t(a.u64("region-mb", 256)) << 20;
    std::printf("\n[pages] %zu MB region: sequential 64 B writes (first touch), then random 1-byte reads\n",
                bytes >> 20);
    Csv csv(a, "pages", "variant,first_touch_ns_per_record,random_read_ns,minor_faults");
    auto report = [&](const char* name, const PageResult& r) {
        std::printf("  %-26s first touch %6.1f ns/record  random read %6.1f ns  minor faults %8ld\n", name,
                    r.first_pass_ns, r.random_read_ns, r.faults);
        csv.row("%s,%.2f,%.2f,%ld\n", name, r.first_pass_ns, r.random_read_ns, r.faults);
    };
    const std::string base = "pg" + std::to_string(::getpid());
    {
        Region r = Region::create(base + "a", bytes, false, /*prefault=*/false);
        report("4 KB pages, lazy", touch_region(r.data(), r.size(), 1));
    }
    {
        Region r = Region::create(base + "b", bytes, false, /*prefault=*/true);
        report("4 KB pages, pre-faulted", touch_region(r.data(), r.size(), 1));
    }
#ifdef __linux__
    try {
        Region r = Region::create(base + "c", bytes, /*huge=*/true, true);
        report("2 MB huge pages", touch_region(r.data(), r.size(), 1));
    } catch (const std::exception& e) {
        std::printf("  2 MB huge pages: skipped (%s)\n    reserve some: sudo sysctl vm.nr_hugepages=%zu\n", e.what(),
                    (bytes >> 21) + 8);
    }
#else
    std::printf("  2 MB huge pages: Linux only (run on Linux for the TLB comparison)\n");
#endif
    return 0;
}

int run_encode(const Args& a) {
    const uint64_t n = a.u64("encode-iters", 50'000'000);
    proto::OrderTemplates tpl(8);
    uint8_t buf[proto::kMaxFrame];
    uint64_t sink = 0;
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < n; ++i) {
        const uint8_t* f = tpl.patch(uint16_t(i & 7), (i & 8) ? Side::Sell : Side::Buy, i, uint32_t(i), uint32_t(i >> 3));
        sink += f[5];
        do_not_optimize(f);
    }
    const double patch_ns = double(now_ns() - t0) / double(n);
    t0 = now_ns();
    for (uint64_t i = 0; i < n; ++i) {
        proto::EnterOrder e{i, (i & 8) ? Side::Sell : Side::Buy, uint32_t(i), uint16_t(i & 7), uint32_t(i >> 3),
                            proto::Tif::Ioc};
        proto::encode_enter(buf, e);
        sink += buf[5];
        do_not_optimize(buf);
    }
    const double full_ns = double(now_ns() - t0) / double(n);
    do_not_optimize(sink);
    std::printf("\n[encode] %llu orders\n  patch pre-encoded template %6.2f ns/order\n  encode every field         %6.2f ns/order\n",
                (unsigned long long)n, patch_ns, full_ns);
    Csv csv(a, "encode", "variant,ns_per_order");
    csv.row("template,%.3f\nfull,%.3f\n", patch_ns, full_ns);
    return 0;
}

int run_dispatch(const Args& a) {
    const uint64_t n = a.u64("dispatch-iters", 20'000'000);
    feed::OrderBook book;
    book.add(1, Side::Buy, 100, 900);
    book.add(2, Side::Sell, 101, 100);
    TakerParams p;
    ImbalanceTaker direct(p);
    auto virt = std::make_unique<VirtualTaker>(p);
    IStrategy* vptr = virt.get();
    std::variant<ImbalanceTaker, OtherStrategy> var{direct};

    // Mixed: two strategy types in random order (indirect branch hard to predict).
    feed::SplitMix64 rng(3);
    std::vector<std::unique_ptr<IStrategy>> vmix;
    std::vector<std::variant<ImbalanceTaker, OtherStrategy>> varmix;
    for (int i = 0; i < 1024; ++i) {
        if (rng.below(2)) {
            vmix.push_back(std::make_unique<VirtualTaker>(p));
            varmix.emplace_back(ImbalanceTaker(p));
        } else {
            vmix.push_back(std::make_unique<VirtualOther>());
            varmix.emplace_back(OtherStrategy{});
        }
    }
    auto time = [&](auto&& f) {
        uint64_t hits = 0;
        const uint64_t t0 = now_ns();
        for (uint64_t i = 0; i < n; ++i) hits += f(i).has_value();
        do_not_optimize(hits);
        return double(now_ns() - t0) / double(n);
    };
    const double d = time([&](uint64_t i) { return direct.peek(uint16_t(i & 7), book, 0); });
    const double v = time([&](uint64_t i) { return vptr->on_book(uint16_t(i & 7), book, 0); });
    const double w = time([&](uint64_t i) {
        return std::visit([&](const auto& s) { return s.peek(uint16_t(i & 7), book, 0); }, var);
    });
    const double vm = time([&](uint64_t i) { return vmix[i & 1023]->on_book(uint16_t(i & 7), book, 0); });
    const double wm = time([&](uint64_t i) {
        return std::visit([&](const auto& s) { return s.peek(uint16_t(i & 7), book, 0); }, varmix[i & 1023]);
    });
    std::printf("\n[dispatch] %llu calls of the strategy decision\n", (unsigned long long)n);
    std::printf("  direct / template (inlined)  %6.2f ns\n", d);
    std::printf("  virtual, one type            %6.2f ns  (predictable indirect branch)\n", v);
    std::printf("  std::variant, one type       %6.2f ns\n", w);
    std::printf("  virtual, 2 types mixed       %6.2f ns  (unpredictable indirect branch)\n", vm);
    std::printf("  std::variant, 2 types mixed  %6.2f ns\n", wm);
    Csv csv(a, "dispatch", "variant,ns_per_call");
    csv.row("direct,%.3f\nvirtual,%.3f\nvariant,%.3f\nvirtual_mixed,%.3f\nvariant_mixed,%.3f\n", d, v, w, vm, wm);
    return 0;
}

}  // namespace bench
