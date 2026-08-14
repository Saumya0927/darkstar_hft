# Handover — read this first after a context compaction

Written 2026-08-09. Repo: `/Users/saumyapatel/Dark Star Technologies/Random/CPP_Project`.
Last commit: `83ad1d3`. **Working tree is clean and everything builds and passes.**
**Milestone 3 is COMPLETE.** Read `docs/M3-REPORT.md` for the verdict; this doc is the
operating manual.

This supersedes the previous handover (written 2026-08-06 at `e785b6f`), whose "where we
are" section described a mid-refactor tree that no longer exists.

---

## 0. What to do before touching anything

Do these in order. Do not skip to the code.

1. **Read this document end to end.**
2. **Read `docs/REVIEW_LOOP.md`.** It is a per-change discipline the user demanded
   explicitly and said must never be forgotten. Section 3 below repeats it, but read the
   file.
3. **Read `docs/M3-REPORT.md`** (the milestone verdict), then **`docs/benchmarks/log.md`**
   (894 lines, append-only). It is the primary artefact of
   Milestone 3 and records every measurement including the ones that failed. Four
   hypotheses in it were wrong; knowing which saves you repeating them.
4. **Read the current M3 spec and plan**:
   `docs/superpowers/specs/2026-08-06-performance-engineering-design.md` and
   `docs/superpowers/plans/2026-08-06-performance-engineering.md`.
5. **Read the two newest specs**, which contain corrections to their own earlier drafts:
   `docs/superpowers/specs/2026-08-08-id-map-design.md` and
   `docs/superpowers/specs/2026-08-08-price-ladder-design.md`.
6. **Read the engine**, in this order: `include/dhft/Types.h`, `Events.h`, `OrderBook.h`,
   `PriceLadder.h`, then `src/OrderBook.cpp` and `src/MatchingEngine.cpp`.
7. **Build and run the tests in all three configurations** before believing anything here.
8. **Research the standards** — section 10 lists the books and what has already been looked
   up. Re-verify rather than trusting this summary.

---

## 1. What this project is

`darkstar_hft` — a limit order book plus matching engine in C++26. Two purposes, and the
second one has grown:

- **Learn C++, low-level programming, and OS/hardware behaviour.** The user writes the
  engine code.
- **Build something genuinely production-grade.** The user has stated the end goal is to
  connect live exchange feeds and trade. Quoted: *"i want this to be so good to where this
  can be used in a production system... eventually i want to connect live exachnage feed(s)
  then use this to trade and make money."*

That second point changed a decision partway through M3 — see section 9, T8.

### The seven-rung ladder (from the M1 spec)

```
1. Event-driven skeleton + limit order book        DONE   (M1)
2. FIFO price-time matching engine                 DONE   (M1)
3. Market-data ingestion + historical replay       NOT STARTED  <- the next rung
4. Backtester = matching engine + strategy
5. First strategy
6. Risk layer + kill switch; low-latency hardening
7. Instrument sharding; venue adapters (Lighter / ITCH)
```

**Important framing established during M3:** if this connects to a live exchange, *the
exchange* runs the matching engine, not you. The `OrderBook` becomes a **book builder**
reconstructing someone else's book from a feed; the `MatchingEngine` becomes the **fill
simulator** for backtesting. Both are needed. Nothing built so far is wasted.

### Milestones

- **M1 (done)** — order book + matching engine. 46 tests. User hand-wrote all of it.
- **M2 (done)** — verification harness: invariants, golden files, property tests, a
  brute-force reference model (`NaiveEngine`), an equivalence driver, a delta-debugging
  shrinker.
- **M3 (done)** — performance engineering. T1-T6, T8, T9, T10, T11 complete. T7 skipped with
  reasons. Result: 79.7 -> 25.8 ns/event. See `docs/M3-REPORT.md`.

---

## 2. Working mode — non-negotiable

**COACH MODE.** The user writes the engine code. The assistant teaches: the mental model,
what the piece does, the interface, the syntax, then reviews what they write.

Specific things the user has said, repeatedly:

- *"i need you to guide and teach me for this. like step by step and in detail"*
- *"do not give me the code. i need to learn c++ and get better at it"*
- *"explain it like i have no context on the project and i am new to c++"*
- *"do not be too verbose"*

**They will sometimes ask directly for the code.** When they do, give it — annotated line by
line, and tell them to type rather than paste. They have said they can read C++ but struggle
to write from scratch. When they are stuck, scaffold heavily: skeleton with blanks, English
above each blank, and a worked example with real numbers. Abstract explanation does not land;
concrete numbers do.

**The assistant does infrastructure**: build system, tests, benchmark harness, measurement,
reviews, docs, specs. **The user does the engine.**

**No assistant comments in the user's files.** They asked twice to strip
"useless/paragraph-like" comments. Short one-line WHY comments are acceptable per their
global CLAUDE.md; paragraphs are not. Test files: no comments.

**Never use emojis** — anywhere, including markdown and commit messages. Global rule.

**Commits are authored as the user**, no Claude/Anthropic attribution, no co-author
trailers:
```
git -c user.name="Saumya Patel" -c user.email="saumyapatel27@gmail.com" commit
```

**Do not run `clang-format`** on the user's files. It restyled a whole file once and had to
be reverted.

**The user pushes back well.** When they challenged "the order book grows forever, is that
really best?", that question produced the research that changed T8's whole design. Take
their challenges seriously; they have been right more than once.

---

## 3. The review loop — run for EVERY change

Full text in `docs/REVIEW_LOOP.md`. Summarised:

1. **Implement precisely.** Know what it does at machine level.
2. **Test all three configs:**
   `for p in debug relassert release; do cmake --build --preset $p -j8 && ctest --preset $p; done`
3. **Mutation-test any new checker.** Deliberately break the thing it should catch, confirm
   it fires, restore. *A test that has never failed has not been tested.*
4. **Review the change** — correctness, efficiency, memory, exception safety.
5. **Review the blast radius** — callers, the reference model, goldens, property accounting,
   CMake.
6. **Measure if it touches the hot path.** Never claim a performance effect without numbers.
7. **Leak-check:** `MallocStackLogging=1 leaks --atExit -- ./build-release/<bin>` on a
   NON-sanitised build.
8. **Report honestly**, including reverts.

### The meta-lesson this project keeps re-learning

**A check that reports success is worthless until you have watched it fail.** This has bitten
six times:

- `timeout` does not exist on macOS — a leak sweep silently did nothing
- a grep filter matched only `src/` while the errors were in the header
- `&& echo CLEAN` chained to `head`, which always succeeds — printed CLEAN over a wall of errors
- clang's default `-ferror-limit=20` truncated before reaching newly written code
- a `-x c++` syntax check on a header reported a bogus error and checked nothing real
- **an overflow guard was added and the constructor never called it** — compiled clean,
  `-Werror` satisfied, 164 tests green, undefined behaviour still live

Always gate on exit status. Always re-run *the original failing check* after a fix.

---

## 4. Measured platform facts — do NOT re-derive, do NOT assume otherwise

Apple M1, Homebrew Clang 22.1.4 + libc++, macOS 26.5. All verified on this machine.

- **Timer granularity is 41.667 ns** (`mach_timebase_info` = 125/3). Every userspace clock is
  the same 24 MHz counter. **A median may ONLY come from batch timing. A per-event p50 is
  not a valid number.** The tail IS measurable (stalls are microseconds).
- **A timer read pair costs ~41 ns** and itself occasionally stalls 18 us.
- **Cache**: line 128 B; P-core L1d 128 KB; P-core L2 12 MB shared across 4 P-cores.
  P-core max clock 3.204 GHz.
- **ASan's LeakSanitizer is a NO-OP on arm64 macOS** — verified, missed a deliberate 4 KB
  leak. Use `leaks --atExit` on a non-sanitised build.
- **`timeout` does not exist on macOS.**
- **`leaks --atExit` hangs on gtest death tests** (they fork). Filter them out.
- **Clang stops at 20 errors by default.** Always use `-ferror-limit=0`. The CMake build
  truncates at 20 too.
- **libc++ `std::unordered_map` uses PRIME bucket counts**, so identity hashing of sequential
  ids is optimal there. This is why murmur mixing measured 7.7% *slower* and was reverted —
  and why it became *necessary* once the table became power-of-two open-addressed (T8).
- **`-mcpu=native` not `-march=native`** — 25 target features vs 12.
- **No CPU pinning exists on macOS.** QoS class is a hint only.
- **Valgrind does not run natively on arm64 macOS.**
- **PMU counters require `sudo`** and the user must run them. Working command:
  `sudo ./build-release/apps/bench --reps 9`. Under sudo the machine is also markedly
  quieter — spread drops from ~5% to ~2%.

---

## 5. Architecture

```
include/dhft/  src/          core engine, target `dhft`            <- USER'S CODE
io/                          text I/O, `dhft_io`, ns dhft::io      assistant
reference/                   NaiveEngine oracle, `dhft_reference`  assistant
testkit/                     Generate/Golden/Shrink, `dhft_testkit` assistant
bench/                       Samples/Harness/Counters, `dhft_bench` assistant
apps/                        demo.cpp, bench.cpp
test/                        172 gtest cases + golden/
docs/                        specs, plans, benchmarks/log.md, REVIEW_LOOP.md
```

Data flow: `Feed -> MatchingEngine (owns an OrderBook) -> Sink`. Single-writer, no clock, no
RNG. `Sequence` is the only time source. Determinism is the property everything rests on.

### The engine's three data structures

This is the heart of it. Every M3 task attacked one of them.

```
THE PRICE LEVELS         THE ORDER RECORDS          THE ID INDEX
"what is the best?"      "who is first in line?"    "where is order 4829?"

PriceLadder<Side,Level>  std::vector<Slot> pool_    ankerl::unordered_dense::map
  array indexed by tick    28-byte records            <OrderId, uint32 slot>
  + occupancy bitmap       + free list                open addressing,
  + cached best index      + intrusive levels         robin hood,
  + std::map tail          (index links, not          backward-shift deletion,
                            pointers)                 reserved to capacity
      T9                        T5 + T6                     T8
```

### `OrderBook` members (current)

```cpp
std::vector<Slot> pool_;                              // 28-byte records, static_assert'd
std::uint32_t freeHead_{kNull};                       // free list head
PriceLadder<Side::Buy, Level> bids_;                  // best = highest
PriceLadder<Side::Sell, Level> asks_;                 // best = lowest
ankerl::unordered_dense::map<OrderId, std::uint32_t> index_;
```

Constructor: `explicit OrderBook(std::size_t expectedOrders = 65536,
Price minPrice = Price{0}, Price maxPrice = Price{16383})`. The band default is **measured**:
it covers the unit tests (0-200), the golden scripts (90-105) and the benchmark
(9900-10100). If it did not cover the benchmark, T9 would have measured nothing.

### `PriceLadder<Side Sd, typename LevelT>` (`include/dhft/PriceLadder.h`, 165 lines)

Header-only template. Public: `find` / `insert` / `erase` / `best` / `for_each`. Everything
else private.

- Templated on `LevelT` because `Level` is a **private nested type of `OrderBook`** — a
  separate class cannot name it.
- `Side` is a non-type template parameter, so direction costs nothing at runtime.
- `find` uses an explicit object parameter (`this Self&&`, C++23 deducing this) so one body
  serves both const and non-const. `front_at` is const and needs the const form.
- `best()` is O(1) — it reads a cached index. The **only** scan is in `erase`, and only when
  the best level is the one removed, starting from that level's word (not the far end).
- `for_each` is the only traversal. `for_each_from_best` was specced and **deliberately not
  built**: only `depth_into` needs order, it is not hot, and building it required directional
  bitmap walking with `1ULL << 64` UB edge cases. `depth_into` collects then sorts.

### `detail::PriceLevelBook` concept

Describes a **capability**, not a container shape:
```cpp
concept PriceLevelBook = requires(M& m, Price p) {
    { m.erase(p) } -> std::same_as<void>;
    { m.best() } -> std::same_as<std::optional<Price>>;
};
```
`best()` is what rules out passing `index_` by mistake, which was the concept's purpose. It
was previously `PriceLevelMap` and checked `key_type`/`mapped_type` — `std::map`'s
vocabulary, which `PriceLadder` correctly does not have.

---

## 6. Current numbers

```
                    M3 baseline    now      change
ns/event                   79.7    25.8      -68%
p99                       291.7    83.3      -71%
p99.9                     375.0   125.0      -67%
worst case             ~115,000   varies    see below

cycles/event                        86.30   (pre-devirtualization)
instructions/event                 267.40
IPC                                  3.10
branch misses/event                  1.79
L1D miss ld/event                    4.38
L1D miss st/event                    0.83
```

**The worst case has no stable value.** Six runs of one unmodified binary gave 15000, 9083,
17917, 5667, 34167, 4708 ns. Earlier entries quote "4583 ns, over 10us: 0" as a result; that
was a single lucky draw. The baseline was ~115000 and nothing approaches it now -- state it
that way, not as a number.

**Do not reuse the 6.08% noise floor.** It was measured at the 80 ns baseline and was still
being applied at 26 ns, where it would have discarded T10's real -4.8% win. A/B of two
byte-identical binaries now measures +0.1%, p=0.279. Re-measure the floor when the baseline
moves; the cheapest way is to A/B a build against itself.

Benchmark checksum `dc015ae88f2b6dd0`, unchanged since the M3 baseline.

**172 tests green in all three configs. Golden files byte-identical throughout M3. Zero
leaks.**

---

## 7. M3 task status

```
T1  benchmark harness            DONE   assistant
T2  kperf counter reader         DONE   assistant  (sudo path now verified)
T3  baseline + noise floor       DONE   assistant
T4  lookup amplification         DONE   user   ZERO gain, kept for clarity
T5  order record layout          DONE   user   \  done together, -15.4%
T6  object pool + intrusive list DONE   user   /
T7  A/B against plf::list        SKIPPED       reasons below
T8  open-addressed id map        DONE   user   -37.9%, tail solved
T9  hybrid price ladder          DONE   user   -35.9%, branch misses -40%
T10 micro-tuning                 DONE   assistant  -4.8%, two of three tools rejected
T11 final report                 DONE   assistant  docs/M3-REPORT.md
```

**T7 skipped, and why:** `plf::list` cannot provide 28-byte records or index links — it
wraps `Order` in its own 56-byte node. Running it means rebuilding the storage layer to test
a design that would be rejected on inspection. And the lesson T7 existed to teach ("do not
assume your hand-rolled thing beats a library") was learned in T8, where the library won
decisively and was adopted.

---

## 8. What is planned next

### M3 is finished. See `docs/M3-REPORT.md`.

T10 landed as three build-flag experiments and no engine code:

- **`-mcpu=native` rejected** — byte-identical binary. The engine is scalar integer code, so
  the extra target features are never used.
- **`-fwhole-program-vtables` adopted**, but only together with `-fvisibility=hidden`. Alone
  it is a no-op under Apple `ld64`. Together they remove the three indirect `Sink::on_event`
  calls from `MatchingEngine::process`. -4.8%, p=0.00186. Now on in the release preset.
- **PGO works (-9.2%) and is deliberately NOT adopted** — the profile was trained on the
  benchmark and measured on the same benchmark. `DHFT_PGO=generate|use` and
  `DHFT_PGO_PROFILE` exist for retraining on real traffic.
- **Prefetch deferred, not rejected.** 4.38 L1D load misses/event is about one per structure
  touched, and they are already overlapped >=2.2x. It could pay only by pipelining the next
  event's hash bucket across the serial `id -> bucket -> slot -> pool_` chain, which needs a
  batch API. That is rung-3 work.

### Then: rung 3, market-data ingestion

The engine is fast enough that another 10% is worth less than pointing it at a real feed.
The `Feed`/`Sink` seams were designed for exactly this.

**Known constraint for that work:** real exchange order ids are **sparse 64-bit** (Nasdaq
ITCH order reference numbers; CME assigns sequentially across the whole venue, so per
instrument they are full of gaps). Any design that assumes dense counting ids will not
survive contact with a feed. This is why T8 rejected a flat array indexed by `id - base`.

---

## 9. Every hypothesis that was WRONG — do not repeat these

This is the most valuable section. Four of M3's guesses were wrong, and the wins came from
falsifying them.

1. **Hash mixing (pre-M3).** Added a murmur3 finalizer to `std::hash<OrderId>` on textbook
   reasoning. Measured **7.7% slower**: libc++ uses prime buckets where identity hashing of
   sequential ids is already optimal. Reverted. **It became correct again in T8**, when the
   table became power-of-two open-addressed and slot selection moved to the low bits. The
   idea was never wrong, only premature.

2. **`Order` trimming.** Recommended dropping redundant `price`/`side`. Research found
   **every** real order book keeps both. Withdrawn.

3. **T4, lookup amplification.** Predicted a win from removing tree walks. Delivered
   **zero** — at 103 levels the tree was cache-resident, so the "expensive" pointer chases
   were L1 hits. Kept for clarity only. *(Reconciled in T9: T4 removed a lookup per **fill**,
   and fills are a minority of events. T9 removed one from **every** event and got -36%.)*

4. **T5+T6, allocation.** Predicted a **tail** win from removing per-order `malloc`.
   Delivered a 15.4% throughput win and **the worst case did not move at all.** Logged as
   unexplained.

5. **Pool vector growth.** Top suspect for the remaining 180 us stall. **Falsified** —
   `pool_.reserve` moved median max_ns from 181,083 to 185,500, worse, fully overlapping.

6. **OS preemption.** Second suspect. **Falsified** — implied clock 3.09 GHz against a
   3.20 GHz maximum means the thread held the core ~97% of the time.

7. **The actual cause: `std::unordered_map` rehashing.** `index_.reserve()` alone gave
   -19.5% throughput and -90.3% worst case. The stall was never in the order records; it was
   one container away.

**Also wrong, in the assistant's own specs and fixes** (all corrected, all recorded):
- T8 spec called `detail::wyhash` directly (internal namespace); memory figures were
  arithmetic and 2.5x low; "14 use sites" was 13; CMake integration was asserted, not tested.
- T9 spec had `PriceLadder` holding `Level` directly — impossible, it is private to
  `OrderBook`; missed that the tail map's comparator depends on the side; left the default
  band unspecified when getting it wrong would have made T9 measure nothing.
- Recommended `DHFT_CHECK` on capacity exhaustion. **Wrong** — killing a live engine because
  the market got busy is worse than a rare stall. Real engines reject orders.
- Recommended "never shrink, write the limit down" for an unbounded structure. The user
  challenged it; research showed real engines pre-allocate a fixed maximum.

---

## 10. Standards and books

The user cares about this a lot and asks for research, not recollection. **Re-verify rather
than trusting this summary.**

### Beautiful C++: 30 Core Guidelines (Davidson & Gregory)

A curated subset of the C++ Core Guidelines, in five sections: *Bikeshedding is bad*,
*Don't hurt yourself*, *Stop using that*, *Use this new thing properly*, *Write code well by
default*. A full audit against all 30 was done (commit `8558e98` and the message before it).
Result at the time: 26 of 30 clean, and the violations fixed were:

- **ES.20** always initialize — `Fill` was the only aggregate without member initialisers
- **F.6** declare `noexcept` if it must not throw — `cancel` and `take_from_front`
- **T.10** specify concepts for all template arguments — the seven generic lambdas
- **P.4** statically type safe — `static_cast<Side>` on an unchecked byte; `validate` now
  rejects it

Ones repeatedly relevant: **C.9** minimise member exposure, **C.20/C.21** rule of zero/five,
**C.45** in-class initialisers, **C.46** explicit single-arg constructors, **C.47** member
declaration order, **C.49** prefer initialisation to assignment, **ES.50** don't cast away
const, **F.51** default arguments over overloading, **I.23** keep argument counts low,
**P.10** prefer immutable data, **P.11/I.30** encapsulate messy constructs, **E.6** RAII.

### C++ Software Design (Klaus Iglberger)

10+1 chapters, ~39 numbered guidelines. Core themes: SOLID, DRY, KISS, YAGNI; managing
dependencies and abstractions; **value semantics over reference semantics** (Guideline 22);
`std::variant`/`std::visit` instead of inheritance; type erasure; the Strategy, Command,
Adapter, Observer, Bridge, Prototype, Decorator patterns; Singleton reframed as an
implementation pattern.

**Not yet audited against.** Relevant to this codebase in two specific places:
- The engine already prefers value semantics — POD events, strong types, no inheritance
  except the `Feed`/`Sink` seams. That is aligned by accident rather than by design review.
- **`Sink::on_event` is a virtual call on the hot path.** Type erasure or a template
  parameter would remove it. This has never been measured. It is a real candidate.

### Smart pointers — researched, and the answer is "no change"

The Core Guidelines split this two ways, and it is not "smart is better":
- **R.3 / R.20 / R.21** — smart pointers express **ownership**.
- **R.30 / F.7** — *"For general use, take `T*` or `T&` arguments rather than smart
  pointers."* Raw pointers/references express a **non-owning observer**.

`PriceLadder::find` returns `LevelT*` pointing at a level the ladder owns. A `unique_ptr`
would claim the caller owns it (and try to delete inside a `std::vector`); a `shared_ptr`
would add atomic refcounting for ownership that does not exist. **The codebase has no owning
raw pointers**, and the one place that genuinely owns something (`Counters::Impl`) uses
`std::unique_ptr`. Correct as-is.

### Domain research already done

- Nasdaq ITCH order reference numbers are 64-bit; an L3 book "can contain millions of orders".
- CME assigns OrderID sequentially **across the whole venue** — sparse per instrument.
- Real matching cores keep client-order-id resolution **in the gateway**, not the matching
  core (arXiv 2606.01183).
- CME's matching engine **rejects** orders outside a dynamically-recalculated price band.
- A granted patent describes pre-allocating "a fixed, predetermined maximum number of order
  objects" — capacity limits are normal practice in this domain, not a hack.
- An ITCH book builder found >30% runtime improvement swapping `std::unordered_map` for
  `google::dense_hash_map`.

Sources are cited in the two 2026-08-08 specs.

---

## 11. Commands

```bash
# three configs
cmake --preset debug      # -O0 + ASan/UBSan          (correctness)
cmake --preset relassert  # -O2, no sanitizers        (fast test runs, 20x faster)
cmake --preset release    # -O3 + ThinLTO + devirtualization + dead_strip (measurement)

for p in debug relassert release; do
  cmake --build --preset $p -j8 && ctest --preset $p
done

# syntax check while writing -- ALWAYS use -ferror-limit=0
/opt/homebrew/opt/llvm/bin/clang++ -std=c++2c -fsyntax-only -Wall -Wextra -Wpedantic \
  -Werror -ferror-limit=0 -Iinclude \
  -Ibuild-relassert/_deps/unordered_dense-src/include src/OrderBook.cpp; echo "exit=$?"

# benchmark
./build-release/apps/bench --reps 9
./build-release/apps/bench --reps 9 --json
sudo ./build-release/apps/bench --reps 9        # PMU counters -- USER must run this

# leaks (non-sanitised build only)
MallocStackLogging=1 leaks --atExit -- ./build-release/apps/demo data/scripts/simple.txt

# goldens: regenerate deliberately, then read the diff
DHFT_UPDATE_GOLDEN=1 ./build-relassert/test/golden_test
```

`CMAKE_AR`/`CMAKE_RANLIB` are pinned to `llvm-ar`/`llvm-ranlib` — Apple's `ar` cannot read
ThinLTO bitcode.

Dependency: `ankerl::unordered_dense` v4.9.0, pinned, MIT, via `FetchContent`. Note it is
**no longer header-only on `main`** (`unordered_dense.h` includes a companion `stl.h`);
`FetchContent` supplies both, so this only bites a hand-vendored copy.

---

## 12. The measurement discipline

Every performance claim in this project follows the same protocol. Keep it.

- **Same-session A/B**, not comparison against a logged number. The T5/T6 measurement was
  redone this way after the logged baseline turned out to predate T4.
- **8 invocations per side**, alternating, same tree, Release preset.
- The sample is the **per-invocation batch median**. Never a per-event median — the timer
  cannot resolve one.
- **Exact Mann-Whitney U test**, two-tailed, alpha 0.05. Report U and p.
- State whether the groups **overlap** or are **completely separated**.
- A change below the **6.08% noise floor** is not a result.
- **p99 is quantised to a 41.67 ns tick** and has been perfectly stable across runs, so any
  p99 movement is a real one-tick step.
- **`max_ns` is a single extreme value.** Eight samples of it prove very little; say so.
- When the machine is too noisy to measure a difference, **compare the generated code
  instead**. Done once already: 38 functions disassembled before/after, 35 byte-identical,
  and the 3 that differed explained down to a single `__LINE__` immediate.

---

## 13. Things that will bite you

- **The golden files are the strongest check in the project.** Five script/expected pairs.
  Every M3 change has kept them byte-identical, which is how you know the engine's behaviour
  did not move. If they change, either you broke something or you meant to — and if you
  meant to, `NaiveEngine` almost certainly needs the same rule.
- **`NaiveEngine` must never be "fixed" to match the fast engine.** It is the oracle. If they
  disagree, the fast engine is wrong until proven otherwise.
- **`take_from_front` and `cancel` hold a `Level*` across `m.erase(price)`.** For an in-band
  price that is safe (erase only clears a bit), but for a tail price `std::map::erase`
  destroys the node. The current code reads everything it needs *before* erasing. Do not
  reorder those lines.
- **References into `pool_` die if the pool reallocates.** `alloc_slot` can `push_back`.
  This is why the links are indices, and why `add()` was restructured to hold no map iterator
  across other work.
- **`validate()` is the safety net.** It checks chain integrity, index consistency, FIFO
  sequence ordering, and slot accounting (`live + free == pool size`), which catches both a
  leaked slot and a double free — neither of which any leak detector can see, because the
  vector still owns the memory.
- **`PriceLadder` invariants are NOT yet in `validate()`.** The T9 spec lists five
  (bitmap agreement, cached best correct, no array/tail overlap, tail is out-of-band only,
  level count) and they were not implemented — step 5 of the T9 plan was skipped because the
  ladder's own 21-test suite plus the engine's 164 tests covered the behaviour. **This is a
  known gap.** A stale cached best would be a silent wrong answer.
- The `relassert` preset is 20x faster than `debug` for test runs.
