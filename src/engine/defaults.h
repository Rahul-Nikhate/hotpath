// Default risk limits, overridable from the command line (control process) or
// set directly in tests and benchmarks.
#pragma once

#include "common/args.h"
#include "shm/layout.h"

namespace hp {

inline ControlBlock default_control(uint16_t num_symbols, const Args* a = nullptr) {
    ControlBlock cb;
    cb.version = 1;
    cb.kill = 0;
    cb.num_symbols = num_symbols;
    SymbolLimits l;
    l.max_order_qty = 1000;
    l.max_position = 5000;
    l.band_ticks = 50;
    l.rate_per_sec = 2000;
    l.burst = 200;
    l.max_notional = 100'000'000;
    if (a) {
        l.max_order_qty = uint32_t(a->u64("max-qty", l.max_order_qty));
        l.max_position = uint32_t(a->u64("max-pos", l.max_position));
        l.band_ticks = uint32_t(a->u64("band", l.band_ticks));
        l.rate_per_sec = uint32_t(a->u64("rate-limit", l.rate_per_sec));
        l.burst = uint32_t(a->u64("burst", l.burst));
        l.max_notional = a->u64("max-notional", l.max_notional);
    }
    for (uint16_t s = 0; s < kMaxSymbols; ++s) cb.limits[s] = l;
    return cb;
}

}  // namespace hp
