// What lives in the shared-memory region between the engine (fast path) and
// the control process, plus the journal record format.
//
//   SharedHeader   : magic, layout hash, heartbeats, engine state, drop counter
//   control        : Seqlock<ControlBlock>  control -> engine (limits, kill flag)
//   journal        : SpscQueue<JournalRecord> engine -> control (every input and
//                    output of the decision logic, for deterministic replay)
#pragma once

#include <atomic>
#include <cstdint>

#include "shm/seqlock.h"
#include "shm/spsc_queue.h"

namespace hp {

inline constexpr uint16_t kMaxSymbols = 64;
inline constexpr size_t kJournalCapacity = 1u << 16;  // 64K records x 64 B = 4 MB

// Per-symbol pre-trade limits. Prices and notionals are in integer ticks.
struct SymbolLimits {
    uint32_t max_order_qty = 0;
    uint32_t max_position = 0;   // |position + open exposure| after this order
    uint32_t band_ticks = 0;     // max |order price - reference (mid)|
    uint32_t rate_per_sec = 0;   // token-bucket refill
    uint32_t burst = 0;          // token-bucket capacity
    uint32_t pad = 0;
    uint64_t max_notional = 0;   // qty * price
};
static_assert(sizeof(SymbolLimits) == 32);

struct ControlBlock {
    uint64_t version = 0;        // logical version, bumped by the control process
    uint32_t kill = 0;           // 1 = send nothing
    uint32_t num_symbols = 0;
    SymbolLimits limits[kMaxSymbols];
};

// One journal record = one input to, or output of, the deterministic core.
// Fixed 64 bytes so the queue slot is exactly one cache line.
enum class Rec : uint8_t {
    RunStart = 1,    // strategy parameters (see engine/journal.h for the field map)
    Market,          // a sequenced market-data message delivered to the book
    OrderSent,       // an order the core decided to send
    OrderEvent,      // exchange response: Accepted / Executed / Canceled / Rejected / Lost
    RiskReject,      // an order the risk check refused
    Gating,          // trading enabled/disabled (session, gap, control, kill)
    Limits,          // one symbol's limits for a control version
    ControlApplied,  // the engine switched to control version N
    RunEnd,          // final counters
};

struct JournalRecord {
    Rec kind = Rec::RunStart;
    uint8_t a = 0;        // side / message type / reason / flag
    uint16_t symbol = 0;
    uint32_t qty = 0;
    uint32_t price = 0;
    uint32_t b = 0;
    uint64_t id = 0;      // order id / token / version
    uint64_t seq = 0;     // feed sequence number / other
    uint64_t ts = 0;      // event timestamp (ns)
    uint64_t c = 0;
    uint64_t d = 0;
    uint64_t e = 0;
};
static_assert(sizeof(JournalRecord) == 64, "one cache line per record");

enum class EngineState : uint32_t { Starting = 0, Running = 1, Finished = 2 };

struct SharedHeader {
    uint64_t magic = 0;
    uint64_t layout_hash = 0;
    int64_t engine_pid = 0;
    alignas(64) std::atomic<uint64_t> engine_heartbeat_ns{0};   // written by engine
    alignas(64) std::atomic<uint64_t> control_heartbeat_ns{0};  // written by control
    alignas(64) std::atomic<uint64_t> journal_dropped{0};
    std::atomic<uint32_t> engine_state{0};
    std::atomic<uint64_t> engine_orders{0};                     // for the status line
    std::atomic<uint64_t> engine_market_msgs{0};
};

struct SharedLayout {
    SharedHeader hdr;
    Seqlock<ControlBlock> control;
    SpscQueue<JournalRecord, kJournalCapacity> journal;
};

inline constexpr uint64_t kMagic = 0x48505348'4d454d31ULL;  // "HPSHMEM1"

// Changes whenever the layout changes, so mismatched binaries refuse to attach.
inline constexpr uint64_t layout_hash() {
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t v : {uint64_t(sizeof(SharedLayout)), uint64_t(sizeof(SharedHeader)),
                       uint64_t(sizeof(ControlBlock)), uint64_t(sizeof(JournalRecord)),
                       uint64_t(kJournalCapacity), uint64_t(kMaxSymbols), uint64_t(2) /* format version */}) {
        h ^= v;
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace hp
