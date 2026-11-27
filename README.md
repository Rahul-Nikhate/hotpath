# Hotpath: a tick-to-trade engine in C++20

The trading-firm side of the loop. A single thread on one core takes each market-data packet through a parser and order book, a strategy, pre-trade risk checks, and out as a pre-encoded binary order over TCP, with **no queue hops on the critical path**. A separate control process records every decision to a journal and owns the risk limits and kill switch, talking to the fast path through **lock-free shared-memory structures**. Every design choice is backed by a measured experiment, and a replay tool proves the engine's decisions are **bit-for-bit reproducible**.

It pairs with my [Low-Latency Market Data Handler](https://github.com/Rahul-Nikhate/Low-Latency-Data-Handler) (*tick in*: multicast feed → gap recovery → order book), which Hotpath reuses as a library rather than rebuilding. Together they cover the whole loop: packet in → book → decision → risk → order out → ack back.

```
  hp_sim (exchange simulator)
     │ UDP multicast/unicast market data (MoldUDP64-style)    ▲ TCP orders        │ TCP acks / fills
     ▼                                                         │ (OUCH-style)      ▼
 ┌─────────────── FAST PATH: one thread, busy-polling, optionally pinned to an isolated core ───────────────┐
 │ recv ─► P1 parser + sequencer ─► P1 book ─► strategy ─► risk ─► patch order template ─► send()           │
 │   also polls: order responses · control seqlock (limits, kill) · heartbeats · hot-path warming          │
 │   then (after the send): every input and output ─► SPSC journal queue (never blocks; drops are counted)  │
 └───────────────────────────────────────┬──────────────────────────────────────────────────────────────────┘
                                         │ shared memory (POSIX shm or hugetlbfs, pre-faulted, mlocked)
 ┌──────────────── hp_control (housekeeping core) ──────────────────────────────────────────────────────────┐
 │ drains journal → append-only file   ·  publishes limits + kill switch (seqlock)  ·  heartbeats both ways │
 └───────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                     hp_replay journal.bin  → re-runs the same decision core → IDENTICAL / DIFFERENT
```

## Results

### Development machine: Apple M1 Pro, macOS 15, loopback, unpinned

> These come from my laptop, not a tuned Linux server. macOS can't pin threads or isolate cores, and the ARM generic timer ticks every **41.7 ns**, so any per-stage number under ~50 ns is quantised to 0, 42 or 83 ns. The numbers that matter for interviews come from `scripts/run_experiments.sh` on bare-metal x86 Linux (table below, to fill in). Everything here is reproducible with one command.

**End to end**: 1,000,000 messages at 200k msg/s, 0.5% simulated packet loss, order session dropped once after 100 orders (`COUNT=1000000 ./scripts/run_demo.sh`):

| Metric (493 orders after a 10k-message warm-up) | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| **Decision latency**: packet received → order bytes ready | 375 ns | 2.9 µs | 4.2 µs | 8.3 µs |
| Decision, our code only: message delivered → order bytes ready | 167 ns | 423 ns | 3.1 µs | 8.3 µs |
| **To-wire**: packet received → `send()` returned (kernel TCP, loopback) | 3.3 µs | 7.3 µs | 14.8 µs | 16.4 µs |
| …of which the `send()` syscall alone | 2.8 µs | 6.5 µs | 14.6 µs | 15.9 µs |

- **The kernel's TCP send path dominates to-wire latency** (~2.8 µs of ~3.3 µs). That's the honest argument for kernel bypass (OpenOnload / ef_vi), which this project measures but deliberately doesn't implement.
- **The gap between the first two rows is batching.** Each datagram carries up to 8 messages but gets one receive timestamp, so a message late in the packet waits behind the earlier ones. That's real queueing, so the headline number includes it; the second row isolates the code.
- **Correctness in the same run:**
  - 1,532 gaps (4,991 messages) recovered.
  - Engine book hash = exchange's reference hash.
  - Reconnect replayed the missed acknowledgements; one order the exchange never received was marked *Lost*; zero duplicate orders.
  - Journal of 1,005,636 records with 0 dropped; replay **IDENTICAL across 3 runs**.

**Experiments** (`./build/hp_bench all`; each one is explained in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md)):

| # | Question | What I measured (M1 Pro) |
|---|---|---|
| 1b | What does each queue design choice buy? | SPSC throughput: mutex 23 → `seq_cst` 29 → acquire/release 35 → **+`alignas(64)` 49** → +cached indices 49 M items/s. Removing false sharing was the biggest single step. Index caching adds nothing on this chip (see notes) |
| 1b | Thread hop vs process hop | Round trip p50 ≈ 83 ns for both: shared memory makes a process boundary free once mapped (timer-limited; re-measure on x86) |
| 1a | Cost of the fused decision path (no network) | p50 83 ns, p99 125 ns from message to order bytes |
| 2 | Why pre-fault? | First touch of a fresh 256 MB region: **13.6 ns/record and 16,384 page faults lazy vs 1.0 ns and 0 pre-faulted** (13×). 16,384 faults = 256 MB / 16 KB, because Apple Silicon uses 16 KB pages. Huge pages: Linux only |
| 3 | Does warming help sparse events? | **Inconclusive on macOS**: differences were within run-to-run noise at 42 ns timer resolution and unpinned scheduling. This is the experiment that most needs the Linux run |
| – | Template patch vs full encode | 2.6 vs 3.6 ns per order |
| 6 | Virtual vs template vs `std::variant` | direct 2.4 ns · virtual 2.8 ns (predictable) · **virtual with 2 types mixed 5.1 ns** · variant mixed 4.2 ns. A predictable virtual call is nearly free; an unpredictable one costs ~2× |

### Linux build check: Ubuntu 24.04, GCC 13 / clang, Docker on the same M1

This checks that the code is correct on Linux, not how fast it is (it's a virtual machine).
- Builds warning-free with GCC 13; **28/28 tests pass**.
- The 3-process demo recovers 1,400 gaps, survives the injected disconnect, matches the exchange's book hash, and replays **identically**.
- Page-fault experiment on 4 KB pages: **65,536 faults lazy vs 0 pre-faulted** (16.7 vs 1.1 ns per record on first touch).
- libFuzzer with ASan/UBSan ran **11.9 million** order-entry inputs in 30 s with no crash.
- Huge pages and core isolation need a real host (below).

### Not measured yet: bare-metal x86 Linux

**Not yet measured:**
- Experiment 4 (core isolation with `isolcpus` / `nohz_full` / IRQ affinity)
- the 2 MB huge-page row of experiment 2
- an x86 TSC-resolution re-run of experiments 1 and 3

These need a bare-metal x86 Linux host, which I haven't had access to. The scripts are complete and checked to build and run on Linux: `scripts/tune_linux.sh`, then `ISOLATED=3 CPUS=3,5 HUGE=1 scripts/run_experiments.sh`. I'm not quoting numbers I haven't measured. See [What I did and didn't measure](#what-i-did-and-didnt-measure).

## Build and run

Needs a C++20 compiler and make, nothing else. Project 1 must sit next to this directory (or set `P1=path`):

```bash
git clone https://github.com/Rahul-Nikhate/Low-Latency-Data-Handler
git clone https://github.com/Rahul-Nikhate/hotpath Hotpath && cd Hotpath
make -j && make test          # 28 tests: unit, session, end-to-end (loss + disconnect + replay)
make asan && make tsan        # same tests under sanitizers
./scripts/run_demo.sh         # 3 processes over loopback + replay
./build/hp_bench all          # the experiments
```

Running the pieces by hand (three terminals):

```bash
./build/hp_sim --count 2000000 --rate 200000 --drop 0.005 --disconnect-after 100
./build/hp_engine --name demo --cpu 3 --huge        # --cpu/--huge: Linux
./build/hp_control --name demo --journal journal.bin  # type: kill | resume | limit all qty 500 | status
./build/hp_replay journal.bin --runs 3
```

On Linux, `scripts/tune_linux.sh` prints the host setup (isolated cores, `nohz_full`, IRQ affinity, huge pages, C-states) and `scripts/run_experiments.sh` runs everything into `results/<host>/`. Plot with `python3 scripts/plot.py results/<host>`.

## What's inside

| Path | What |
|---|---|
| `src/engine/fast_path.h` | The fused loop: stage timestamps, journaling after the send, gating, control polling, warming |
| `src/engine/core.h` | Deterministic decision core (book → strategy → risk → order manager). No I/O, no clock: the live engine and replay run this same class |
| `src/engine/strategy.h` | `Strategy` concept + a deliberately simple queue-imbalance taker |
| `src/engine/risk.h` | Kill switch, max qty/notional, price band, position incl. open orders, token-bucket rate limit, in flat arrays |
| `src/engine/order_manager.h` | Order state machine, positions, open exposure; handles fill-after-cancel races |
| `src/engine/gateway.*` | Non-blocking TCP session client: login with sequence numbers, heartbeats, replay on reconnect, Lost-order rule |
| `src/proto/` | OUCH-style messages in SoupBinTCP-style frames, byte-wise big-endian codec, pre-encoded templates ([docs/PROTOCOL.md](docs/PROTOCOL.md)) |
| `src/shm/` | `SpscQueue` (with switchable variants for the experiment), `Seqlock`, named shared-memory/hugetlbfs `Region`, shared layout |
| `src/sim/` | Exchange simulator: feed + recovery (project 1's protocol) + order entry with replay log and fault injection |
| `src/replay/` | Journal reader and deterministic replay |
| `bench/` | `hp_bench queue \| fastpath \| pages \| encode \| dispatch` |
| `tests/` | 28 tests including a reference-model fuzz of the risk checks and decoder fuzzing; `fuzz/` has a libFuzzer target |

## Design decisions (full log in [docs/DESIGN.md](docs/DESIGN.md))

1. **One thread for the hot path, not a pipeline**: every cross-core hop is a cache-line transfer. Splitting stages helps throughput and isolation, not latency (exp. 1).
2. **SPSC, not MPMC**: each queue has a single writer, so no CAS loops, just acquire/release.
3. **Integer ticks, never `double`, for prices.**
4. **Templates and a concept, not virtual calls, on the hot path**: so the whole path can inline (stretch exp. 6 shows when it matters).
5. **Kernel TCP with busy-polling; to-wire latency reported separately**: what kernel bypass would remove is visible in the numbers.
6. **The journal never blocks trading**: a full queue drops and counts, and the run is then reported as not replayable.
7. **Pre-faulted, locked shared memory; huge pages on Linux** (exp. 2).
8. **Warm the path instead of hoping the cache stays hot** (exp. 3), proven not to change any decision (test).
9. **Never resend an order after a reconnect**: anything still unacknowledged after the replay provably never arrived, so it is marked Lost, not resent.

## What I did and didn't measure

| Claim | Status |
|---|---|
| Decision and to-wire latency, queue designs, page faults, encoding, dispatch | **Measured** on an Apple M1 Pro, macOS, loopback, unpinned (numbers above) |
| Correct on Linux: build, 28 tests, 3-process demo + replay, fuzzing | **Verified** in Ubuntu 24.04 (GCC 13 / clang) under Docker |
| Core isolation (`isolcpus`, `nohz_full`, IRQ affinity, pinning) | **Designed and scripted, not measured**: needs bare-metal x86 Linux |
| 2 MB huge pages (TLB misses) | **Implemented** (`--huge`, hugetlbfs) **and scripted, not measured**: same reason |
| Kernel bypass, NIC hardware timestamps, FPGA | **Not built**: discussed in docs/DESIGN.md |

If asked *"Did you measure `isolcpus`?"*, the honest answer is:

> "I wrote the tuning and experiment scripts and checked they build and run on Linux, but I didn't have bare-metal x86, so my published numbers are from an M1 over loopback. I'd expect isolation to mainly cut the p99.9 and the max rather than the median, because it removes timer ticks, interrupts and other tasks from the core. That's what the experiment is set up to show, using `nonvoluntary_ctxt_switches` and `/proc/interrupts` as the evidence."

## Honest limitations

- **The reused project-1 order book allocates.** It uses `std::map` and `unordered_map` nodes, so the hot thread performs ~0.4 heap allocations per market message. Hotpath's own stages allocate nothing (`hot_stages_allocate_nothing` test). Replacing the book with a flat price-level array and an order pool is the first project-1 upgrade.
- **The simulator fills IOCs at its top of book with no market impact.** It's a harness, not a matching engine.
- **All numbers so far are in-host loopback on macOS.** No NIC timestamps, no kernel bypass, no FPGA.
- **The strategy is trivial on purpose.** This project is about infrastructure latency and correctness, not alpha.
