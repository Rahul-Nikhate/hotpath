// hp_control: the housekeeping process. Runs on a non-critical core and never
// touches the hot path directly. It:
//   * publishes risk limits and the kill switch through the seqlock
//   * drains the journal queue to an append-only file (for replay)
//   * heartbeats (the engine stops trading if this process goes silent)
//   * watches the engine's heartbeat and reports if it dies
//   * accepts commands on stdin: kill | resume | limit <sym|all> <field> <value> | status | quit
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>

#include "common/args.h"
#include "common/clock.h"
#include "engine/defaults.h"
#include "shm/layout.h"
#include "shm/region.h"

using namespace hp;

namespace {

void usage() {
    std::puts(
        "hp_control [options]\n"
        "  --name N            instance to attach to (default demo)\n"
        "  --journal FILE      journal output (default journal.bin)\n"
        "  --symbols N         instruments (default 8)\n"
        "  --huge              the engine's region is on hugetlbfs\n"
        "  --max-qty --max-pos --band --rate-limit --burst --max-notional   default limits\n"
        "  --kill-after-s S    flip the kill switch after S seconds (demo/test)\n"
        "  --engine-timeout-s S  declare the engine dead after S s without heartbeat (default 2)\n"
        "  --stdin             read commands from stdin even when it is not a terminal\n"
        "  --quiet             no periodic status line\n");
}

bool apply_command(const std::string& line, ControlBlock& cb, bool& quit) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd == "kill") {
        cb.kill = 1;
    } else if (cmd == "resume") {
        cb.kill = 0;
    } else if (cmd == "quit") {
        quit = true;
        return false;
    } else if (cmd == "limit") {
        std::string sym, field;
        uint64_t v = 0;
        in >> sym >> field >> v;
        auto set = [&](SymbolLimits& l) {
            if (field == "qty") l.max_order_qty = uint32_t(v);
            else if (field == "pos") l.max_position = uint32_t(v);
            else if (field == "band") l.band_ticks = uint32_t(v);
            else if (field == "rate") l.rate_per_sec = uint32_t(v);
            else if (field == "burst") l.burst = uint32_t(v);
            else if (field == "notional") l.max_notional = v;
        };
        if (sym == "all") for (auto& l : cb.limits) set(l);
        else if (std::stoul(sym) < kMaxSymbols) set(cb.limits[std::stoul(sym)]);
    } else {
        return false;
    }
    ++cb.version;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args a(argc, argv);
    if (a.has("help")) {
        usage();
        return 0;
    }
    const std::string name = a.str("name", "demo");
    const bool quiet = a.has("quiet");
    Region region = Region::attach(name, a.has("huge"), 30'000);
    auto* shm = static_cast<SharedLayout*>(region.data());

    // Wait until the engine has finished constructing the layout.
    auto* magic = reinterpret_cast<std::atomic<uint64_t>*>(&shm->hdr.magic);
    for (int i = 0; i < 3000 && magic->load(std::memory_order_acquire) != kMagic; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (magic->load(std::memory_order_acquire) != kMagic || shm->hdr.layout_hash != layout_hash()) {
        std::fprintf(stderr, "[control] region layout mismatch: rebuild both binaries\n");
        return 1;
    }

    ControlBlock cb = default_control(uint16_t(a.u64("symbols", 8)), &a);
    shm->control.store(cb);
    shm->hdr.control_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);

    const std::string path = a.str("journal", "journal.bin");
    FILE* jf = std::fopen(path.c_str(), "wb");
    if (!jf) {
        std::perror("journal");
        return 1;
    }
    static char iobuf[1 << 20];
    std::setvbuf(jf, iobuf, _IOFBF, sizeof(iobuf));

    const bool interactive = ::isatty(0);
    bool read_stdin = interactive || a.has("stdin");
    const double kill_after = a.f64("kill-after-s", 0);
    const uint64_t engine_timeout = uint64_t(a.f64("engine-timeout-s", 2) * 1e9);
    const uint64_t started = now_ns();
    uint64_t written = 0, last_status = started;
    bool kill_sent = false, quit = false, engine_dead = false;
    std::string line;

    if (interactive && !quiet) std::fprintf(stderr, "[control] commands: kill | resume | limit <sym|all> <qty|pos|band|rate|burst|notional> <v> | status | quit\n");

    for (;;) {
        const uint64_t now = now_ns();
        shm->hdr.control_heartbeat_ns.store(now, std::memory_order_relaxed);

        size_t drained = 0;
        JournalRecord r;
        while (drained < 8192 && shm->journal.try_pop(r)) {
            std::fwrite(&r, sizeof(r), 1, jf);
            ++drained;
        }
        written += drained;

        if (kill_after > 0 && !kill_sent && double(now - started) * 1e-9 >= kill_after) {
            cb.kill = 1;
            ++cb.version;
            shm->control.store(cb);
            kill_sent = true;
            std::fprintf(stderr, "[control] kill switch ON (version %llu)\n", (unsigned long long)cb.version);
        }

        if (read_stdin) {
            pollfd p{0, POLLIN, 0};
            if (::poll(&p, 1, 0) > 0) {
                char tmp[256];
                const ssize_t n = ::read(0, tmp, sizeof(tmp));
                if (n <= 0) {
                    read_stdin = false;  // EOF: stop reading, keep running
                } else {
                    line.append(tmp, size_t(n));
                    for (size_t pos; (pos = line.find('\n')) != std::string::npos;) {
                        const std::string cmd = line.substr(0, pos);
                        line.erase(0, pos + 1);
                        if (cmd == "status") {
                            std::fprintf(stderr, "[control] version %llu kill=%u msgs %llu orders %llu journal %llu\n",
                                         (unsigned long long)cb.version, cb.kill,
                                         (unsigned long long)shm->hdr.engine_market_msgs.load(),
                                         (unsigned long long)shm->hdr.engine_orders.load(), (unsigned long long)written);
                        } else if (apply_command(cmd, cb, quit)) {
                            shm->control.store(cb);
                            std::fprintf(stderr, "[control] applied version %llu (kill=%u)\n",
                                         (unsigned long long)cb.version, cb.kill);
                        } else if (!quit && !cmd.empty()) {
                            std::fprintf(stderr, "[control] unknown command: %s\n", cmd.c_str());
                        }
                    }
                }
            }
        }

        const auto state = EngineState(shm->hdr.engine_state.load(std::memory_order_acquire));
        if (state == EngineState::Finished && drained == 0 && shm->journal.size_approx() == 0) break;
        const uint64_t hb = shm->hdr.engine_heartbeat_ns.load(std::memory_order_relaxed);
        if (state == EngineState::Running && hb && now > hb && now - hb > engine_timeout) {
            engine_dead = true;
            std::fprintf(stderr, "[control] engine heartbeat lost: engine presumed dead\n");
            break;
        }
        if (quit) break;

        if (!quiet && now - last_status >= 1'000'000'000ULL) {
            last_status = now;
            std::fprintf(stderr, "[control] msgs %llu orders %llu journal %llu dropped %llu kill=%u\n",
                         (unsigned long long)shm->hdr.engine_market_msgs.load(),
                         (unsigned long long)shm->hdr.engine_orders.load(), (unsigned long long)written,
                         (unsigned long long)shm->hdr.journal_dropped.load(), cb.kill);
        }
        if (drained == 0) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    std::fclose(jf);
    std::printf("[control] journal %s: %llu records, %llu dropped%s\n", path.c_str(), (unsigned long long)written,
                (unsigned long long)shm->hdr.journal_dropped.load(), engine_dead ? " (engine died)" : "");
    return engine_dead ? 3 : 0;
}
