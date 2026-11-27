// Pre-encoded order messages.
//
// Everything about an Enter Order frame except token, quantity and price is
// known at startup (length, packet type, message type, side, symbol, TIF). So we
// build one complete 24-byte frame per (symbol, side) once, and sending an order
// patches 16 bytes in place instead of encoding all fields. The template for a
// hot symbol stays in L1, and the send path has no branches on message layout.
#pragma once

#include <array>
#include <cstdint>

#include "proto/order_entry.h"
#include "shm/layout.h"

namespace hp::proto {

class OrderTemplates {
public:
    // Frame byte offsets of the patched fields.
    static constexpr size_t kTokenOff = kFrameHdr + 1;
    static constexpr size_t kQtyOff = kFrameHdr + 10;
    static constexpr size_t kPriceOff = kFrameHdr + 16;

    explicit OrderTemplates(uint16_t num_symbols = kMaxSymbols) {
        for (uint16_t s = 0; s < kMaxSymbols && s < num_symbols; ++s)
            for (Side side : {Side::Buy, Side::Sell}) {
                EnterOrder e;
                e.side = side;
                e.symbol = s;
                e.tif = Tif::Ioc;
                encode_enter(frame(s, side).data(), e);
            }
    }

    // Returns a ready-to-send frame of kEnterFrame bytes.
    const uint8_t* patch(uint16_t symbol, Side side, uint64_t token, uint32_t qty, uint32_t price) {
        uint8_t* f = frame(symbol, side).data();
        be::put64(f + kTokenOff, token);
        be::put32(f + kQtyOff, qty);
        be::put32(f + kPriceOff, price);
        return f;
    }

private:
    using FrameBuf = std::array<uint8_t, kEnterFrame>;
    FrameBuf& frame(uint16_t symbol, Side side) {
        return frames_[size_t(symbol) * 2 + (side == Side::Sell ? 1 : 0)];
    }
    alignas(64) std::array<FrameBuf, size_t(kMaxSymbols) * 2> frames_{};
};

}  // namespace hp::proto
