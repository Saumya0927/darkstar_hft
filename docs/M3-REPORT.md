# Milestone 3 — performance engineering, final report

Written 2026-08-13. Apple M1, Homebrew Clang 22.1.4 + libc++, macOS 26.5.
Full measurement history in `docs/benchmarks/log.md`; this is the summary and the verdict.

---

## 1. Result

400000 generated events, 50000 warm-up, seed 1, Release preset.

| | M3 baseline (`1665389`) | now | change |
|---|---|---|---|
| ns/event | 79.7 | **25.8** | **-68%** |
| p99 | 291.7 | **83.3** | **-71%** |
| p99.9 | 375.0 | **125.0** | **-67%** |
| worst case | ~115000 | see caveat below | |
| events > 1 us (of 350000) | 13 | 0-7 | |
| events > 10 us | 5 | 0-3 | |

Per event type at the end, p99 / p99.9 ns:

```
new         83.3 / 125.0   (210277 samples)
cancel      41.7 /  83.3   (87451 samples)
modify      83.3 / 125.0   (52272 samples)
```

Hardware counters, last measured under `sudo` **before** the T10 devirtualization:

```
cycles/event           87.09
instructions/event    267.87
IPC                     3.08     (up from 2.32 after T8)
branch misses/event    1.7820
L1D miss ld/event      4.3804
L1D miss st/event      0.8292
implied clock           3.13 GHz  (M1 P-core max 3.20)
```

**The worst-case figure needs a caveat, and it corrects an earlier claim in this project.**
Earlier entries record "worst case 4583 ns, events >10us: 0" as an achievement. Six runs of
one unmodified binary produced worst cases of 15000, 9083, 17917, 5667, 34167 and 4708 ns,
with over-10us counts of 3, 0, 1, 0, 1, 0. **`max_ns` is a single extreme sample and does not
have a stable value on this machine.** The honest statement is that the M3 baseline was
~115 us and nothing now approaches it; the specific number should not be quoted.

Throughout: **172 tests green in all three build configurations, five golden files
byte-identical, benchmark checksum `dc015ae88f2b6dd0` unchanged from the baseline to today,
zero leaks.** No optimisation in this milestone changed one character of engine output.

---

## 2. How everything was measured

The instrument was built first, by a different party than the one making the changes, and
deliberately so.

- **Timer granularity on this machine is 41.667 ns** (`mach_timebase_info` = 125/3). Every
  userspace clock is the same 24 MHz counter. **A per-event median is not a representable
  number.** Medians come only from batch timing; the tail is measurable because stalls are
  microseconds.
- **Same-session A/B**, both binaries built and present, runs alternating. Never a comparison
  against a number logged on another day.
- **8 invocations per side**, sample = that invocation's batch median.
- **Exact Mann-Whitney U**, two-tailed, alpha 0.05. U and p reported; whether the groups
  overlap or separate reported separately.
- **PMU counters via kperf/kperfdata**, requiring root. Under `sudo` the machine is also
  markedly quieter.
- When the machine was too noisy to resolve a difference, **the generated code was compared
  instead**: 38 functions disassembled before and after, 35 byte-identical, the 3 that
  differed explained down to a single `__LINE__` immediate.

### The noise floor is not a constant, and treating it as one nearly cost a real result

M3 opened by measuring a **6.08%** noise floor and every entry judged against it. At the end
of the milestone, A/B-ing two **byte-identical binaries** gave **+0.1%, p=0.279** — the real
floor is now far below 6%, because the engine got three times faster and a fixed percentage
of 80 ns is not a fixed percentage of 26 ns.

The T10 devirtualization win was **-4.8%**. Judged against the inherited floor it would have
been discarded as noise. **Re-measure the floor whenever the baseline moves.**

---

## 3. What was done

```
T1  benchmark harness                DONE   assistant
T2  kperf PMU counter reader         DONE   assistant
T3  baseline + noise floor           DONE   assistant
T4  lookup amplification             DONE   user       zero gain, kept for clarity
T5  order record layout              DONE   user   \   done together, -15.4%
T6  object pool + intrusive list     DONE   user   /
T7  A/B against plf::list            SKIPPED           reasons in section 5
T8  open-addressed order-id map      DONE   user       -37.9%, the tail stall solved
T9  hybrid price ladder              DONE   user       -35.9%, branch misses -40%
T10 micro-tuning                     DONE   assistant  -4.8%, two of three tools rejected
T11 this report                      DONE   assistant
```

The engine has three data structures. Every task attacked one of them.

```
THE PRICE LEVELS          THE ORDER RECORDS         THE ID INDEX
"what is the best?"       "who is first in line?"   "where is order 4829?"

PriceLadder<Side,Level>   std::vector<Slot> pool_   ankerl::unordered_dense
  array indexed by tick     28-byte records           open addressing, robin hood,
  + occupancy bitmap        + free list               backward-shift deletion,
  + cached best index       + intrusive levels        reserved to capacity
  + std::map tail           (index links)
      T9                        T5 + T6                     T8
```

---

## 4. What the hypothesis list predicted versus what the profile said

This is the part worth keeping. **Four of the milestone's predictions were wrong**, and the
wins came from falsifying them.

| # | prediction | outcome |
|---|---|---|
| 1 | Mixing the hash will speed up the id map | **7.7% slower.** libc++ uses prime bucket counts, where identity hashing of sequential ids is already optimal. Reverted. It became correct again in T8, once the table was power-of-two open-addressed and slot selection moved to the low bits. The idea was never wrong, only premature. |
| 2 | `Order` carries redundant `price`/`side`; trim it | Withdrawn. Research found **every** real order book keeps both. |
| 3 | T4: removing tree walks per fill will pay | **Exactly zero.** At ~103 levels the tree was cache-resident, so the "expensive" pointer chases were L1 hits. |
| 4 | T5/T6: removing per-order `malloc` will fix the **tail** | Delivered a 15.4% **throughput** win and the worst case **did not move at all**. |
| 5 | The 180 us stall is `pool_` vector growth | **Falsified.** `pool_.reserve` moved the median max from 181083 to 185500 ns — worse, fully overlapping. |
| 6 | The 180 us stall is OS preemption | **Falsified.** Implied clock 3.09 GHz against a 3.20 GHz maximum: the thread held the core ~97% of the time. |
| 7 | — | **The actual cause was `std::unordered_map` rehashing.** `index_.reserve()` alone: -19.5% throughput, -90.3% worst case. The stall was never in the order records; it was one container away. |

T4 and T9 looked contradictory — the same idea, zero result then -36%. They are not. T4
removed a price-map lookup per **fill**, and fills are a minority of events (the book grows
from 19338 to 151874 in quantity, so most orders rest rather than trade); roughly 2 ns/event
against a then-80 ns baseline, comfortably inside the noise floor. T9 removed price-map cost
from **every** event. Different denominators.

The one prediction that held precisely was T9's: the spec argued *before any code existed*
that a red-black tree walk is the unpredictable-branch pattern, and that T9 might therefore
deliver part of what T10 was going to chase. Branch misses fell **39.7%**, and cycles/event
fell 36.6% against a wall-clock fall of 35.9% — two independent instruments agreeing.

---

## 5. What was tried and rejected

| | why rejected |
|---|---|
| **Murmur hash mixing** (pre-M3) | 7.7% slower against libc++'s prime buckets. Reverted, then adopted in T8 when the table type changed. |
| **Trimming `Order`** | Real order books keep `price` and `side` on the record. Withdrawn before implementation. |
| **T7: `plf::list`** | Cannot provide 28-byte records or index links; it wraps `Order` in a 56-byte node. Testing it meant rebuilding the storage layer to evaluate a design that fails on inspection. The lesson T7 existed to teach — "do not assume your hand-rolled structure beats a library" — was learned in T8 instead, where the library won decisively and was adopted. |
| **`for_each_from_best`** | Specced, then deliberately not built. Only `depth_into` needs sorted order, it is not hot, and it required directional bitmap walking with `1ULL << 64` UB edge cases. `depth_into` collects and sorts. One traversal method instead of two, and a class of bug that now cannot exist. |
| **`[[likely]]`/`[[unlikely]]`** | The remaining branches are data-dependent: does this order cross, did the level empty, full or partial fill, buy or sell. There is no biased branch to annotate, and published HFT work finds static hints unreliable for exactly this case. |
| **`__builtin_prefetch`** | **Deferred, not dead.** 4.38 L1D load misses per event is roughly one per structure touched — with ~10^5 live orders, `pool_` and `index_` are several MB against a 128 KB L1d. Prefetching cannot reduce that count, only hide latency, and the latency is already hidden at >=2.2x overlap (proved by arithmetic: 5.21 misses at 12 cycles each would leave 24.6 cycles to retire 267.9 instructions, an IPC of 10.9 on a core that retires at most 8). Where it could pay — pipelining the next event's hash bucket across the serial `id -> bucket -> slot index -> pool_[idx]` chain — needs a batch API, which is rung-3 work. |
| **`-mcpu=native`** | Byte-identical binary. The engine is scalar integer code; fp16, dotprod and crypto are never used, and `countr_zero` lowers to baseline ARMv8 `rbit`/`clz`. |
| **`-fwhole-program-vtables` alone** | Also byte-identical. Under Apple `ld64`, default symbol visibility forces the compiler to assume a class could be overridden outside the link unit. It works only when paired with hidden visibility — and a fourth experimental arm confirmed hidden visibility *alone* changes nothing, so the win is genuinely the devirtualization. |
| **Instrumentation PGO** | **Works — the largest single win at -9.2%, and deliberately not adopted.** The profile was trained on the benchmark and then measured on the same benchmark, which is overfitting by construction. It tells you how fast the engine runs the synthetic generator. The mechanism stays available (`DHFT_PGO=generate|use`) for retraining on real traffic. |

---

## 6. The discipline, and what it caught

Two rules did most of the work.

**A check that reports success is worthless until you have watched it fail.** This bit six
times during M3: `timeout` does not exist on macOS so a leak sweep silently did nothing; a
grep filter matched only `src/` while the errors were in a header; `&& echo CLEAN` chained to
`head`, which always succeeds, printed CLEAN over a wall of compiler errors; clang's default
`-ferror-limit=20` truncated before reaching newly written code; a `-x c++` syntax check on a
header reported a bogus error and checked nothing; and an overflow guard was added while the
constructor was never changed to call it — compiled clean, `-Werror` satisfied, every test
green, the undefined behaviour still live.

That rule is why every checker in this repo has been deliberately broken and watched to fail.
The `PriceLadder` invariants added at the end of the milestone went through five mutations;
one of them, removing a `used != 0` guard, was caught by exactly one test and nothing else.

**Verification is the thing that made speed safe.** The engine's entire storage layer was
replaced three times — records, id index, price levels — and the five golden files never
moved by one character. That is the only reason any of these changes could be trusted.

The final review found the gap in that armour: `GenConfig` defaults to prices 95-105, so
every equivalence, property and golden test stayed inside the ladder's `[0, 16383]` band and
**the tail map had no coverage through the engine at all** — precisely where the sharpest
hazard lives, a `Level*` held across a `std::map::erase`. Five tests closed it, and they were
proven to reach the path by injecting that use-after-free and watching ASan trap it while the
in-band tests carried on passing.

---

## 7. State at the end of M3

```
throughput   25.8 ns/event      p99 83.3 ns      p99.9 125.0 ns
tests        172, green in debug (ASan+UBSan), relassert, release
goldens      5 pairs, byte-identical since before M3
checksum     dc015ae88f2b6dd0, unchanged from the M3 baseline
leaks        zero, verified with leaks(1) on a non-sanitised build
```

Release builds with `-O3`, ThinLTO, whole-program devirtualization and `-dead_strip`.

### What is left undone, honestly

- **The counter figures in section 1 predate the T10 devirtualization.** They need one
  `sudo ./build-release/apps/bench --reps 9` to be current.
- **PGO is available and unadopted**, pending a profile from something other than the
  benchmark it is measured on.
- **Prefetch is deferred to rung 3**, where a feed delivers batches and the cross-event
  pipelining that the miss data justifies becomes expressible.
- **`Sink::on_event` is still virtual in the source.** The build flag removes the indirect
  call; the *design* question raised by *C++ Software Design* — type erasure or a template
  parameter — was never answered on its merits, only worked around. That is a fair trade for
  now: the flag costs nothing and changes no code.

### The next rung

Market-data ingestion. The engine is fast enough that another 10% is worth less than pointing
it at a real feed, and the `Feed`/`Sink` seams were designed for exactly that.

One constraint is already known and must survive the transition: **real exchange order ids
are sparse 64-bit.** Nasdaq ITCH order reference numbers are 64-bit and an L3 book can hold
millions of orders; CME assigns sequentially across the whole venue, so per instrument the
ids are full of gaps. Any design that assumes dense counting ids will not survive contact
with a feed — which is why T8 rejected a flat array indexed by `id - base`.

The framing that matters for everything after this: **if this connects to a live exchange,
the exchange runs the matching engine, not you.** `OrderBook` becomes a book builder
reconstructing someone else's book from a feed, and `MatchingEngine` becomes the fill
simulator for backtesting. Both are needed. Nothing built so far is wasted.
