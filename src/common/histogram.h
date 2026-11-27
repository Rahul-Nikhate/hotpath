// Log-linear latency histogram (HdrHistogram-style), fixed size, no allocation.
//
// Values below 32 get one bucket each; above that every power of two is split
// into 32 sub-buckets, so any recorded value is reported within ~3%. Recording
// is a count-leading-zeros, a shift and an increment: cheap enough for the hot
// path. Percentiles report the upper edge of the bucket (conservative).
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hp {

class Histogram {
public:
    static constexpr int kSubBits = 5;
    static constexpr uint64_t kSub = 1u << kSubBits;
    static constexpr size_t kBuckets = (64 - kSubBits + 1) * kSub;

    void record(uint64_t v) {
        ++counts_[index(v)];
        ++total_;
        sum_ += v;
        if (v < min_) min_ = v;
        if (v > max_) max_ = v;
    }

    void merge(const Histogram& o) {
        for (size_t i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        min_ = std::min(min_, o.min_);
        max_ = std::max(max_, o.max_);
    }

    void reset() { *this = Histogram{}; }

    uint64_t count() const { return total_; }
    uint64_t min() const { return total_ ? min_ : 0; }
    uint64_t max() const { return max_; }
    double mean() const { return total_ ? double(sum_) / double(total_) : 0.0; }

    // q in [0, 1]; nearest-rank on bucket counts.
    uint64_t percentile(double q) const {
        if (total_ == 0) return 0;
        uint64_t rank = uint64_t(q * double(total_) + 0.999999);
        rank = std::clamp<uint64_t>(rank, 1, total_);
        uint64_t seen = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            seen += counts_[i];
            if (seen >= rank) return std::min(upper(i), max_);
        }
        return max_;
    }

    static size_t index(uint64_t v) {
        if (v < kSub) return size_t(v);
        const int msb = 63 - std::countl_zero(v);
        const int shift = msb - kSubBits;
        const uint64_t sub = (v >> shift) & (kSub - 1);
        return (size_t(shift + 1) << kSubBits) + size_t(sub);
    }
    static uint64_t upper(size_t i) {
        if (i < kSub) return i;
        const int shift = int(i >> kSubBits) - 1;
        const uint64_t sub = i & (kSub - 1);
        return ((kSub + sub) << shift) + ((uint64_t(1) << shift) - 1);
    }

    // One line in nanoseconds: "label n=… p50 … p99 … p99.9 … max …".
    void print(const char* label, FILE* out = stdout) const {
        std::fprintf(out,
                     "%-26s n=%-9llu p50 %7llu  p90 %7llu  p99 %7llu  p99.9 %8llu  max %9llu ns\n",
                     label, (unsigned long long)total_, (unsigned long long)percentile(0.50),
                     (unsigned long long)percentile(0.90), (unsigned long long)percentile(0.99),
                     (unsigned long long)percentile(0.999), (unsigned long long)max());
    }

    std::string csv_row(const std::string& label) const {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s,%llu,%llu,%llu,%llu,%llu,%llu,%.1f", label.c_str(),
                      (unsigned long long)total_, (unsigned long long)percentile(0.50),
                      (unsigned long long)percentile(0.90), (unsigned long long)percentile(0.99),
                      (unsigned long long)percentile(0.999), (unsigned long long)max(), mean());
        return buf;
    }
    static const char* csv_header() { return "metric,count,p50_ns,p90_ns,p99_ns,p999_ns,max_ns,mean_ns"; }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t total_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = UINT64_MAX;
    uint64_t max_ = 0;
};

}  // namespace hp
