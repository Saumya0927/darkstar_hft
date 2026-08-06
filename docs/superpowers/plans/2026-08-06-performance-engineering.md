# Milestone 3 — Performance Engineering: Learning Curriculum

> **COACH MODE.** The user writes every optimisation in Phase 3. The assistant builds the
> measurement infrastructure (Phase 1), establishes the baseline (Phase 2), writes the
> final report (Phase 4), and reviews. For each user task the assistant supplies the
> concept, the mechanism, the interface, and the measurement gate — never the
> implementation.

**Spec:** `docs/superpowers/specs/2026-08-06-performance-engineering-design.md`

**Goal:** reduce tail latency, learning the low-level C++ that makes it possible, with
every change proven correct by the oracle and proven beneficial by measurement.

## How each Phase-3 lesson runs

Identical shape every time, because the discipline *is* the lesson:

1. **Concept** — what the technique is, why it should help, and what it costs.
2. **Measure before** — record the current numbers in the benchmark log.
3. **You implement** — assistant gives the interface and the mental model, not the code.
4. **Verify** — 113 tests in all three configs, oracle agreement, `validate()`, zero leaks.
5. **Measure after** — same script, same steady-state depth, 9+ repetitions.
6. **Keep or revert** — and write the log entry either way, including the reason.

Step 6 is not a formality. Two changes have already been reverted in this project for
failing it, and the log of what *didn't* work is as valuable as the code that did.

## Global constraints (inherited, still binding)

- Integer tick prices, strong types, `enum class`, brace-init.
- Hot path `noexcept`; `std::expected` for failures; no exceptions in matching.
- No naked `new`/`delete` **outside the pool** — the pool is the single deliberate
  exception, and it owns its storage by RAII.
- Namespace `dhft`; new module uses `dhft::bench`.
- No emojis. No assistant comments in the user's files.
- A median may only be quoted from batch timing. Per-event `p50` is not a valid number.

---

## Phase 1 — Measurement infrastructure

## Task 1: Benchmark harness   [ASSISTANT]

**Deliverable:** a `bench/` module (`dhft_bench`) plus `apps/bench.cpp`:

- fixed replay script, generated once with a known seed and **committed**, so every build
  sees identical input
- **batch mode** — time N events, divide; the only source of a valid median/throughput
- **per-event mode** — pre-allocated `std::vector<std::uint32_t>` of tick counts, filled by
  index increment, percentiles computed after the run
- per-event-type histograms (new / cancel / modify) reported separately
- **checksum over every `OutEvent`**, printed once, so the optimiser cannot delete work
  whose results are unobserved — and so an unexpected checksum flags a behaviour change
- warm-up to a **stated** steady-state book depth, prefix discarded, depth logged
- `thermalState` polled before and after; runs above nominal are discarded
- `QOS_CLASS_USER_INTERACTIVE` set on the benchmark thread (a hint toward P-cores, not a
  guarantee — macOS has no CPU pinning)
- JSON output suitable for `compare.py`

**Done when:** two consecutive runs of the same binary agree within the measured noise
floor, and the checksum is stable across builds.

---

## Task 2: kperf counter reader   [ASSISTANT]

**Deliverable:** `dhft::bench::Counters` reading cycles, instructions, and L1D misses via
the private `kperf`/`kperfdata` frameworks, resolving events from `/usr/share/kpep/*.plist`.

**Why it is load-bearing, not decoration:** wall-clock cannot distinguish an engine stall
from the OS descheduling the thread — and the timer itself was measured stalling 18 us.
Per-thread PMU counters accumulate only while the thread runs, so a cycle count excludes
preemption. Attributing the tail honestly requires them.

**Must degrade gracefully:** without root, or if the frameworks or the chip's event
database are missing, report timing only and say so plainly. Never fail, never report
zeros as if they were data. No optimisation decision may depend on counters being present.

**Done when:** running as root reports plausible cycles/instructions for a known workload,
and running as a normal user prints a clear "counters unavailable" notice and still
produces timings.

---

## Phase 2 — Baseline

## Task 3: Baseline, noise floor, and first profile   [ASSISTANT, user reviews]

**Deliverable:**
- the identical binary run 20-30 times to establish the **empirical noise floor** — the
  run-to-run spread in throughput and in P99/P99.9. Anything smaller than this is not a
  result, and knowing the number prevents chasing ghosts.
- baseline entry in `docs/benchmarks/log.md`: commit, config, book depth, throughput,
  P99/P99.9 per event type, counters if available
- a **profile**: `xcrun xctrace record --template 'Time Profiler'` over millions of
  iterations (Time Profiler samples at 1 ms, so a 64 ns event is only visible in aggregate)

**This step reorders Phase 3.** The task order below is a hypothesis. The profile is
evidence, and evidence wins.

---

## Phase 3 — The optimisations   [USER]

## Task 4: Lookup amplification

**Start here** — it removes work rather than trading one cost for another, and it changes
no data structure, so it isolates cleanly.

**Concept.** Each fill currently performs four lookups for a node the engine already holds
a pointer to: `best_ask()`, `front_at()`, then `cancel()` or `modify()` each re-finding the
order *by id* from scratch. The public API takes keys because outside callers only have
keys. Internal code has a handle and is throwing it away.

**C++ you learn:** the cost of an abstraction boundary; designing an internal API distinct
from the public one; passing handles rather than keys; why `const`-correctness and
lifetime reasoning get harder once you hold a handle across a mutation.

**Gate:** oracle agreement is the real test here — this touches the match loop directly.

---

## Task 5: Order record layout

**Decide first, implement second.** The spec sets out two mutually exclusive designs:
Design A (storage indexed by `id - base`; `id` implicit; 24 bytes; slots never reclaimed)
versus Design B (slot map plus pooled free list; `id` stored; 28-32 bytes; memory tracks
live orders). Pick one, write down why, and size the benchmark accordingly.

Also decide whether `Quantity` narrows to 32 bits. If it does, oversized quantities must be
**rejected** with a stated reason and `NaiveEngine` must adopt the identical rule.

**C++ you learn:** struct layout and alignment; why field reordering did *not* help here
(measured: 40 bytes either way) but narrowing does; fixed-width types; narrowing
conversions and where to check them; `static_assert(sizeof(T) == N)` to pin layout so it
cannot silently regress; and the **internal-versus-API type split** — the public interface
keeps taking `Order` while storage uses a compact record, which forces `front_at`'s
`const Order*` return to become something else.

**Gate:** `static_assert` on the record size, plus the full verification gate.

---

## Task 6: Object pool and intrusive list

The classic low-level C++ exercise, and the one most likely to move the *tail*: every
resting order currently costs an `operator new`, and allocator slow paths are exactly the
rare multi-microsecond stalls that dominate P99.9.

**Design.** One pre-allocated `std::vector` of records; a free list threaded through unused
slots as 32-bit indices; price levels become intrusive doubly-linked lists using those same
indices. Zero allocation on the hot path after startup. This is the shape real production
code uses — Serum/OpenBook runs an audited `u32`-index free list through a flat slab.

**C++ you learn:** owning storage without `new`/`delete` on the hot path; free lists;
**indices instead of pointers** (half the size, and they survive reallocation while
pointers do not); intrusive containers, where the links live inside the element; RAII for
the pool itself; and object lifetime when slots are reused.

**Gate:** ASan clean, `leaks(1)` zero, oracle agreement — a pool bug is exactly the kind of
silent corruption `validate()` and the reference model exist to catch.

---

## Task 7: A/B against `plf::list`

**Concept.** You have written a pool-backed list. Now find out honestly how it compares to
a mature one. `plf::list` offers the same iterator-stability contract with block
allocation, is header-only and zlib-licensed.

**C++ you learn:** integrating a third-party dependency via CMake `FetchContent`;
benchmarking two implementations under identical conditions; and the discipline of
measuring your own work against the state of the art instead of assuming.

**Outcome is informative either way** — if the library wins, that is worth knowing; if the
hand-rolled version wins because it exploits something specific to this engine, that is
worth knowing too. Record which, and why.

---

## Task 8: Slot map replacing the id hash map

**Concept.** Ids are monotonic, so `index_` can become a dense vector indexed by
`id - base`: one array subscript, no hashing, no bucket chain, and critically **no rehash
stall** — a `std::unordered_map` rehash is a periodic tail spike.

Real implementations use a **sentinel** for absent entries, not generation counters,
because ids are never reused within a session. Generation counters solve a problem this
engine does not have.

**C++ you learn:** recognising when a hash map is the wrong tool entirely; sentinel values
versus optional; growth policy; and bounds discipline for ids below `base` or past the end.

---

## Task 9: Array-indexed price ladder with a bitmap

**Concept.** Replace `std::map<Price, Level>` (a red-black tree, O(log n) with a cache miss
per hop) with a flat array indexed by `(price - minPrice) / tick`, plus a bitmap of
occupied levels and a **cached best-price index**. The cached scalar answers the hot path in
O(1); only when the inside level empties does `std::countr_zero` over the bitmap find the
next occupied level. This exact hybrid is confirmed in working production-grade code.

**Decide first:** every real array ladder is **bounded**. Out-of-range prices are either
rejected with a new `RejectReason` (simple, fits a narrow futures-like range, but a
behaviour change requiring the oracle to match) or fall through to a `std::map` tail
(no semantic change, more complexity — what `liquibook` does).

**C++ you learn:** the `<bit>` header — `std::countr_zero`, `std::countl_zero`,
`std::popcount` — and how one instruction replaces a search; bitmap manipulation; the
cached-value-plus-fallback pattern and the invalidation discipline it demands (a stale
cached best price is a silent wrong answer, which is precisely what `validate()` catches).

---

## Task 10: Micro-tuning

**Only where the profile points**, and only after the structural work.

- `[[likely]]` / `[[unlikely]]` on genuinely biased branches. Treat with scepticism:
  published HFT work argues static hints are unreliable for data-dependent branches, which
  describes most of a matching engine.
- `__builtin_prefetch` where a pointer chase is confirmed by counter data, not suspected.
- **Instrumentation PGO**: `-fprofile-generate` -> `llvm-profdata merge` -> `-fprofile-use`.
  Sampling PGO is unavailable here (it needs Linux `perf`).

**C++ you learn:** what the compiler already does for you, how to tell it something it
cannot know, and — most usefully — how often these produce nothing measurable.

---

## Phase 4 — Report

## Task 11: Final report   [ASSISTANT]

Baseline versus final: throughput, P99/P99.9 per event type, counters where available. An
honest list of what was tried and rejected, with reasons. A short note on what the profile
said versus what the initial hypothesis list predicted.

---

## Self-review

- **Spec coverage:** harness -> T1, counters -> T2, baseline/profile -> T3, the five
  optimisation items -> T4-T9, micro-tuning -> T10, report -> T11. All seven success
  criteria map to a task.
- **Split honoured:** the user writes T4-T10 (every optimisation). The assistant writes
  T1-T3 and T11 (measurement and reporting) — deliberately, so the person making the
  changes is not also the person building the instrument that judges them.
- **Coach adaptation:** implementations for user tasks are withheld by design; each ships a
  concept, a mechanism, an interface, and a measurement gate.
- **Ordering is provisional.** T3's profile reorders T4-T9. This is stated in the spec and
  repeated here because it is the one place this plan is most likely to be wrong.
- **Two decisions must be made before their task starts**, not during: the storage design
  in T5 and the out-of-range price policy in T9. Both change behaviour, and both require
  `NaiveEngine` to adopt the same rule or equivalence fails for the wrong reason.
