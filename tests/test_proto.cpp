// Order-entry codec, framing, pre-encoded templates, and decoder fuzzing.
#include <cstring>
#include <vector>

#include "feed_generator.h"  // project 1: SplitMix64
#include "proto/order_entry.h"
#include "proto/order_template.h"
#include "test.h"

using namespace hp;
using namespace hp::proto;

TEST(proto_enter_roundtrip_and_frame_size) {
    EnterOrder e{0x0102030405060708ULL, Side::Sell, 1500, 7, 10045, Tif::Ioc};
    uint8_t buf[kMaxFrame];
    const size_t n = encode_enter(buf, e);
    CHECK_EQ(n, kEnterFrame);
    CHECK_EQ(n, 24u);
    Frame f;
    size_t used = 0;
    CHECK(parse_frame(buf, n, f, used) == Parse::Ok);
    CHECK_EQ(used, n);
    CHECK(f.type == Pkt::Unsequenced);
    ClientMsg m;
    CHECK(decode_client_msg(f.payload, f.len, m));
    CHECK(m.type == Msg::EnterOrder);
    CHECK_EQ(m.enter.token, e.token);
    CHECK(m.enter.side == Side::Sell);
    CHECK_EQ(m.enter.qty, 1500u);
    CHECK_EQ(m.enter.symbol, 7u);
    CHECK_EQ(m.enter.price, 10045u);
    CHECK(buf[3 + 1] == 0x01 && buf[3 + 8] == 0x08);  // big-endian on the wire
}

TEST(proto_every_server_event_roundtrips) {
    OrderEvent evs[4];
    evs[0] = {Msg::Accepted, 11, 1234, 300, 10010, 3, Side::Buy, 0, 77};
    evs[1] = {Msg::Executed, 11, 1235, 100, 10011, 0, Side::Buy, 0, 88};
    evs[2] = {Msg::Canceled, 11, 1236, 200, 0, 0, Side::Buy, kReasonIoc, 0};
    evs[3] = {Msg::Rejected, 12, 1237, 0, 0, 0, Side::Buy, kRejDuplicate, 0};
    for (const OrderEvent& e : evs) {
        uint8_t buf[kMaxFrame];
        const size_t n = encode_sequenced(buf, e);
        Frame f;
        size_t used = 0;
        CHECK(parse_frame(buf, n, f, used) == Parse::Ok);
        CHECK(f.type == Pkt::Sequenced);
        OrderEvent d;
        CHECK(decode_event(f.payload, f.len, d));
        CHECK(d.type == e.type);
        CHECK_EQ(d.token, e.token);
        CHECK_EQ(d.ts, e.ts);
        CHECK_EQ(d.qty, e.type == Msg::Rejected ? 0u : e.qty);
        CHECK_EQ(d.reason, e.reason);
        if (e.type == Msg::Accepted) {
            CHECK_EQ(d.symbol, e.symbol);
            CHECK_EQ(d.ref, e.ref);
        }
    }
}

TEST(proto_login_roundtrip) {
    LoginRequest r;
    copy_padded(r.user, sizeof(r.user), "ALICE");
    copy_padded(r.pass, sizeof(r.pass), "pw");
    r.requested_seq = 42;
    uint8_t buf[kMaxFrame];
    const size_t n = encode_login(buf, r);
    Frame f;
    size_t used = 0;
    CHECK(parse_frame(buf, n, f, used) == Parse::Ok);
    LoginRequest d;
    CHECK(decode_login(f.payload, f.len, d));
    CHECK_EQ(d.requested_seq, 42u);
    CHECK(std::memcmp(d.user, "ALICE ", 6) == 0);

    LoginAccepted a;
    std::memcpy(a.session, "SESSION001", 10);
    a.first_seq = 5;
    a.end_seq = 9;
    const size_t m = encode_login_accepted(buf, a);
    CHECK(parse_frame(buf, m, f, used) == Parse::Ok);
    LoginAccepted da;
    CHECK(decode_login_accepted(f.payload, f.len, da));
    CHECK_EQ(da.first_seq, 5u);
    CHECK_EQ(da.end_seq, 9u);
}

TEST(proto_template_patch_equals_fresh_encode) {
    OrderTemplates tpl(8);
    for (uint16_t s = 0; s < 8; ++s)
        for (Side side : {Side::Buy, Side::Sell}) {
            const uint64_t token = 1000 + s;
            const uint8_t* f = tpl.patch(s, side, token, 300 + s, 9990 + s);
            uint8_t fresh[kMaxFrame];
            encode_enter(fresh, EnterOrder{token, side, uint32_t(300 + s), s, uint32_t(9990 + s), Tif::Ioc});
            CHECK(std::memcmp(f, fresh, kEnterFrame) == 0);
        }
}

TEST(proto_parser_handles_partial_and_concatenated_frames) {
    std::vector<uint8_t> stream;
    for (uint64_t i = 1; i <= 3; ++i) {
        uint8_t buf[kMaxFrame];
        const size_t n = encode_sequenced(buf, OrderEvent{Msg::Canceled, i, i, 1, 0, 0, Side::Buy, kReasonIoc, 0});
        stream.insert(stream.end(), buf, buf + n);
    }
    // Feed it one byte at a time: every frame must be found exactly once.
    std::vector<uint8_t> acc;
    uint64_t found = 0;
    for (uint8_t b : stream) {
        acc.push_back(b);
        Frame f;
        size_t used = 0;
        if (parse_frame(acc.data(), acc.size(), f, used) == Parse::Ok) {
            OrderEvent e;
            CHECK(decode_event(f.payload, f.len, e));
            CHECK_EQ(e.token, found + 1);
            ++found;
            acc.erase(acc.begin(), acc.begin() + long(used));
        }
    }
    CHECK_EQ(found, 3u);
    CHECK(acc.empty());
}

// Deterministic fuzz: random and mutated inputs must never be accepted with
// impossible values, and must never read out of bounds (ASan catches that).
TEST(proto_decoders_survive_random_and_mutated_input) {
    feed::SplitMix64 rng(7);
    uint64_t accepted = 0;
    for (int i = 0; i < 200'000; ++i) {
        uint8_t buf[80];
        const size_t n = size_t(rng.below(sizeof(buf)));
        for (size_t k = 0; k < n; ++k) buf[k] = uint8_t(rng.next());
        if (i % 2 == 0 && n > 20) {  // half the time start from a valid frame and flip bytes
            encode_enter(buf, EnterOrder{rng.next(), Side::Buy, 1, 1, 1, Tif::Ioc});
            buf[rng.below(24)] ^= uint8_t(1u << rng.below(8));
        }
        Frame f;
        size_t used = 0;
        if (parse_frame(buf, n, f, used) != Parse::Ok) continue;
        CHECK(used <= n);
        ClientMsg m;
        if (decode_client_msg(f.payload, f.len, m) && m.type == Msg::EnterOrder) {
            CHECK(m.enter.side == Side::Buy || m.enter.side == Side::Sell);
            ++accepted;
        }
        OrderEvent e;
        (void)decode_event(f.payload, f.len, e);
        LoginRequest lr;
        (void)decode_login(f.payload, f.len, lr);
    }
    CHECK(accepted > 0);  // the mutation half produced some valid orders
}
