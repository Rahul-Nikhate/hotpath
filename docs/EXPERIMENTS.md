# Experiments

Each experiment: question → hypothesis → how to run → result → explanation. Development-machine results are from an **Apple M1 Pro, macOS 15, unpinned**, where the timer resolution is 41.7 ns. Re-run on Linux with `scripts/run_experiments.sh` and add a column.

---

## 1a. What does the fused decision path cost?

- **Question:** from "message delivered" to "order bytes ready", how long does our own code take?
- **Hypothesis:** tens of nanoseconds per stage; well under 200 ns in total when caches are warm.
- **Run:** `hp_bench fastpath`. This is the real `FastPath` with an in-memory feed and gateway, so there are no sockets in the measurement.
- **M1 result:** p50 83 ns, p99 125 ns, p99.9 211 ns (2M messages, ~29k orders). Strategy, risk and encode each read 0–42 ns: below the timer's resolution.
- **To explain:** with the cycle counter on x86 (~0.3 ns resolution) the per-stage breakdown becomes meaningful. That's the Linux column to fill in.

## 1b. What does each queue design choice buy, and what does a hop cost?

- **Question:** starting from a mutex-protected ring, what does each change to an SPSC queue buy?
- **Hypothesis:** lock-free beats mutex; acquire/release beats seq_cst on stores; padding removes false sharing between head and tail; cached indices remove most remaining cross-core reads.
- **Run:** `hp_bench queue [--cpus 2,4]`. One template, `SpscQueue<T, N, SpscOptions>`, so each variant differs by exactly one change.

| Variant | M1 (M items/s) | Linux |
|---|---|---|
| mutex + ring | 23.2 | |
| SPSC, seq_cst, unpadded | 28.6 | |
| SPSC, acquire/release | 35.0 | |
| + `alignas(64)` padding | **48.6** | |
| + cached indices | 48.9 | |

- **Explanation:**
  - **Padding was the biggest step.** Unpadded, `head_` and `tail_` share a cache line: every push invalidates the consumer's copy and every pop the producer's (false sharing, MESI ping-pong).
  - **Cached indices gave nothing measurable on M1.** In a throughput test the queue is rarely near full or empty, and M1's cluster-shared L2 makes the remaining cross-core reads cheap. Expect a visible gain on multi-socket or large-core-count x86, where a cross-core line transfer costs more. *This is a good "the result surprised me" talking point.*
  - **seq_cst vs acquire/release:**
    - On x86, a seq_cst store compiles to `xchg` (a full fence), while release stores and acquire loads are plain `mov`s.
    - On ARMv8 the difference sits in the loads: seq_cst loads must use `ldar`, while acquire loads can use the weaker `ldapr` (ARMv8.3+, which M1 has). Stores are `stlr` either way.
    - Either way, the queue only needs acquire/release (see `04-concurrency-lock-free.md` in the prep notes).
- **Hop cost:** ping-pong round trip p50 ≈ 83 ns both thread-to-thread and process-to-process over shared memory. Once memory is mapped, a process boundary costs nothing extra: it's the same cache-coherence traffic. That's why multi-process designs over shared memory are viable.

## 2. Why pre-fault and lock memory, and why huge pages?

- **Question:** what does the first touch of fresh memory cost on the hot path, and what do huge pages do for the TLB?
- **Hypothesis:** lazy allocation makes every first touch of a page a minor fault (~0.5–2 µs). Huge pages cut TLB misses on large random-access structures.
- **Run:** `hp_bench pages [--region-mb 256]` (on Linux with `vm.nr_hugepages` reserved for the 2 MB row). Engine: `--no-prefault` vs default vs `--huge`.

| Variant | First touch (ns / 64 B record) | Random read (ns) | Minor faults |
|---|---|---|---|
| lazy (M1, 16 KB pages) | 13.6 | 9.0 | 16,384 |
| pre-faulted + mlock (M1) | **1.0** | 5.9 | 0 |
| 4 KB lazy (Linux VM) | 26.4 | 11.4 | 65,536 |
| 4 KB pre-faulted (Linux VM) | 1.4 | 9.4 | 0 |
| **2 MB huge pages (Linux VM)** | 1.2 | **8.1** | 0 |

On Linux, 2 MB pages read ~14% faster at random than pre-faulted 4 KB pages: fewer TLB misses, because each entry covers 512× more memory. Hardware dTLB counters aren't available in a VM on a Mac, so the TLB explanation is inferred from the timing, not counted.

- **Explanation:**
  - 16,384 faults = 256 MB ÷ 16 KB: Apple Silicon uses 16 KB pages (x86 Linux uses 4 KB, so expect 65,536 faults there).
  - The extra 12.6 ns per record × 256 records per 16 KB page ≈ 3.2 µs per fault: a kernel trap plus zeroing a 16 KB page. On x86 Linux with 4 KB pages, expect roughly 0.5–1 µs per fault, but four times as many of them.
  - Random reads are slower after lazy allocation, likely because pages end up scattered and some page-table work remains.
  - **Bug found by this experiment:** `mlock` faults pages in, so the region code originally made "lazy" regions pre-faulted by accident. It now locks only when pre-faulting.
  - On Linux, add `perf stat -e dTLB-load-misses` to the 4 KB vs 2 MB comparison.

## 3. Why is the first order after a quiet period slow, and does warming fix it?

- **Question:** with sparse events (one every 200 µs) and a neighbour thrashing the shared cache, does re-running the decision path every 20 µs on a dummy event (without sending) lower decision latency?
- **Hypothesis:** yes, mostly at the tail (p90–p99): instructions, data and branch history stay hot.
- **Run:** `hp_bench fastpath --sparse-us 200` (add `--no-thrash` for the control case).
- **M1 result:** inconclusive. Across repeated runs the warmed and cold distributions overlapped (p50 211 ns both with thrashing; p90/p99 swapped order between runs).
- **Why:** the 41.7 ns timer quantises the very effect being measured, macOS can't pin the fast path or keep the thrasher off its cluster, and Apple's large shared caches blunt the thrashing.
- **What a Linux run needs:**
  - fast path pinned to an isolated core, thrasher pinned to a sibling on the same socket (shared L3)
  - x86 TSC timing
  - `perf stat -e L1-icache-load-misses,branch-misses` for both modes
- **Correctness:** `warming_does_not_change_decisions` proves warming never changes an order.

## 4. Why pin and isolate?

- **Question:** what happens to p99.9 and max when the fast path shares cores with other work?
- **Hypothesis:** the median barely moves; the tail improves by an order of magnitude once timer ticks, interrupts and other tasks stay off the core.
- **Run (Linux only):**
  1. `scripts/tune_linux.sh` (boot parameters + IRQ affinity).
  2. `ISOLATED=3 ./scripts/run_experiments.sh` runs the end-to-end demo unpinned, then pinned to the isolated core with `stress-ng` running elsewhere.
- **What to compare:** decision and to-wire p99.9 and max, plus `nonvoluntary_ctxt_switches` in `/proc/<pid>/status` (should stay at 0 when isolated).
- **Status: not measured.** I haven't had a bare-metal x86 Linux host; Docker on a Mac can't isolate cores. The scripts are ready. In interviews I say exactly this rather than quoting an expected number as if it were measured.
- **Expected outcome (a hypothesis, not a result):** the median barely changes, while p99.9 and max drop because timer ticks, IRQs and other tasks no longer land on the core.

## Stretch 5. Nagle + delayed ACK

Not implemented as a switch: `TCP_NODELAY` is always on, set by project 1's `tcp_connect`. To demonstrate the stall, remove it and watch to-wire latency jump to tens of milliseconds whenever two small orders go out back to back.

## Stretch 6. Virtual vs template vs `std::variant`

- **Run:** `hp_bench dispatch`.

| Variant | M1 (ns/call) |
|---|---|
| direct / template (inlined) | 2.39 |
| virtual, one type | 2.82 |
| `std::variant`, one type | 3.23 |
| virtual, two types mixed randomly | **5.14** |
| `std::variant`, two types mixed | 4.19 |

- **Explanation:**
  - A virtual call whose target is always the same is predicted perfectly by the indirect branch predictor: it costs a load of the vptr plus a slot, and the lost inlining.
  - When the target changes unpredictably it costs ~2× (a pipeline flush on mispredict).
  - `std::variant` compiles to a jump table on the index. It does better than virtual when mixed and slightly worse when uniform.
  - So "virtual is slow" is only half true. The honest statement is "unpredictable indirect branches and lost inlining are slow".
