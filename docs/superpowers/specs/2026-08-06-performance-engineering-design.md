# darkstar_hft — Milestone 3: Performance Engineering

**Date:** 2026-08-06
**Package:** `darkstar_hft` (namespace `dhft`), repo `/Users/saumyapatel/Dark Star Technologies/Random/CPP_Project`
**Follows:** Milestone 2 (verification harness, 113 tests, reference-model oracle)

## Goal

Reduce the engine's tail latency, with every change proven correct by the reference model
and proven beneficial by measurement. No change ships on reasoning alone.

## Why this milestone can exist now

Two prerequisites are in place that were not before:

1. **An oracle.** `NaiveEngine` is an independent implementation whose internals will not
   change. If the fast engine still agrees with it on 800 random scripts after a rewrite,
   the rewrite preserved behaviour. Divergence produces a minimal reproducer via the
   shrinker.
2. **Release builds.** `relassert` and `release` presets exist, so measurements reflect
   optimised code rather than `-O0` plus sanitizers.

Two prior episodes justify the discipline this milestone imposes. Hash mixing was added
on textbook reasoning and, when finally measured, cost 7.7% throughput and turned a
worst-case bucket chain of 1 into 7 — libc++ uses prime bucket counts, so identity
hashing of sequential ids was already optimal. Separately, trimming `Order` by dropping
redundant `price`/`side` was recommended before research showed that **every** real order
book studied keeps both fields, because a fill report needs them inline and a
`std::list::iterator` cannot name its own container. Both recommendations were wrong, and
both were caught only by evidence.

## Working mode

**COACH MODE.** The user writes the optimisations (Phase 3). The assistant builds the
measurement infrastructure (Phases 1-2) and the final report (Phase 4), and reviews.

Chosen approach for data structures: **hand-roll first, then A/B against a mature
library.** The user implements the pool, intrusive list, slot map, and array ladder;
each is then benchmarked against an established equivalent (`plf::list`,
`boost::intrusive`) to calibrate the hand-written version honestly.

---

## Verified platform constraints

These were measured on the target machine, not assumed. They shape the whole design.

### The timer cannot resolve a single event

```
mach_timebase_info = 125/3  ->  41.667 ns per tick

back-to-back steady_clock::now() deltas, 200000 samples:
    0 ns 48.8%   41 ns 17.0%   42 ns 34.1%   83 ns 0.1%   125 ns 0.0%
```

`std::chrono::steady_clock`, `mach_absolute_time`, `clock_gettime_nsec_np`, and a direct
`CNTVCT_EL0` read are all the same 24 MHz counter. There is no finer userspace clock on
Apple Silicon. The engine currently runs at roughly 64 ns/event — about 1.5 ticks — so a
single event reads as 42 or 83 ns, never 64.

**Consequence, and it is not a compromise but the correct response:**

- **Median and throughput are measured by batching.** Time N events, divide. Exact.
- **The tail is measured per event.** Tail latency is made of *stalls* — an allocator slow
  path, a rehash, a page fault — which are microseconds, i.e. hundreds of ticks, and
  therefore plainly visible at 41.67 ns granularity.

The tail is what this milestone targets, and the tail is what remains measurable.

### The tail exists, and is worth attacking (measured)

Per-event timing over a 400000-event script after warm-up, on the current engine:

```
p50 = 83 ns   p90 = 166   p99 = 292   p99.9 = 833   p99.99 = 1500   max = 115875
events over 1 us: 244 (0.07%)     over 10 us: 4
```

P99.9 is ten times the median and the maximum is over a thousand times it. There is a real
tail, so this milestone is not optimising something that does not exist.

### The instrument distorts what it measures

```
timer read pair: p50 = 41 ns ... max = 18084 ns
```

Two consequences that constrain every conclusion drawn from wall-clock data:

1. **Bracketing a ~64 ns event costs ~41 ns of timer.** Roughly 40% of a per-event
   measurement is the instrument. This is harmless for detecting microsecond stalls but
   makes per-event medians meaningless. **A median may only be quoted from batch timing;
   a per-event p50 is not a valid number and must not appear in the log.**
2. **The timer itself occasionally stalls 18 us.** Part of any observed tail is the OS
   descheduling the thread or a timer hiccup, not the engine.

Point 2 is why the kperf counter reader is load-bearing rather than optional: per-thread
PMU counters accumulate only while the thread is actually running, so a cycle count
excludes descheduling. Wall-clock time alone cannot distinguish an engine stall from a
preemption, and this milestone's entire subject is stalls.

### Other platform facts

- **No CPU pinning exists on macOS.** `THREAD_AFFINITY_POLICY` is a hint about L2 sharing,
  not a binding. QoS class is the only lever: `QOS_CLASS_USER_INTERACTIVE` biases toward
  P-cores; `QOS_CLASS_BACKGROUND` is confined to E-cores. Neither guarantees placement.
- **Thermal state is readable** via `ProcessInfo.thermalState` (public API) — coarse, but
  enough to discard a run that was throttled.
- **PMU counters require root and private frameworks** (`kperf`, `kperfdata`), with
  per-chip event databases in `/usr/share/kpep/*.plist`. Two fixed counters (cycles,
  instructions) plus up to eight configurable, ten simultaneous maximum. Undocumented and
  liable to break on macOS updates.
- **Time Profiler samples at 1 ms**, so a 64 ns event is invisible per-call. Function-level
  attribution requires running the hot path millions of times so sampling accumulates
  proportionally.
- **Valgrind does not run natively on arm64 macOS**, and ASan's LeakSanitizer silently does
  nothing here (verified: it missed a deliberate 4 KB leak). Leak checking uses macOS
  `leaks(1)` against a non-sanitised build.

---

## Scope

**In scope:** benchmark harness, kperf counter reader, baseline and profile, then the
data-structure work — order record layout, object pool, intrusive list, slot map,
array-indexed price ladder, lookup amplification — followed by micro-tuning (branch hints,
prefetch, PGO) and a final report.

**Out of scope:** threading, kernel bypass, FPGA, new order types, real market data,
multi-instrument sharding.

---

## Architecture of the measurement harness

A new test-only module, mirroring the `io/`, `reference/`, `testkit/` pattern:

```
bench/
  include/dhft/bench/Harness.h     replay driver, batch and per-event modes
  include/dhft/bench/Histogram.h   pre-allocated sample buffer -> percentiles
  include/dhft/bench/Counters.h    kperf wrapper, with graceful no-root fallback
  src/...
  CMakeLists.txt                   dhft_bench
apps/bench.cpp                     the runnable benchmark, JSON output
```

### Replay methodology

Modelled on `exchange-core`'s published approach, which is the closest real precedent for
benchmarking a stateful matching engine:

- **A fixed, recorded script**, generated once with a known seed and committed, so every
  build is compared against identical input. Not regenerated per run.
- **Warm-up to a steady-state book** before recording, so measurements are not dominated by
  an empty-book ramp. The warm-up prefix is discarded. **"Steady state" must be a stated
  target book depth** (resting order count and populated level count), recorded in the log
  alongside every measurement: per-event cost depends on book size, so runs at different
  depths are not comparable.
- **Percentiles reported per event type** (new / cancel / modify), because pooling
  heterogeneous operations smears the distribution and hides which one regressed.
- **Deterministic, non-bursty input** — the script drives the engine directly with no
  artificial pacing, since we measure the engine, not a network path.

### Defeating the optimiser without perturbing the measurement

Per-event `DoNotOptimize` inserts an inline-asm barrier into a 64 ns budget, which is
itself a perturbation. Instead the harness accumulates a **checksum over every `OutEvent`**
and prints it once at the end. Because the checksum is observably printed, no work feeding
it can be eliminated — and an unexpected checksum is a free correctness signal, catching
both compiler misbehaviour and genuine logic regressions.

### Percentiles

Samples are written into a pre-allocated `std::vector<std::uint32_t>` (tick counts, not
nanoseconds) by index increment — no growth, no allocation, no I/O inside the loop.
Percentiles are computed after the run. At least 10^5 samples per configuration so the
P99.9 bucket has a meaningful population.

### Statistical rigour

An improvement counts only if it survives:

- **At least 9 repetitions** per configuration — google/benchmark's own
  `UTEST_OPTIMAL_REPETITIONS` constant, described in their source as "lowest reasonable
  number, more is better."
- **A Mann-Whitney U test** between baseline and candidate, alpha 0.05 — the same test
  `tools/compare.py` applies. The samples must be named explicitly and are not
  interchangeable: for throughput, the per-repetition **batch means**; for the tail, the
  per-repetition **P99 (or P99.9) values**. One test per metric.
- **An empirically established noise floor**: the identical binary run 20-30 times, with
  the observed spread in P50/P99/P99.9 recorded. A change smaller than that spread is not
  a result.
- Runs are discarded if `thermalState` rose above nominal.

### kperf counter reader

Reads cycles, instructions, and L1D cache misses via the private frameworks. Requires
`sudo`. **Must degrade gracefully**: when not running as root, or when the frameworks or
the per-chip event database are unavailable, the harness reports timing only and says so
explicitly rather than failing or silently reporting zeros. Counter data is a diagnostic
aid; no optimisation decision may depend on it being present.

---

## The optimisation programme

Each item is gated: measure, change, verify against the oracle and the full test suite,
measure again, then **keep or revert**. Reverting is a normal outcome, as the hash mixing
already demonstrated.

Ordering below reflects current expectation, but **Phase 2's profile reorders it.** The
list is hypotheses, not evidence.

### 1. Order record layout

Today `Order` is 40 bytes with 7 bytes of padding, and field reordering alone does not
help — measured, 40 bytes either way. The gain comes from a different route:

```
today:  id(8) side(1) pad(7) price(8) qty(8) seq(8)             = 40 bytes
target: price(4) qty(4) seq(4) next(4) prev(4) side(1) pad(3)   = 24 bytes
```

32-bit fields plus 32-bit intrusive links replace 64-bit ones. Price and side are **kept**,
matching every real implementation studied.

**The `id` field depends on a storage decision, and the two options are mutually
exclusive. This must be chosen before implementation.**

- **Design A — storage indexed directly by `id - base`.** The order's identity *is* its
  index, so `id` is not stored and 24 bytes is reachable. The cost: ids are monotonic and
  never reused, so slots are never reclaimed and memory grows with every id ever issued
  rather than with live orders (about 5.8 MB across a 400000-event script; unbounded over
  a long session without periodic rebasing).
- **Design B — a slot map `id -> pool index`, with a pooled free list.** Memory tracks
  *live* orders and stays small. But the pool index does not encode the id, and trade
  reports must name the resting order, so **`id` must be stored** and the record lands at
  28-32 bytes.

"24 bytes" and "reuse freed slots" cannot both be had.

**DECIDED 2026-08-06: Design B**, on measured evidence rather than the size headline.

Measured on the 400000-event benchmark: 232981 ids issued, 27626 peak resting, a ratio of
8.4 to 1. That ratio is what settles it. Design A's array would be only **11.9% occupied**,
so a 128-byte cache line holding 5 records would carry about 0.6 live ones; the live working
set would span roughly 3.5 MB of cache lines. Design B's pool is 100% live and spans about
0.77 MB - **5.7x fewer lines touched for the same data** - at the cost of one extra
indirection and 4 bytes per record. Design B is also flat in memory over a long session
(1.63 MB vs 5.33 MB and growing).

The 24-byte target was partly cosmetic: sparse-and-smaller loses to dense-and-slightly-bigger.

**The true baseline is worse than `sizeof(Order)` suggests.** Measured by intercepting
`operator new`: a `std::list<Order>` node requests **56 bytes** (40 of `Order` plus two
8-byte links the container adds invisibly) and the allocator returns a **64-byte** block.
So the real per-resting-order cost today is 64 bytes, and Design B's 28-byte pooled record
is a **2.29x** reduction - and today's line holds 2 scattered orders where Design B's holds
4 dense ones.

**Second open decision:** narrowing `Quantity` to 32 bits caps it near 2.1 billion and is
what reaches 24 bytes under Design A. Keeping 64-bit quantity adds 4 bytes. 32 bytes still
fits four records per 128-byte cache line; 24 fits five. The cap is irrelevant at
futures scale, but capping is a **behaviour change**: oversized quantities must be
rejected with a stated `RejectReason`, and `NaiveEngine` must adopt the identical rule or
equivalence will fail for the wrong reason.

**Consequence:** the public API keeps taking and returning `Order`, while storage uses a
compact internal record. `front_at` currently returns `const Order*` pointing into storage;
that becomes either a small by-value view or a different accessor. This internal-versus-API
type split is real work and is the reason this item was deferred until the oracle existed.

### 2. Object pool and intrusive list

Every resting order currently costs one `operator new` (a `std::list` node) and every
cancel one `free`. Allocation is the single most likely source of *tail* latency, because
allocator slow paths are exactly the kind of rare multi-microsecond stall that dominates
P99.9.

Hand-rolled design: one pre-allocated `std::vector` of records, a free list threaded
through unused slots as 32-bit indices, and levels as intrusive doubly-linked lists using
those same indices. Zero allocation on the hot path after startup.

This mirrors real production practice — Serum/OpenBook uses exactly this shape (`u32`
index free list through a flat slab, audited, in production).

**Then A/B against `plf::list`**, which provides the same iterator-stability contract with
block allocation, to calibrate the hand-written version.

### 3. Slot map replacing the id hash map

Order ids are monotonic, so `index_` can become a dense vector indexed by `id - base`.
Lookup becomes one array subscript: no hashing, no bucket chain, no rehash stall. This also
removes a tail-latency source, since `unordered_map` rehashing is a periodic spike.

Real implementations use a **sentinel** (an obviously-invalid location) to mark absent
entries rather than generation counters, because ids are never reused within a session.
Generation counters solve a problem this system does not have.

**Design points:** what `base` is and how it advances; growth policy when ids outrun the
vector; and what happens to a lookup for an id below `base` or beyond the end.

### 4. Array-indexed price ladder

Replace `std::map<Price, Level>` with a flat array indexed by `(price - minPrice) / tick`,
plus a bitmap of occupied levels and a cached best-price index. When the inside level
empties, `std::countr_zero` / `countl_zero` over the bitmap finds the next occupied level;
the cached scalar serves every other access in O(1). This exact hybrid is confirmed in
working code.

**This changes semantics, and that matters for the oracle.** A bounded ladder must decide
what happens to a price outside its range. Two options:

- **Reject out-of-range prices** with a new `RejectReason`. Simplest and appropriate given
  a narrow, known futures-like range — but it is a behaviour change, so `NaiveEngine` must
  adopt the same rule or equivalence will fail for the wrong reason.
- **Two-level structure**: array near the touch, `std::map` for the long tail. No semantic
  change, more complexity. This is what `liquibook` does for its depth layer.

The decision is explicit and must be made before implementation, not discovered during it.

### 5. Lookup amplification

The match loop performs four lookups per fill for a node it already holds a pointer to:
`best_ask()`, `front_at()`, then `cancel()` or `modify()` each re-finding by id. Passing a
handle instead of a key collapses these. Costs nothing and removes work rather than trading
one cost for another — the only item on this list that is unambiguously free.

### 6. Micro-tuning

Only after the structural work, and only where the profile points: `[[likely]]`/
`[[unlikely]]` on genuinely biased branches, `__builtin_prefetch` where a pointer-chase is
confirmed by counters, and instrumentation PGO (`-fprofile-generate` -> `llvm-profdata
merge` -> `-fprofile-use`; sampling PGO is unavailable on macOS as it requires Linux perf).

Branch hints deserve scepticism: published HFT work argues static hints are unreliable for
data-dependent branches, which describes most of a matching engine's control flow.

---

## Verification gate

No optimisation is accepted unless, after the change:

1. All existing tests pass in all three build configurations.
2. The fast engine and `NaiveEngine` agree on every golden script and all random scripts.
3. `validate()` holds after every event of every property run.
4. `leaks(1)` reports zero on a non-sanitised build.
5. The measured improvement exceeds the noise floor and survives a Mann-Whitney U test.

If (1)-(4) fail the change is wrong. If (5) fails the change is pointless and is reverted.

---

## Benchmark log

A committed, append-only `docs/benchmarks/log.md` records every measurement: date, commit,
configuration, P50 / P99 / P99.9 per event type, throughput, counter data when available,
and the verdict (kept / reverted) with the reason. The log is the milestone's primary
artefact — a record of what actually helped, including the things that did not.

---

## Success criteria

1. A benchmark harness exists that reports throughput plus P50 / P99 / P99.9 per event
   type, is deterministic across runs, and emits machine-readable output.
2. A kperf counter reader exists and degrades gracefully without root.
3. A baseline is recorded, with an empirically measured noise floor.
4. Each optimisation in the programme has a log entry with before and after numbers and an
   explicit keep-or-revert verdict.
5. Every accepted change passes the full verification gate.
6. A final report compares baseline to final, states the total improvement in tail latency,
   and lists what was tried and rejected.
7. **The user wrote the optimisations** and can explain, for each, both the mechanism and
   the measured effect.
