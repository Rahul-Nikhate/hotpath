#include "replay/replay.h"

#include <cstdio>
#include <map>
#include <memory>

#include "engine/core.h"
#include "engine/journal.h"

namespace hp {
namespace {

struct Fnv {
    uint64_t h = 1469598103934665603ULL;
    void add(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ULL;
        }
    }
};

// One decision, reduced to the fields that must match.
struct Decision {
    bool is_order = false;
    uint64_t token = 0;  // 0 for rejects
    uint8_t side = 0, reason = 0;
    uint16_t symbol = 0;
    uint32_t qty = 0, price = 0;
    bool operator==(const Decision&) const = default;
    void hash_into(Fnv& f) const {
        f.add(is_order);
        f.add(token);
        f.add(side);
        f.add(reason);
        f.add(symbol);
        f.add(qty);
        f.add(price);
    }
};

struct ReplayOut {
    std::vector<Decision>* out;
    void on_order(uint64_t token, const OrderIntent& o, uint64_t) {
        out->push_back({true, token, uint8_t(o.side), 0, o.symbol, o.qty, o.price});
    }
    void on_risk_reject(RiskResult r, const OrderIntent& o, uint64_t) {
        out->push_back({false, 0, uint8_t(o.side), uint8_t(r), o.symbol, o.qty, o.price});
    }
    void on_warm(const OrderIntent&) {}
};

}  // namespace

std::vector<JournalRecord> read_journal(const std::string& path) {
    std::vector<JournalRecord> v;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return v;
    JournalRecord r;
    while (std::fread(&r, sizeof(r), 1, f) == 1) v.push_back(r);
    std::fclose(f);
    return v;
}

ReplayResult replay(const std::vector<JournalRecord>& recs) {
    ReplayResult res;
    res.records = recs.size();
    if (recs.empty() || recs.front().kind != Rec::RunStart) {
        res.error = "journal does not start with RunStart";
        return res;
    }
    const JournalRecord& start = recs.front();
    if (start.a != jr::kFormatVersion) {
        res.error = "unsupported journal format";
        return res;
    }
    auto core = std::make_unique<Core<ImbalanceTaker>>(start.symbol, size_t(start.seq), ImbalanceTaker(jr::params_from(start)));

    std::vector<Decision> live, replayed;
    std::map<uint64_t, ControlBlock> versions;  // limits by control version
    ReplayOut out{&replayed};
    NullProbe probe;
    bool ended = false;

    for (size_t i = 1; i < recs.size(); ++i) {
        const JournalRecord& r = recs[i];
        switch (r.kind) {
            case Rec::Market:
                core->on_market(jr::market_from(r), probe, out);
                ++res.market_msgs;
                break;
            case Rec::OrderEvent:
                core->on_order_event(jr::event_from(r));
                break;
            case Rec::Gating:
                core->set_enabled(r.a != 0);
                break;
            case Rec::Limits: {
                ControlBlock& cb = versions[r.id];
                cb.version = r.id;
                if (r.symbol < kMaxSymbols) cb.limits[r.symbol] = jr::limits_from(r);
                break;
            }
            case Rec::ControlApplied: {
                ControlBlock cb = versions[r.id];
                cb.version = r.id;
                cb.kill = r.a;
                cb.num_symbols = r.symbol;
                core->apply_control(cb);
                break;
            }
            case Rec::OrderSent:
                live.push_back({true, r.id, r.a, 0, r.symbol, r.qty, r.price});
                break;
            case Rec::RiskReject:
                live.push_back({false, 0, uint8_t(r.b), r.a, r.symbol, r.qty, r.price});
                break;
            case Rec::RunEnd:
                res.journal_dropped = r.c;
                ended = true;
                break;
            case Rec::RunStart:
                res.error = "second RunStart in journal";
                return res;
        }
    }

    Fnv lh, rh;
    for (const Decision& d : live) d.hash_into(lh);
    for (const Decision& d : replayed) d.hash_into(rh);
    for (const Decision& d : live) (d.is_order ? res.live_orders : res.live_rejects)++;
    for (const Decision& d : replayed) (d.is_order ? res.replay_orders : res.replay_rejects)++;
    res.live_hash = lh.h;
    res.replay_hash = rh.h;
    res.ok = ended && res.journal_dropped == 0;
    if (!ended) res.error = "journal has no RunEnd (engine crashed or still running?)";
    else if (res.journal_dropped) res.error = "journal dropped records: not replayable";
    res.match = res.ok && live == replayed;
    if (res.ok && !res.match) {
        size_t k = 0;
        while (k < live.size() && k < replayed.size() && live[k] == replayed[k]) ++k;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "first difference at decision %zu (live %zu decisions, replay %zu)", k,
                      live.size(), replayed.size());
        res.error = buf;
    }
    return res;
}

}  // namespace hp
