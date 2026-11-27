#pragma once

#include <cstdint>

#include "protocol.h"  // project 1: feed::Side, feed::Message

namespace hp {

using feed::Side;

// What the strategy wants to do. Always an IOC at a price already in the book.
struct OrderIntent {
    uint16_t symbol = 0;
    Side side = Side::Buy;
    uint32_t price = 0;  // integer ticks
    uint32_t qty = 0;
};

// Points on the hot path where the fast path takes a timestamp.
enum class Stage : uint8_t { Rx = 0, Delivered, Book, Decision, Risk, Encoded, Sent, Count };

// Probe used by code that doesn't time anything (replay, unit tests).
struct NullProbe {
    void mark(Stage) {}
};

}  // namespace hp
