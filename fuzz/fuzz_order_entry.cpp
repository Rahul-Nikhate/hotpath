// libFuzzer target for everything that parses bytes from the network on the
// order-entry path. Build and run with `make fuzz` (needs clang with libFuzzer,
// e.g. on Linux; Apple's clang does not ship it). ASan/UBSan flag any
// out-of-bounds read or undefined behaviour the fuzzer provokes.
#include <cstddef>
#include <cstdint>

#include "proto/order_entry.h"

using namespace hp::proto;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    size_t off = 0;
    while (off < size) {
        Frame f;
        size_t used = 0;
        const Parse p = parse_frame(data + off, size - off, f, used);
        if (p != Parse::Ok) break;
        if (used == 0 || used > size - off) __builtin_trap();  // parser must make progress, stay in bounds
        ClientMsg m;
        (void)decode_client_msg(f.payload, f.len, m);
        OrderEvent e;
        (void)decode_event(f.payload, f.len, e);
        LoginRequest lr;
        (void)decode_login(f.payload, f.len, lr);
        LoginAccepted la;
        (void)decode_login_accepted(f.payload, f.len, la);
        off += used;
    }
    return 0;
}
