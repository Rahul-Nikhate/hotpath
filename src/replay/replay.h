// Deterministic replay: feed a journal's recorded inputs (market messages,
// order responses, gating changes, control versions) back through the same
// Core class the live engine used, and check that it makes byte-identical
// decisions (every order and every risk reject, in order).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "shm/layout.h"

namespace hp {

struct ReplayResult {
    bool ok = false;             // journal was well-formed and complete
    bool match = false;          // replayed decisions == recorded decisions
    uint64_t records = 0, market_msgs = 0;
    uint64_t live_orders = 0, replay_orders = 0, live_rejects = 0, replay_rejects = 0;
    uint64_t live_hash = 0, replay_hash = 0;
    uint64_t journal_dropped = 0;
    std::string error;           // why !ok or !match
};

std::vector<JournalRecord> read_journal(const std::string& path);
ReplayResult replay(const std::vector<JournalRecord>& recs);

}  // namespace hp
