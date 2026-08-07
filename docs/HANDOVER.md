# Handover — read this first after a context compaction

Written 2026-08-06. Repo: `/Users/saumyapatel/Dark Star Technologies/Random/CPP_Project`.
Last commit: `a2d1241`. **The working tree is mid-refactor and does not compile — that is
expected and deliberate.** See "Where we are exactly" below.

---

## 1. What this project is

`darkstar_hft` — a limit order book plus matching engine in C++26, built as a vehicle for
the user to learn C++, low-level programming, and OS/hardware behaviour. Not a toy: it has
a reference-model oracle, property tests, golden files, a benchmark harness, and PMU
counters.

Three milestones:

- **M1 (done)** — order book + matching engine. 46 tests. The user hand-wrote all of it.
- **M2 (done)** — verification harness: invariants, golden files, property tests, a
  brute-force reference model (`NaiveEngine`), an equivalence driver, and a delta-debugging
  shrinker. 143 tests.
- **M3 (in progress)** — performance engineering. T1-T4 done, T5/T6 in progress.

---

## 2. Working mode — non-negotiable

**COACH MODE.** The user writes the code. The assistant teaches: mental model, what the
piece is for, the interface, the syntax needed, and reviews what they write. Do NOT write
their implementation unprompted.

**The user has explicitly said they can read C++ but struggle to write from scratch.** When
they are stuck, scaffold heavily — describe each line and the syntax it needs — and give
worked code only when they ask directly (they have asked a few times; that is fine, annotate
it heavily and tell them to type rather than paste). Point them at analogous code *they*
already wrote as the template.

**Assistant does infrastructure**: build system, tests, benchmark harness, measurement,
reviews, docs. **User does the engine and the optimisations.**

**No comments in the user's files.** They asked twice to strip "useless/paragraph-like"
comments. Short one-liners explaining WHY are acceptable per their global CLAUDE.md;
paragraphs are not. Test files: no comments at all.

**Communication:** terse, no filler. Full rigour in the work.

---

## 3. The review loop — run for EVERY change

Also in `docs/REVIEW_LOOP.md` and in assistant memory. The user demanded this explicitly
and said it must not be forgotten.

1. Implement precisely; know what it does at machine level.
2. Test all three configs: `for p in debug relassert release; do cmake --build --preset $p -j8 && ctest --preset $p; done`
3. **Mutation-test any new checker** — deliberately break the thing it should catch, confirm
   it fires, restore. A test that has never failed has not been tested.
4. Review the change (correctness, efficiency, memory, exception safety).
5. Review the blast radius (callers, reference model, goldens, property accounting, CMake).
6. Measure if it touches the hot path. Never claim a perf effect without numbers.
7. Leak-check: `MallocStackLogging=1 leaks --atExit -- ./build-release/<bin>` on a
   NON-sanitised build.
8. Report honestly, including reverts.

---

## 4. Measured platform facts — do NOT re-derive, do NOT assume otherwise

All verified on this machine (Apple M1, Homebrew Clang 22.1.4 + libc++, macOS 26.5):

- **Timer granularity is 41.667 ns** (`mach_timebase_info` = 125/3). `steady_clock`,
  `mach_absolute_time`, `clock_gettime_nsec_np`, and raw `CNTVCT_EL0` are all the same
  24 MHz counter. A ~64 ns event reads as 42 or 83, never 64.
  **Consequence: a median may ONLY come from batch timing. A per-event p50 is not a valid
  number and must not appear in the log.** The tail IS measurable (stalls are microseconds).
- **A timer read pair costs ~41 ns** and itself occasionally stalls 18 us. Wall clock cannot
  separate an engine stall from OS preemption — that needs per-thread PMU counters.
- **Cache**: line 128 B; P-core L1d 128 KB; P-core L2 12 MB (shared across 4 P-cores).
- **ASan's LeakSanitizer is a NO-OP on arm64 macOS** — verified, it missed a deliberate 4 KB
  leak. Use `leaks --atExit` on a non-sanitised build. ASan intercepts malloc and hides the
  heap from `leaks`, so the sanitised build cannot be leak-checked.
- **`timeout` does not exist on macOS.** A leak sweep using it silently did nothing.
- **`leaks --atExit` hangs on gtest death tests** (they fork). Filter them out.
- **Clang stops at 20 errors by default** (`-ferror-limit`). During this refactor that hides
  errors in newly written code. **Always use `-ferror-limit=0`.**
- **libc++ `std::unordered_map` uses PRIME bucket counts.** Measured with 100k sequential
  ids: identity hash -> 100000 buckets used, worst chain 1. Murmur-mixed hash -> 63813
  buckets, worst chain 7, and 7.7% slower. **Do not mix the hash while `std::unordered_map`
  is in use.** Ship mixing only together with a power-of-two open-addressing table.
- **`-mcpu=native` not `-march=native`**: measured 25 target features vs 12.
- **No CPU pinning exists on macOS.** QoS class is a hint only.
- **Valgrind does not run natively on arm64 macOS.**
- **A `std::list<Order>` node requests 56 bytes and the allocator returns 64.** So the true
  per-resting-order cost today is **64 bytes**, not `sizeof(Order)` = 40.

---

## 5. Two times general principle was WRONG here — both caught only by measuring

1. **Hash mixing.** Added a murmur3 finalizer to `std::hash<OrderId>` on textbook reasoning
   ("identity hashing is dangerous"). Measured: 7.7% slower and worse bucket distribution,
   because libc++ uses prime buckets where identity hashing of sequential ids is already
   optimal. **Reverted** (commit 7e1165d).
2. **`Order` trimming.** Recommended dropping redundant `price`/`side`. Research found
   **every** real order book keeps both — a fill report needs them inline and a
   `std::list::iterator` cannot name its container. Recommendation withdrawn.

Also: **T4 predicted a speedup and delivered zero** — see section 8.

This is why M3's rule is: measure before and after, and reverting is a normal outcome.

---

## 6. Build and test commands

```bash
# three configs
cmake --preset debug     # -O0 + ASan/UBSan          (correctness)
cmake --preset relassert # -O2, no sanitizers        (fast test runs, 20x faster)
cmake --preset release   # -O2 + ThinLTO + dead_strip (measurement)

cmake --build --preset <p> -j8 && ctest --preset <p>

# fast syntax check while writing (USE -ferror-limit=0)
/opt/homebrew/opt/llvm/bin/clang++ -std=c++2c -fsyntax-only -Wall -Wextra -Werror \
  -ferror-limit=0 -Iinclude src/OrderBook.cpp 2>&1 | grep error:

# benchmark
./build-release/apps/bench --reps 9
./build-release/apps/bench --reps 9 --json
./build-release/apps/bench --reps 9 --min-price 9990 --max-price 10010   # crossing-heavy
sudo ./build-release/apps/bench --reps 9                                 # PMU counters
```

`CMAKE_AR`/`CMAKE_RANLIB` are pinned to `llvm-ar`/`llvm-ranlib` in `CMakePresets.json` —
Apple's `ar` cannot read ThinLTO bitcode and produces a stub archive.

---

## 7. Repo layout

```
include/dhft/   src/          core engine (dhft)          <- USER'S CODE
io/                           text I/O (dhft_io, dhft::io)
reference/                    NaiveEngine oracle (dhft_reference)
testkit/                      generator, golden, shrinker (dhft_testkit)
bench/                        Samples, Harness, Counters (dhft_bench)
apps/                         demo.cpp, bench.cpp
test/                         gtest suites + golden/
docs/                         specs, plans, benchmarks/log.md, REVIEW_LOOP.md
```

Key docs:
- `docs/superpowers/specs/2026-08-06-performance-engineering-design.md` — M3 spec
- `docs/superpowers/plans/2026-08-06-performance-engineering.md` — M3 curriculum, T1-T11
- `docs/benchmarks/log.md` — baseline + every measurement
- `docs/research/2026-08-05-optimization-options.md` — library/tooling research
- `docs/REVIEW_LOOP.md` — the per-change discipline

---

## 8. M3 progress so far

**Baseline (commit 1665389, in `docs/benchmarks/log.md`):**
```
ns/event 79.72 (median of 20 runs)   p99 291.7   p99.9 375.0
noise floor: 6.08% throughput, p99 perfectly stable (quantised to a tick)
book: 103 populated levels, script = seed 1, 400000 events, 50000 warmup, band 9900-10100
```
**Detection threshold: throughput changes under ~6% are invisible.**

- **T1 done** — benchmark harness (`bench/`). Batch timing for throughput, per-event for
  tail, per-event-type histograms, `ChecksumSink` (FNV) to defeat dead-code elimination and
  detect behaviour change, QoS hint, JSON output. Reproducibility across runs: 0.73%.
- **T2 done** — `Counters`, kperf/kperfdata via `dlopen`, degrades gracefully without root.
  Verified unprivileged: both frameworks load, 17 symbols resolve, PMU DB opens, 4 events
  resolve. **The sudo path is UNVERIFIED — the assistant cannot run sudo. The user must run
  `sudo ./build-release/apps/bench --reps 9` and `sudo ./build-release/test/counters_test`.**
- **T3 done** — baseline + noise floor logged.
- **T4 done, committed 635afaf — KEPT but with NO measurable performance gain.**
  Added `OrderBook::take_from_front(side, price, want)` returning
  `std::optional<OrderBook::Fill>`, replacing `front_at` + `cancel`/`modify` in the match
  loop. Measured before/after at 103 levels, ~20 levels (crossing-heavy), and 958 levels:
  all differences inside noise. **Why the hypothesis failed:** at these sizes the price tree
  is cache-resident (103 nodes approx 7 KB, 958 approx 61 KB, at or under L1), so the
  removed lookups were L1 hits (~1 ns) not misses (~100 ns). Kept for clarity: match loop
  went 17 lines to 4. Full write-up in `docs/benchmarks/log.md`.
  **Signal: since lookups were not the cost, allocation (T6) is now the best-supported
  hypothesis.**

---

## 9. WHERE WE ARE EXACTLY — T5 + T6 in progress

T5 (record layout) and T6 (object pool + intrusive list) are being done **as one change**,
because T5's byte savings only pay off once records are contiguous.

### Decisions already made (recorded in the M3 spec)

- **Design B**, decided on measured evidence. The workload has 232981 ids issued vs 27626
  peak resting = **8.4:1**. Design A (slot index == order id) would leave the array only
  **11.9% occupied**, spanning ~3.5 MB of cache lines; Design B's pool is 100% live and
  spans ~0.77 MB — **5.7x fewer lines** — at the cost of one extra indirection and 4 bytes
  per record. Design B is also flat in memory over a long session (1.63 MB vs 5.33 MB and
  growing).
- **Narrowing policy: `DHFT_CHECK`**, not a new reject reason. Prices/quantities/ids above
  ~2.1 billion are treated as a programming error and abort. No behaviour change for valid
  input, so `NaiveEngine` needs no matching rule.
- **`Fill` was moved from namespace scope to `OrderBook::Fill`** (public nested), because it
  only means anything relative to `OrderBook`. `MatchingEngine` needed no change since it
  used `auto`.

### What is DONE in the working tree (uncommitted)

`include/dhft/OrderBook.h` — private section fully rewritten:
```cpp
static constexpr std::uint32_t kNull = 0xFFFFFFFFu;
struct Slot {  // 28 bytes, static_assert enforces it
    std::uint32_t id{}; std::int32_t price{}; std::int32_t qty{};
    std::uint32_t seq{}; std::uint32_t next{kNull}; std::uint32_t prev{kNull};
    std::uint8_t side{};
};
static_assert(sizeof(Slot) == 28, "Slot layout regressed");
struct Level { std::uint32_t head{kNull}; std::uint32_t tail{kNull}; };
std::vector<Slot> pool_;
std::uint32_t freeHead_{kNull};
std::map<Price, Level, std::greater<>> bids_;
std::map<Price, Level> asks_;
std::unordered_map<OrderId, std::uint32_t> index_;   // id -> slot index
[[nodiscard]] std::uint32_t alloc_slot();
void free_slot(std::uint32_t idx) noexcept;
void link_back(Level& level, std::uint32_t idx) noexcept;
void unlink(Level& level, std::uint32_t idx) noexcept;
```
`using Level = std::list<Order>;` and `struct Location` are DELETED. `#include <list>`
removed.

`src/OrderBook.cpp` — at the bottom of the file:
- `free_slot` is **written and correct**:
  ```cpp
  void OrderBook::free_slot(std::uint32_t idx) noexcept {
      pool_[idx].next = freeHead_;
      freeHead_ = idx;
  }
  ```
- `alloc_slot` is written but has **THREE TYPOS the user must fix** (they were told):
  - line 292: `pool[idx].next` -> `pool_[idx].next`
  - line 293: `pool_.[idx] = Slot{};` -> `pool_[idx] = Slot{};`
  - line 299: `pool_size()` -> `pool_.size()`

### THE IMMEDIATE NEXT STEP

The user fixes those three typos, then runs the `-ferror-limit=0` syntax check. After that,
the remaining errors are the work queue (currently 23 errors, 20 of them from the functions
below).

### Remaining work queue — pieces 3 to 9

Every one of these still assumes `Level` is a `std::list<Order>`:

| piece | function(s) | line(s) | what it needs |
|---|---|---|---|
| 3 | `link_back`, `unlink` | not written | append to / remove from an intrusive chain by index |
| 4 | `add` | ~21, 29 | alloc a slot, fill it (with `DHFT_CHECK` narrowing), `link_back`, index it |
| 5 | `cancel` | ~93 | look up index, `unlink`, `free_slot`, erase index entry, erase empty level |
| 6 | `take_from_front` | ~260-261 | walk to `level.head`, fill, unlink+free or reduce qty |
| 7 | `modify` | ~121 | decrease in place; increase = cancel + re-add with `newSeq` |
| 8 | `front_at`, `depth_into`, `total_quantity`, `contains` | ~142-149, 68, 158 | walk the chain by index instead of iterating a list |
| 9 | `validate` | ~197-212 | rewritten invariants (see below) |

**`front_at` signature must change.** It currently returns `const Order*` pointing into a
`std::list` node. The pool holds `Slot`, not `Order`. Change it to
`std::optional<Order>` returned by value, reconstructed from the slot. `orderbook_test.cpp`
uses it (`ASSERT_NE(moved, nullptr)` becomes `ASSERT_TRUE(moved)`; `->id.v` still works on
an optional).

**`validate` needs new invariants** on top of the existing six:
- intrusive chain integrity: `prev`/`next` consistent both ways, `tail` reachable from
  `head`, no cycles, `head`/`tail` sentinels correct
- free list does not overlap live slots
- `index_.size()` equals the number of slots reachable through all level chains

### Then

- Rebuild all three configs, run all 143 tests, run the equivalence oracle.
- Measure with the benchmark. **Expected if the hypothesis holds:** modest throughput gain,
  and a large p99.9 / worst-case improvement, because nothing on the hot path can call into
  the kernel any more. Today's worst case is ~115000 ns.
- Log the result in `docs/benchmarks/log.md` — including the verdict and, if it fails,
  the reason.
- **Step B (still to come):** replace `std::unordered_map<OrderId, std::uint32_t> index_`
  with a flat `std::vector<std::uint32_t>` indexed by `id - base`. Separate, verifiable step.

---

## 10. Remaining M3 tasks after T5/T6

- **T7** — A/B the hand-rolled pool against `plf::list` (user chose "hand-roll, then
  benchmark against a library").
- **T8** — slot map replacing the id hash map (this is Step B above).
- **T9** — array-indexed price ladder + bitmap + `std::countr_zero`, replacing `std::map`.
  **Open decision, must be made before starting:** an out-of-range price is either rejected
  with a new `RejectReason` (behaviour change — `NaiveEngine` must adopt the identical rule)
  or falls through to a `std::map` tail (no semantic change, more complexity).
- **T10** — micro-tuning: branch hints, prefetch, instrumentation PGO. Sampling PGO is
  unavailable on macOS (needs Linux perf).
- **T11** — final report.

---

## 11. Things to be careful about

- The user's global CLAUDE.md: **never use emojis**, no Claude/Anthropic attribution in
  commits, terse communication.
- Git commits are authored as the user (`-c user.name="Saumya Patel"
  -c user.email="saumyapatel27@gmail.com"`), no co-author trailers.
- Do not run `clang-format` on the user's files — it restyled their whole file once and had
  to be reverted. Their style: 4-space indent inside `namespace dhft`, `Type& name`.
- When a filter/grep reports "clean", verify the tool actually ran and looked at the right
  thing. This has produced three false negatives in this project already.
- The `relassert` preset is 20x faster than `debug` for test runs (0.27s vs 5.3s+).
