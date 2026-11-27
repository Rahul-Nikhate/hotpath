// Order-entry protocol: OUCH-style binary messages inside SoupBinTCP-style
// framing. Full byte layouts are in docs/PROTOCOL.md.
//
//   frame   = [u16 length of (type + payload)] [u8 packet type] [payload]
//   client  -> server packets: LoginRequest 'L', Unsequenced 'U' (carries an
//              order message), ClientHeartbeat 'R', Logout 'O'
//   server  -> client packets: LoginAccepted 'A', LoginRejected 'J',
//              Sequenced 'S' (carries an order response, implicitly numbered
//              1, 2, 3...), ServerHeartbeat 'H', EndOfSession 'Z'
//
// All integers are big-endian. Encoding/decoding is byte-wise (feed::be), never
// by casting structs onto buffers: no alignment, padding or aliasing hazards.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "protocol.h"  // project 1: feed::be, feed::Side

namespace hp::proto {

namespace be = feed::be;
using feed::Side;

enum class Pkt : uint8_t {
    LoginRequest = 'L',
    LoginAccepted = 'A',
    LoginRejected = 'J',
    Unsequenced = 'U',
    Sequenced = 'S',
    ClientHeartbeat = 'R',
    ServerHeartbeat = 'H',
    Logout = 'O',
    EndOfSession = 'Z',
};

enum class Msg : uint8_t {
    EnterOrder = 'O',
    CancelOrder = 'X',
    Accepted = 'A',
    Executed = 'E',
    Canceled = 'C',
    Rejected = 'J',
    Lost = 'L',  // never on the wire: the engine's local verdict for an order the
                 // exchange provably never received (see Gateway / docs/DESIGN.md)
};

enum class Tif : uint8_t { Ioc = 'I', Day = 'D' };

// Cancel / reject reason codes.
inline constexpr uint8_t kReasonIoc = 'I';         // IOC remainder
inline constexpr uint8_t kReasonUser = 'U';        // client asked
inline constexpr uint8_t kReasonDisconnect = 'D';  // cancel-on-disconnect
inline constexpr uint8_t kRejDuplicate = 'T';
inline constexpr uint8_t kRejSymbol = 'S';
inline constexpr uint8_t kRejQty = 'Q';
inline constexpr uint8_t kRejUnsupported = 'X';

// ---- sizes -----------------------------------------------------------------
inline constexpr size_t kFrameHdr = 3;          // u16 length + u8 packet type
inline constexpr size_t kEnterLen = 21;         // type1 token8 side1 qty4 sym2 price4 tif1
inline constexpr size_t kCancelLen = 13;        // type1 token8 qty4
inline constexpr size_t kAcceptedLen = 36;      // type1 ts8 token8 side1 qty4 sym2 price4 ref8
inline constexpr size_t kExecutedLen = 33;      // type1 ts8 token8 qty4 price4 match8
inline constexpr size_t kCanceledLen = 22;      // type1 ts8 token8 qty4 reason1
inline constexpr size_t kRejectedLen = 18;      // type1 ts8 token8 reason1
inline constexpr size_t kLoginReqLen = 24;      // user6 pass10 requested_seq8
inline constexpr size_t kLoginAccLen = 26;      // session10 first_seq8 end_seq8
inline constexpr size_t kEnterFrame = kFrameHdr + kEnterLen;  // 24 bytes on the wire
inline constexpr size_t kMaxFrame = 64;

// ---- decoded forms ---------------------------------------------------------
struct EnterOrder {
    uint64_t token = 0;
    Side side = Side::Buy;
    uint32_t qty = 0;
    uint16_t symbol = 0;
    uint32_t price = 0;
    Tif tif = Tif::Ioc;
};

struct CancelOrder {
    uint64_t token = 0;
    uint32_t qty = 0;  // 0 = whole order
};

// Server -> client order response (any of Accepted/Executed/Canceled/Rejected).
struct OrderEvent {
    Msg type = Msg::Accepted;
    uint64_t token = 0;
    uint64_t ts = 0;
    uint32_t qty = 0;     // Accepted: order qty; Executed: fill qty; Canceled: qty cancelled
    uint32_t price = 0;   // Accepted: limit price; Executed: fill price
    uint16_t symbol = 0;  // Accepted only
    Side side = Side::Buy;
    uint8_t reason = 0;   // Canceled / Rejected
    uint64_t ref = 0;     // Accepted: exchange order ref; Executed: match id
};

struct LoginRequest {
    char user[6] = {};
    char pass[10] = {};
    uint64_t requested_seq = 1;  // next sequenced message the client expects
};

struct LoginAccepted {
    char session[10] = {};
    uint64_t first_seq = 1;  // first message that will be (re)sent
    uint64_t end_seq = 1;    // one past the last message already in the log
};

// ---- framing ---------------------------------------------------------------
inline size_t put_frame_header(uint8_t* out, Pkt type, size_t payload) {
    be::put16(out, uint16_t(1 + payload));
    out[2] = uint8_t(type);
    return kFrameHdr;
}

struct Frame {
    Pkt type = Pkt::ServerHeartbeat;
    const uint8_t* payload = nullptr;
    size_t len = 0;
};

enum class Parse { Ok, NeedMore, Bad };

// Parses one frame from [p, p+n). On Ok, `used` is the frame's total size.
inline Parse parse_frame(const uint8_t* p, size_t n, Frame& f, size_t& used) {
    if (n < 2) return Parse::NeedMore;
    const size_t len = be::get16(p);
    if (len == 0 || len > kMaxFrame) return Parse::Bad;
    if (n < 2 + len) return Parse::NeedMore;
    f.type = Pkt(p[2]);
    f.payload = p + 3;
    f.len = len - 1;
    used = 2 + len;
    return Parse::Ok;
}

inline void copy_padded(char* dst, size_t n, const char* src) {
    std::memset(dst, ' ', n);
    for (size_t i = 0; i < n && src[i]; ++i) dst[i] = src[i];
}

// ---- client -> server ------------------------------------------------------
inline size_t encode_login(uint8_t* out, const LoginRequest& r) {
    size_t o = put_frame_header(out, Pkt::LoginRequest, kLoginReqLen);
    std::memcpy(out + o, r.user, 6);
    std::memcpy(out + o + 6, r.pass, 10);
    be::put64(out + o + 16, r.requested_seq);
    return o + kLoginReqLen;
}
inline bool decode_login(const uint8_t* p, size_t n, LoginRequest& r) {
    if (n != kLoginReqLen) return false;
    std::memcpy(r.user, p, 6);
    std::memcpy(r.pass, p + 6, 10);
    r.requested_seq = be::get64(p + 16);
    return true;
}

inline size_t encode_enter(uint8_t* out, const EnterOrder& e) {
    size_t o = put_frame_header(out, Pkt::Unsequenced, kEnterLen);
    uint8_t* p = out + o;
    p[0] = uint8_t(Msg::EnterOrder);
    be::put64(p + 1, e.token);
    p[9] = uint8_t(e.side);
    be::put32(p + 10, e.qty);
    be::put16(p + 14, e.symbol);
    be::put32(p + 16, e.price);
    p[20] = uint8_t(e.tif);
    return o + kEnterLen;
}

inline size_t encode_cancel(uint8_t* out, const CancelOrder& c) {
    size_t o = put_frame_header(out, Pkt::Unsequenced, kCancelLen);
    uint8_t* p = out + o;
    p[0] = uint8_t(Msg::CancelOrder);
    be::put64(p + 1, c.token);
    be::put32(p + 9, c.qty);
    return o + kCancelLen;
}

inline size_t encode_empty(uint8_t* out, Pkt type) { return put_frame_header(out, type, 0); }

// Client message inside an Unsequenced packet.
struct ClientMsg {
    Msg type = Msg::EnterOrder;
    EnterOrder enter;
    CancelOrder cancel;
};
inline bool decode_client_msg(const uint8_t* p, size_t n, ClientMsg& m) {
    if (n == 0) return false;
    m.type = Msg(p[0]);
    if (m.type == Msg::EnterOrder) {
        if (n != kEnterLen) return false;
        if (p[9] != uint8_t(Side::Buy) && p[9] != uint8_t(Side::Sell)) return false;
        if (p[20] != uint8_t(Tif::Ioc) && p[20] != uint8_t(Tif::Day)) return false;
        m.enter.token = be::get64(p + 1);
        m.enter.side = Side(p[9]);
        m.enter.qty = be::get32(p + 10);
        m.enter.symbol = be::get16(p + 14);
        m.enter.price = be::get32(p + 16);
        m.enter.tif = Tif(p[20]);
        return true;
    }
    if (m.type == Msg::CancelOrder) {
        if (n != kCancelLen) return false;
        m.cancel.token = be::get64(p + 1);
        m.cancel.qty = be::get32(p + 9);
        return true;
    }
    return false;
}

// ---- server -> client ------------------------------------------------------
inline size_t encode_login_accepted(uint8_t* out, const LoginAccepted& a) {
    size_t o = put_frame_header(out, Pkt::LoginAccepted, kLoginAccLen);
    std::memcpy(out + o, a.session, 10);
    be::put64(out + o + 10, a.first_seq);
    be::put64(out + o + 18, a.end_seq);
    return o + kLoginAccLen;
}
inline bool decode_login_accepted(const uint8_t* p, size_t n, LoginAccepted& a) {
    if (n != kLoginAccLen) return false;
    std::memcpy(a.session, p, 10);
    a.first_seq = be::get64(p + 10);
    a.end_seq = be::get64(p + 18);
    return true;
}

// Writes just the order-response body (no frame header); returns its length.
inline size_t encode_event_body(uint8_t* p, const OrderEvent& e) {
    p[0] = uint8_t(e.type);
    be::put64(p + 1, e.ts);
    be::put64(p + 9, e.token);
    switch (e.type) {
        case Msg::Accepted:
            p[17] = uint8_t(e.side);
            be::put32(p + 18, e.qty);
            be::put16(p + 22, e.symbol);
            be::put32(p + 24, e.price);
            be::put64(p + 28, e.ref);
            return kAcceptedLen;
        case Msg::Executed:
            be::put32(p + 17, e.qty);
            be::put32(p + 21, e.price);
            be::put64(p + 25, e.ref);
            return kExecutedLen;
        case Msg::Canceled:
            be::put32(p + 17, e.qty);
            p[21] = e.reason;
            return kCanceledLen;
        case Msg::Rejected:
            p[17] = e.reason;
            return kRejectedLen;
        default:
            return 0;
    }
}

inline size_t encode_sequenced(uint8_t* out, const OrderEvent& e) {
    const size_t body = encode_event_body(out + kFrameHdr, e);
    if (body == 0) return 0;
    put_frame_header(out, Pkt::Sequenced, body);
    return kFrameHdr + body;
}

inline bool decode_event(const uint8_t* p, size_t n, OrderEvent& e) {
    if (n < 17) return false;
    e = OrderEvent{};
    e.type = Msg(p[0]);
    e.ts = be::get64(p + 1);
    e.token = be::get64(p + 9);
    switch (e.type) {
        case Msg::Accepted:
            if (n != kAcceptedLen) return false;
            if (p[17] != uint8_t(Side::Buy) && p[17] != uint8_t(Side::Sell)) return false;
            e.side = Side(p[17]);
            e.qty = be::get32(p + 18);
            e.symbol = be::get16(p + 22);
            e.price = be::get32(p + 24);
            e.ref = be::get64(p + 28);
            return true;
        case Msg::Executed:
            if (n != kExecutedLen) return false;
            e.qty = be::get32(p + 17);
            e.price = be::get32(p + 21);
            e.ref = be::get64(p + 25);
            return true;
        case Msg::Canceled:
            if (n != kCanceledLen) return false;
            e.qty = be::get32(p + 17);
            e.reason = p[21];
            return true;
        case Msg::Rejected:
            if (n != kRejectedLen) return false;
            e.reason = p[17];
            return true;
        default:
            return false;
    }
}

}  // namespace hp::proto
