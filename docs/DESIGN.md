# Design

## Goals and non-goals

**Goals:**
- the lowest and most predictable latency from market-data packet to order bytes
- decisions that are provably reproducible
- safe behaviour when a connection or a process fails
- every design choice measured

**Non-goals:**
- alpha (the strategy is a placeholder)
- a matching engine (the simulator is a harness)
- kernel bypass and FPGA (measured, discussed, not built)

## Processes and threads

| Process / thread | Core | Job |
|---|---|---|
| `hp_engine` main thread = **fast path** | isolated, pinned (`--cpu`) | market data, decisions, risk, order send, order responses, control polling |
| `hp_engine` recovery thread (project 1's `RecoveryClient`) | any | blocking TCP to the retransmission server, only while a gap is open |
| `hp_control` | housekeeping | journal → file, limits and kill switch, heartbeats, CLI |
| `hp_sim` | another core | exchange simulator (harness) |

Shared memory between engine and control (`src/shm/layout.h`):

| Structure | Direction | Why this structure |
|---|---|---|
| `SharedHeader` | both | magic + layout hash (mismatched builds refuse to attach), heartbeats, engine state, counters |
| `Seqlock<ControlBlock>` | control → engine | one writer, a reader that must never block it; the engine checks one sequence word per maintenance tick and copies the ~2 KB block only when it changes |
| `SpscQueue<JournalRecord, 65536>` | engine → control | one writer, one reader; 64-byte records = one cache line each; never blocks the engine |

## The fast path, step by step

1. `recv()` a datagram (non-blocking, busy-polled) and stamp it with the cycle counter. **That's the "tick".**
2. Project 1's `PacketReader` and `Sequencer` deliver in-order messages. Gaps are buffered and recovered over TCP on another thread.
3. `Core::on_market`:
   - book update (project 1)
   - `strategy.on_book` (C++20 concept, inlined)
   - `risk.check` (flat arrays, integer compares)
   - `om.on_send` (fixed table)
4. `FastPath::on_order` → `Gateway::send_order` patches 16 bytes of a pre-built 24-byte frame and calls `send()` once (`TCP_NODELAY`).
5. **Only now** is the journal written: the market record (held back) and the order record. Journaling is off the tick-to-order path, yet the journal order is exactly the order the core saw.
6. Order responses are polled and applied to the order manager; each is journaled.
7. Every ~100 µs (cold path). The loop decides this by reading the CPU cycle counter (one register read), not the system clock. The real clock is read only when maintenance is due: profiling showed the old per-iteration `steady_clock::now()` at ~7% of the engine's instructions (see EXPERIMENTS.md, "Profiling"). Maintenance covers:
   - gateway heartbeats and reconnects
   - the control seqlock's sequence word
   - trading gating
   - shared heartbeat and counters
   - warming

### Trading gating

Trading is enabled only when **all** of these hold, and every change is journaled (so replay sees it too):
- the order session is logged in and its replay has caught up
- the feed has no open gap (the book could be wrong)
- the control process is alive and has published limits
- the feed hasn't ended

## Decision log

Each entry: decision → alternatives → why → evidence.

1. **One thread for the whole critical path.**
   - *Alternatives:* a pipeline of threads, or processes, joined by SPSC queues.
   - *Why:* every hop is at least one cache-line transfer between cores (~50–150 ns on x86) plus queueing. A pipeline buys throughput and isolation, not latency.
   - *Evidence:* exp. 1a (fused path cost) and 1b (hop cost).
2. **SPSC queues, single-writer state.**
   - *Alternatives:* MPMC queues, mutexes.
   - *Why:* with one writer per index, push and pop are a load and a release store, with no CAS retry loop (wait-free). Shared mutable state with several writers is avoided by design.
   - *Evidence:* exp. 1b throughput progression.
3. **Integer ticks for prices.**
   - *Alternative:* `double`.
   - *Why:* exact comparison and arithmetic, and exchange protocols are integer anyway.
4. **Static polymorphism.**
   - *Alternative:* a virtual `IStrategy`.
   - *Why:* lets the compiler inline the whole path; virtual dispatch costs most when the target is unpredictable.
   - *Evidence:* stretch exp. 6 (2.8 ns predictable vs 5.1 ns mixed vs 2.4 ns direct on M1).
5. **Kernel TCP, busy-polled, with to-wire reported separately.**
   - *Alternative:* OpenOnload / ef_vi / DPDK.
   - *Why:* keeps the project portable and honest. The to-wire vs decision split shows exactly what bypass would remove (the `send()` syscall is ~85% of to-wire on loopback).
6. **The journal never blocks.**
   - *Alternative:* block, or grow an unbounded buffer.
   - *Why:* slowing trading to protect a log is the wrong trade. Drops are counted, and replay refuses to vouch for a run with drops.
7. **Pre-faulted, mlocked shared memory; hugetlbfs on Linux.**
   - *Why:* no page faults on the hot path, no TLB misses on large structures.
   - *Evidence:* exp. 2 (13× slower first touch when lazy).
8. **Warming.**
   - *Alternative:* trust the cache.
   - *Why:* during quiet periods other code evicts the hot path's instructions and data, and branch predictors forget. Warming re-runs the decision path on a dummy event without sending.
   - *Evidence:* exp. 3 (inconclusive on macOS; Linux run pending). A test proves warming changes no decision.
9. **Never resend after a reconnect; mark unacknowledged orders Lost.**
   - *Alternative:* resend unacknowledged orders.
   - *Why:* the exchange acknowledges every order it receives, and the login replay contains every acknowledgement. So once the replay has caught up, an order with no acknowledgement provably never arrived. Resending could create duplicates if that reasoning were wrong; marking it Lost cannot.
   - *Evidence:* the session test and the e2e test check that the exchange's unique tokens equal sent minus lost.
10. **No clock reads in decision logic.**
    - *Why:* the risk rate limiter uses the market-data timestamp, so replay reproduces rate-limit decisions too.

## Failure handling

| Failure | What happens |
|---|---|
| Market-data packet loss | Sequencer detects the gap → trading gated off → TCP recovery (project 1's protocol) → book identical to the exchange's (hash checked in tests) → trading resumes |
| Order session drops | Gateway reconnects with "next expected sequence" → exchange replays missed responses → still-unacknowledged orders marked Lost → trading resumes. Never resends |
| Control process dies | Its heartbeat goes stale → engine gates trading off within 1 s |
| Engine dies | Control sees the engine heartbeat stop and exits with a report. The exchange's cancel-on-disconnect would cancel resting orders; this strategy only sends IOCs, so nothing rests |
| Journal queue full | Record dropped and counted; run reported as not replayable |
| Kill switch | Control bumps the version with `kill=1` → engine applies it on the next maintenance tick → every order is refused by risk (`killed`) |

## Known gaps and next steps

- **Project 1's map-based book allocates per new order.** Replace it with a flat price-level array, an intrusive order list and a pool.
- **Kernel bypass.** OpenOnload is a drop-in; ef_vi or AF_XDP for raw frames.
- **NIC hardware timestamps** (`SO_TIMESTAMPING`) for true wire-to-wire numbers.
- **A quoting strategy** to measure tick-to-cancel, the latency market makers care about most.
