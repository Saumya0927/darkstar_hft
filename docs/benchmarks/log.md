# Benchmark log

Append-only. Every measurement, including the ones that led to a revert.

Method: `./build-release/apps/bench --reps 9`, Release preset (`-O2`, ThinLTO, `-Wl,-dead_strip`),
Homebrew Clang 22.1.4 + libc++, Apple M1. Fixed script, seed 1, 400000 events, 50000 discarded
as warm-up, price band 9900-10100. Throughput is batch-timed; percentiles are per-event and
carry ~41 ns of timer overhead. A per-event median is not a valid number on this hardware.

## Noise floor (2026-08-06, commit 1665389)

20 independent invocations of the identical binary:

| metric | median | min | max | spread |
|---|---|---|---|---|
| ns/event | 79.72 | 78.86 | 83.71 | 6.08% |
| p99 | 291.7 | 291.7 | 291.7 | 0.00% |
| p99.9 | 375.0 | 375.0 | 416.7 | 11.12% |

Checksum identical across all 20 runs; book identical at 103 populated levels.

**Detection thresholds.** Throughput changes below ~6% are indistinguishable from noise.
p99 is quantised to a tick and was perfectly stable, so any p99 movement is a real one-tick
(41.7 ns) step. p99.9 straddles a tick boundary, so only multi-tick moves count there.

## Baseline (2026-08-06, commit 1665389)

```
book depth:  entering 25452 qty / 103 levels    leaving 151874 qty / 103 levels
ns/event     79.72 (median of 20 runs)
p99          291.7 ns        p99.9  375.0 ns
per type     new 291.7 / 416.7    cancel 125.0 / 166.7    modify 250.0 / 291.7
worst        ~115000 ns      over 1us: ~11     over 10us: ~7
checksum     dc015ae88f2b6dd0
counters     unavailable without root
```

Structures in place at baseline: `std::map<Price, std::list<Order>>` per side,
`std::unordered_map<OrderId, Location>` index, `Order` = 40 bytes, one heap allocation per
resting order.

| change | commit | ns/event | p99 | p99.9 | verdict |
|---|---|---|---|---|---|
| baseline | 1665389 | 79.72 | 291.7 | 375.0 | - |
| T4 lookup amplification | 635afaf | 81.33 | 291.7 | 416.7 | KEPT, no perf gain |
| T5+T6 record layout + object pool | 0a91b60 | 68.75 | 250.0 | 333.3 | KEPT, -15.4% and two ticks off p99.9 |
| T8 open-addressed id map + capacity | 4fce4bc | 42.70 | 125.0 | 208.3 | KEPT, -37.9% and worst case -88% |
| T9 hybrid price ladder | (this) | 27.50 | 83.3 | 125.0 | KEPT, -35.9% |

## T4 - lookup amplification (2026-08-06)

Replaced the match loop's `front_at` + `cancel`/`modify` sequence with a single
`OrderBook::take_from_front(side, price, want)`. Per fill this removes one tree walk and
one hash lookup for a node the matcher already held a pointer to.

**Result: no measurable improvement at any book size or workload.**

| workload | levels | before ns/event | after ns/event |
|---|---|---|---|
| default (band 9900-10100) | 103 | 79.72 | 81.33 |
| crossing-heavy (9990-10010) | ~20 | 73.38 | 73.49 |
| wide book (9000-11000) | 958 | 89.22 | 89.60 |

All differences are far inside the 6% noise floor. Checksums identical in every pairing,
so behaviour is unchanged; 143 tests and 800 equivalence scripts pass.

**Why the hypothesis failed.** The prediction was that a tree walk is ~7 pointer hops,
each a potential ~100 ns cache miss. But at these sizes the price tree is cache-resident:
103 map nodes is roughly 7 KB and 958 is roughly 61 KB, at or under L1. The pointers being
chased were already in L1 (~1 ns) and the CPU overlaps them with surrounding work. The
pattern (pointer chasing) was costed without checking the precondition (working set larger
than cache). Same failure mode as the earlier hash-mixing episode: a sound general
principle applied where its precondition did not hold.

**Verdict: kept, explicitly not on performance grounds.** It is strictly less work so it
cannot be slower; the match loop went from 17 lines to 4; and a single "take from front"
operation is a better boundary to optimise behind once T6 replaces the underlying storage.

**Useful signal for what is next.** Since the lookups were not the cost, the remaining time
is most likely the per-order `malloc`/`free` from `std::list` - which is exactly what the
object pool in T6 targets.

## T5 + T6 - record layout and object pool (2026-08-07)

Replaced `std::map<Price, std::list<Order>>` with `std::map<Price, Level>` where a `Level`
is two 32-bit indices, orders live in one `std::vector<Slot>` pool, levels are intrusive
doubly-linked lists threaded through the slots by index, and freed slots form a free list
through their own `next` field. Design B (see the M3 spec): the id is stored, slots are
reused. `index_` still a hash map, now `OrderId -> uint32 slot`.

`Order` 40 bytes in a 64-byte `std::list` node -> `Slot` 28 bytes in a pooled vector.
Per-order `malloc`/`free` on add/cancel: eliminated.

### Method

Same-session A/B rather than comparison against the logged baseline, because the logged
baseline predates T4 and its recorded book depth no longer matched. Old and new code built
from the same tree, same Release preset, alternating measurement blocks. Eight invocations
of `bench --reps 9` per side; the sample is the per-invocation batch median.

### Throughput

| | n | median ns/event | min | max | within-group spread |
|---|---|---|---|---|---|
| before | 8 | 81.25 | 80.04 | 84.11 | 5.0% |
| after | 8 | 68.75 | 67.10 | 69.87 | 4.0% |

**-15.4%.** Noise floor is 6.08%, so this clears it by a factor of 2.5. The groups are
completely separated - the slowest new run (69.87) is faster than the fastest old run
(80.04). Exact Mann-Whitney U = 0, two-tailed **p = 0.000155**.

### Tail

| | p99 | p99.9 | worst |
|---|---|---|---|
| before | 291.7 (identical in all runs) | 416.7 (identical in all runs) | 188-204 us |
| after | 250.0 (identical in all runs) | 333.3 (identical in all runs) | 178-185 us |

p99 improved by exactly one 41.67 ns tick, p99.9 by two. Both were perfectly stable across
every run on both sides, so these are real quantised steps, not noise.

### What did NOT improve, and it matters

**The worst case is essentially unchanged at roughly 180 us.** The hypothesis was that
per-order allocation caused the extreme outliers; removing every per-order `malloc` did not
remove them. Whatever produces a 180 us stall is still there.

Candidates, in order of suspicion:

1. **The pool's own growth.** `alloc_slot` uses `push_back`, so the vector doubles and
   memcpy's. At peak the largest single reallocation copies roughly 390 KB. This was a
   deliberate, recorded decision - measure first, then decide - and it is now the obvious
   next experiment: add `reserve()` and re-measure `max_ns` alone.
2. **OS preemption.** macOS has no CPU pinning, and the timer read pair itself was
   previously measured stalling 18 us. Wall clock cannot separate an engine stall from the
   thread being descheduled. This is exactly why the kperf counters exist, and they need
   root - still unverified.
3. Something outside the book entirely (the checksum sink, the sample buffer).

Note the `over_1us` and `over_10us` counts moved around (6-15 and 3-8 on both sides) but
there are only about ten such events in 350000, so three samples of that population say
nothing. Not treated as evidence either way.

### Correctness

- 143/143 tests in all three configurations (debug+ASan/UBSan, relassert, release).
- **The five golden files are byte-identical** - `git status` reports no change under
  `test/golden/`. The engine's entire textual output over those scripts is unchanged.
- The benchmark checksum is `dc015ae88f2b6dd0` before and after: the full `OutEvent` stream
  over 350000 measured events is identical.
- `NaiveEngine` equivalence holds over the goldens plus 800 random scripts.
- `leaks --atExit` on non-sanitised release binaries: 0 leaks in `demo` and `bench`.

Before the engine compiled, `link_back`, `unlink`, `alloc_slot` and `free_slot` were
extracted verbatim into a standalone harness and differential-tested against `std::list`:
1.6 million operations with nine invariants re-checked after every one, plus a growth-heavy
run that forced repeated vector reallocation (peak 2442 slots). Six mutations were injected
to prove the harness could fail; five were caught, and the sixth - dropping a defensive
`next = kNull` in `link_back` - was not, because every `link_back` in that harness follows a
fresh `alloc_slot`. Recorded rather than papered over.

`validate()` gained the invariants the pool needs: links in range, `prev` agreeing with the
forward walk, `tail` genuinely ending the chain, no slot in two chains (which also
terminates on a cycle), the free list disjoint from live slots, free slots carrying
`prev == kNull`, and **live + free == pool size** - the accounting check that catches both a
leaked slot and a double free, neither of which any leak detector can see because the vector
still owns the memory.

**Verdict: KEPT.** First change in M3 to produce a measurable gain.

## Diagnosis: the ~180 us worst case is the id map rehashing (2026-08-07)

Not shipped as a change - this entry records the experiments that located the cause,
which is what T8 will fix properly.

### Counters, finally verified

The kperf path had never been executed by anyone. It works. First run also exposed a bug
in the harness: `counters.begin()/end()` wrapped the whole `run_batch` call, so every
per-event figure divided cycles for 400000 events plus two full-book depth scans by the
350000-event measured window. Fixed; all four numbers were inflated by about 13%.

Corrected, on the standard script:

```
cycles/event          219.14
instructions/event    508.66
IPC                     2.32
branch misses/event    4.1850
implied clock           3.09 GHz   (M1 P-core maximum 3.20)
```

**Implied clock is the load-bearing number.** Cycles accrue only while the thread runs;
wall clock also counts time it does not. 3.09 of a possible 3.20 means the thread held the
core about 97% of the time, so there is not enough non-running time for OS preemption to
explain a 180 us event. That killed the second hypothesis.

**Branch misses at 4.19/event** cost roughly 59 cycles at ~14 cycles each - about 27% of the
219-cycle budget. This was not on the hypothesis list and is now the largest identified
cost after the map. IPC 2.32 on a core that retires 8/cycle is consistent with a
branch-bound rather than memory-bound workload.

### Hypothesis 1: the pool's own vector growth. FALSIFIED.

`pool_.reserve(65536)` so no reallocation can occur during the run.

| | median max_ns |
|---|---|
| baseline | 181083 |
| pool reserved | 185500 |

Slightly worse, distributions fully overlapping. Copying ~390 KB is not what costs 180 us.

### Hypothesis 2: `std::unordered_map` rehashing. CONFIRMED.

`index_.reserve(65536)` only, pool left alone. Eight invocations per side.

| metric | baseline | index reserved | change | test |
|---|---|---|---|---|
| ns/event | 68.7 | 55.3 | **-19.5%** | U=0, p=0.000155, complete separation |
| worst case | 180875 | 17625 | **-90.3%** | U=0, p=0.000155, complete separation |
| p99 | 250.0 | 208.3 | -1 tick | stable in every run on both sides |
| p99.9 | 333.3 | 291.7 | -1 tick | stable in every run on both sides |
| events >10us | 5 | 2.5 | -50% | p=0.0148 |

The index grows to about 27600 entries, so it rehashes roughly 14 times, each time
reallocating the bucket array and re-hashing every element. That is deterministic, which is
exactly why the worst case reproduced at 178-184 us on 8 of 10 baseline runs. The
consistency was the clue; it was initially attributed to the wrong structure.

**Honest limit on the explanation.** Avoided rehash work is about 56000 element-rehashes
over the run, which accounts for perhaps 7 of the 19.5 percentage points. The remainder is
most likely heap layout - without `reserve` the bucket arrays are repeatedly allocated and
freed interleaved with node allocations, scattering the nodes and degrading every lookup.
That split has not been isolated. Doing so needs an L1D-miss counter; the `Counters` alias
table currently resolves only cycles, instructions, branches and branch-misses.

### Why this matters for the story so far

- T4 predicted a lookup win and delivered zero.
- T5+T6 predicted a tail win, delivered a 15.4% throughput win, and left the worst case
  untouched. The log recorded that as unexplained. This is the explanation: the stall was
  never in the order records, it was in the id index.
- Removing every per-order `malloc` did not touch the tail because the tail was one
  container away.

**Not shipping `reserve`.** 65536 is a magic number, it commits about 512 KB of bucket array
up front for a book peaking at 27600 orders, and it does not scale. T8's flat vector indexed
by `id - base` removes the rehash, the hash, and the redundant `contains`-then-`try_emplace`
lookup in `MatchingEngine`. T8 now measures against the 68.7 ns baseline and should claim
the whole win.

## T8 - open-addressed id map with a declared capacity (2026-08-08)

Replaced `std::unordered_map<OrderId, std::uint32_t>` with
`ankerl::unordered_dense::map` (v4.9.0, pinned, MIT, via FetchContent), added a hash
specialisation routed through the library's public integral hash, and gave `OrderBook` an
`explicit OrderBook(std::size_t expectedOrders = 65536)` that reserves both `index_` and
`pool_`.

Spec: `docs/superpowers/specs/2026-08-08-id-map-design.md`.

### Result

Eight invocations per side, same tree, Release, alternating.

| metric | baseline | T8 | change | test |
|---|---|---|---|---|
| ns/event | 68.7 | **42.7** | **-37.9%** | U=0, p=0.000155, complete separation |
| worst case | 180875 | **21333** | **-88.2%** | U=0, p=0.000155, complete separation |
| p99 | 250.0 | **125.0** | -3 ticks | stable across runs |
| p99.9 | 333.3 | **208.3** | -3 ticks | stable across runs |
| events >1us | 13 | 4.5 | -65% | p=0.001088 |
| events >10us | 5 | 1.5 | -70% | p=0.014763 |

**Within-group spread fell from 4.3% to 0.6%** - a seven-fold improvement in run-to-run
consistency. For a latency-sensitive system that is a result in its own right, not a
footnote: the engine is now predictable as well as faster.

### Attribution, measured in two steps

The change was applied in two commits so each half could be measured separately.

| step | ns/event | worst case |
|---|---|---|
| baseline | 68.7 | 180875 |
| swap the map only, no reserve | 55.2 | ~121000 |
| + declared capacity | **42.7** | **21333** |

Swapping the container bought -19.6%; reserving bought another -22.6%. Neither alone gets
close to the pair. Worth recording because the earlier `index_.reserve()` experiment on
`std::unordered_map` gave -19.5% - so a better container and a capacity are roughly equal
contributors here, and the naive conclusion "it was just the rehashing" would have been
half the story.

### Cumulative, milestone to date

| | ns/event | p99 | p99.9 | worst |
|---|---|---|---|---|
| M3 baseline (1665389) | 79.7 | 291.7 | 375.0 | ~115000 |
| after T8 | **42.7** | **125.0** | **208.3** | **21333** |
| improvement | **-46%** | -4 ticks | -4 ticks | **-81%** |

### Correctness

- 143/143 in all three build configurations.
- **The five golden files are byte-identical**; `git status` reports nothing under
  `test/golden/`.
- Benchmark checksum `dc015ae88f2b6dd0`, unchanged since the M3 baseline.
- `NaiveEngine` equivalence holds over the goldens plus 800 random scripts.
- `leaks --atExit` on non-sanitised release binaries: 0 leaks in `demo` and `bench`.

### What was verified before writing any code

- All 17 `index_` call patterns compiled and run against the new map with the real
  `OrderId` type, including const `find` (`validate()` is const) and `size()` as a live
  count across a 500000-wide id gap.
- CMake `FetchContent` integration built end to end in a scratch project.
- `hash_is_avalanching_v` asserted at compile time, so a wrong marker fails the build
  rather than silently degrading the distribution.

### Under sudo, with counters: the tail is gone

```
ns/event   41.86 (spread 2.0%)
p99        125.0     p99.9  208.3
worst      4875 ns   over 1us: 1   over 10us: 0

cycles/event          136.07
instructions/event    360.96
IPC                     2.65
branch misses/event    2.9719
implied clock           3.20 GHz
```

| | before T8 | after T8 |
|---|---|---|
| worst case | ~181000 ns | **4875 ns** |
| events >1us (of 350000) | 13 | **1** |
| events >10us | 5 | **0** |
| implied clock | 3.09 GHz | **3.20 GHz** |
| cycles/event | 219.14 | **136.07** |
| instructions/event | 508.66 | **360.96** |
| IPC | 2.32 | **2.65** |
| branch misses/event | 4.1850 | **2.9719** |

**Implied clock at 3.20 GHz - the P-core maximum - means the thread held the core for
essentially all of the wall time.** No descheduling at all. That also retrospectively
explains the old 3.09: rehashing allocated large fresh blocks, and first-touching new pages
is kernel time during which the thread is not running. The stall was never purely compute.

**The two instruments agree exactly.** Cycles/event fell 37.9% and wall-clock ns/event fell
37.9%. Independent measurements landing on the same figure is evidence the measurement
apparatus itself is sound.

### Still open

- **Branch misses are now the largest identified cost.** 2.97/event at roughly 14 cycles
  each is about 42 of the 136 cycles per event - **31% of the budget**, up from 27% as a
  share even though the absolute count fell 29%. This was never on the original hypothesis
  list; the counters found it.
- **361 instructions per event** is still a lot for "find a level, walk a chain, adjust a
  quantity."
- The worst case is 4875 ns rather than zero. One event in 350000 exceeds 1 us. Not yet
  explained, but no longer worth chasing ahead of the branch work.
- The split between "avoided rehashing" and "better heap layout" in the original reserve
  experiment was never isolated, and still has not been.

## T9 - hybrid price ladder (2026-08-09)

Replaced `std::map<Price, Level>` on each side with `PriceLadder<Side, Level>`: an array
indexed by price tick, an occupancy bitmap, a cached best index, and a `std::map` tail for
prices outside the band. Default band [0, 16383], chosen because it covers the unit tests
(0-200), the golden scripts (90-105) and the benchmark (9900-10100).

Spec: `docs/superpowers/specs/2026-08-08-price-ladder-design.md`.

### Result

Eight invocations per side, same tree, Release, alternating.

| metric | before | after | change | test |
|---|---|---|---|---|
| ns/event | 42.9 | **27.5** | **-35.9%** | U=0, p=0.000155, complete separation |
| p99 | 125.0 | **83.3** | -1 tick | stable in every run on both sides |
| p99.9 | 208.3 | **125.0** | -2 ticks | stable in every run on both sides |
| worst case | 14354 | 11896 | -17% | p=0.80 - **not significant** |
| spread | 5.3% | 2.6% | | |

**The worst case did not move, and that is the honest reading.** p=0.80 with fully
overlapping samples. T8 removed the stall that dominated the tail; whatever remains at
~12 us is not the price map.

### Why T4 measured nothing here and T9 measured -36%

T4 removed roughly one price-map lookup **per fill** and saw zero. Fills are a minority of
events - the book grows from 19338 to 151874 in quantity, so most orders rest rather than
trade. Roughly 2 ns/event against a then-80 ns baseline, inside the 6.08% noise floor.

T9 removes price-map cost from **every** event. Different denominators, not a contradiction.
This was predicted in the spec before the work started, from a probe measuring the structure
in isolation at 3.1x even with zero level churn.

### Cumulative

| | ns/event | p99 | p99.9 | worst |
|---|---|---|---|---|
| M3 baseline (1665389) | 79.7 | 291.7 | 375.0 | ~115000 |
| after T9 | **27.5** | **83.3** | **125.0** | ~12000 |
| improvement | **-65%** | **-71%** | **-67%** | **-90%** |

### Correctness

- 164/164 in all three build configurations.
- **The five golden files are byte-identical.** The entire price-level storage layer was
  replaced and the engine's textual output did not move by one character.
- Benchmark checksum `dc015ae88f2b6dd0`, unchanged since the M3 baseline.
- `NaiveEngine` equivalence holds over the goldens plus 800 random scripts.
- Zero leaks in `demo` and `bench`.

### Three defects found by review before wiring it in

None were caught by any test; all were found by re-running the check that motivated them.

1. **Undefined behaviour in the ladder constructor.** `max.ticks - min.ticks` overflows;
   UBSan confirmed it on [INT64_MIN, INT64_MAX], and nothing checked `min <= max`. Now
   computed in unsigned arithmetic, which is defined to wrap, with `DHFT_CHECK` on both.
2. **`rescan_best` scanned from the far end of the bitmap.** At the 16384-tick default band
   with the book near 9900, that was ~100 empty words per rescan, every time the best level
   emptied. After erasing the best every remaining level is necessarily worse, so the scan
   can start at the erased level's word: **48.7 ns -> 4.5 ns**, and band width no longer
   affects the cost at all.
3. **`find` had no const overload**, so `front_at` - which is const - could not have
   compiled. Solved with an explicit object parameter (`this Self&&`) so one body serves
   both, rather than duplicating it or casting away const.

A fourth near-miss is worth recording: the overflow guard was added but the constructor was
not changed to call it. Everything compiled, `-Werror` was satisfied, and all 164 tests
passed while the undefined behaviour remained. Only re-running the original UBSan probe
exposed it.

### Design note

The spec called for a second traversal method, `for_each_from_best`, merging the array with
the map tail in price order. It was dropped. Only `depth_into` needs sorted order, it is not
on the hot path, and building it would have required walking the bitmap directionally with
`1ULL << 64` undefined-behaviour edge cases. `depth_into` now collects via `for_each` and
sorts. One traversal method instead of two, and a whole class of bug that never existed.

### T9 under sudo, with counters

```
ns/event   27.22 (spread 1.9%)
p99        83.3      p99.9  125.0
worst      4583 ns   over 1us: 1   over 10us: 0

cycles/event           86.30
instructions/event    267.40
IPC                     3.10
branch misses/event    1.7870
implied clock           3.20 GHz
```

| | after T8 | after T9 | change |
|---|---|---|---|
| cycles/event | 136.07 | **86.30** | -36.6% |
| instructions/event | 360.96 | **267.40** | -25.9% |
| IPC | 2.32 | **3.10** | +33.6% |
| branch misses/event | 2.9719 | **1.7870** | **-39.7%** |
| implied clock | 3.20 | 3.20 | at maximum |

**The branch prediction was correct.** The spec argued, before any code existed, that a
red-black tree walk is the unpredictable-branch pattern - each step a comparison the
predictor cannot learn - and that T9 might therefore deliver part of what T10 was going to
chase. Branch misses fell 39.7%.

Cycles/event fell 36.6% against a wall-clock fall of 35.9%. Two independent instruments,
same answer.

**IPC 3.10** on a core that retires 8/cycle, up from 2.32. The engine is no longer
branch-bound to the same degree.

### Milestone 3, baseline to here

| | M3 baseline (1665389) | after T9 | improvement |
|---|---|---|---|
| ns/event | 79.7 | **27.2** | **-66%** |
| p99 | 291.7 | **83.3** | **-71%** |
| p99.9 | 375.0 | **125.0** | **-67%** |
| worst case | ~115000 | **4583** | **-96%** |
| events >1us (of 350000) | 13 | **1** | |
| events >10us | 5 | **0** | |
| cycles/event | n/a | 86.30 | |
| branch misses/event | n/a | 1.79 | |

### What is left

- **Branch misses are still the largest identified cost**: 1.79/event at roughly 14 cycles
  is about 25 of the 86 cycles per event, **29% of the budget**. The absolute count fell 40%
  but the share is roughly unchanged, because the total shrank too.
- **267 instructions per event.** Down from 509, still high for the work being done.
- The remaining branches are largely data-dependent - does the order cross, is the level
  emptied, is the fill full or partial. Static hints are the wrong tool for those; the M3
  spec already flagged that published HFT work finds them unreliable. Instrumentation PGO
  is the honest instrument: it measures the real branch probabilities rather than guessing.

---

## Closing the T9 verification gap: `PriceLadder::validate()`

Step 5 of the T9 plan was skipped. The ladder shipped with a cached `bestIdx_` that nothing
checked, which is the only piece of state in the engine that can produce a **plausible**
wrong answer: the engine would match at a price with no orders behind it and every test
would still pass.

### What was written, and what was deliberately not

The T9 spec listed five invariants. Three of them cannot fail:

| spec invariant | verdict |
|---|---|
| 2. cached best is correct | **written** - the only silent-wrong-answer risk |
| 4. tail holds out-of-band prices only | **written** - the routing rule the class rests on |
| 1. bitmap agrees with the levels | already covered: `check_level` rejects a null-headed level, and a wrongly cleared bit surfaces as `index_.size() != counted` |
| 3. no price in both structures | implied by 4 - the array only ever holds in-band prices |
| 5. level count matches | tautological - `for_each` is built from exactly the two things it would compare |

Writing 1, 3 and 5 would have produced three checks incapable of failing. **A check that
cannot fail is worse than no check**, because it reads as coverage.

A third check was added that the spec missed: **no occupancy bit set past the end of
`levels_`**. `occupied_` is sized in whole words, so a band of 200 ticks has 56 padding
bits; a set one would make `for_each` read `levels_[237]` out of bounds and `rescan_best`
return an index that is not a level.

The best index is recomputed by brute force over the whole bitmap, using the `for_each`
idiom rather than `rescan_best`. That is deliberate: `rescan_best` is the function under
test, and an oracle that calls the code it checks agrees with its bugs.

### Mutation testing - five mutations, five caught

| # | mutation | caught by | message |
|---|---|---|---|
| 1 | `erase` no longer invalidates the cached best | property_test, seed 1 event 4 | `bid ladder: cached best index is 96 but the true best is none` |
| 2 | `insert` no longer updates the cached best | property_test, seed 1 event 0 | `ask ladder: cached best index is none but the true best is 99` |
| 3 | `insert` routes every price to the tail | property_test, seed 1 event 0 | `ask ladder: in-band price 99 is in the tail map` |
| 4 | `set()` also lights the top bit of the final word | price_ladder_test | `occupancy bit set past the end of the ladder` |
| 5 | the `used != 0` guard removed | price_ladder_test **only** | false positive, as designed |

Two results worth keeping:

- Under mutation 4, `ValidateAcceptsABandThatFillsItsWordsExactly` stayed green. For a
  128-tick band bit 63 of the final word is a legitimate index, so the check correctly
  declines to fire. It discriminates rather than always firing.
- **Mutation 5 was caught by exactly one test and nothing else.** `~0ULL << 0` is all ones,
  so dropping the guard flags every legitimate bit in a band whose span is a multiple of 64.
  `property_test` did not catch it - the default band's final word covers ticks 16320-16383
  and is always empty at the prices the generator uses. Without that one test the guard
  could have been deleted silently.

**The default band has no padding bits at all** (16384 ticks = exactly 256 words), so
Check A never executes through `OrderBook::validate()`. It only runs on ladders whose span
is not a multiple of 64, and `price_ladder_test` did not call `validate()` at all. The check
was untestable until those calls were added. Found by trying to mutate it, not by review.

### Cost - measured, not assumed

`validate()` runs after every event in `Property.InvariantsHoldAfterEveryEvent`: 500 seeds,
roughly 350000 calls.

| | before | after | change |
|---|---|---|---|
| `Property.InvariantsHoldAfterEveryEvent` | 55-65 ms | 77-79 ms | **+35%** |
| full `ctest --preset relassert` | 0.87 s | 1.03 s | **+18%** |
| benchmark ns/event | 27.83 | 27.83 | none |
| benchmark checksum | `dc015ae88f2b6dd0` | `dc015ae88f2b6dd0` | none |

The scan walks 256 words per side whether or not the book is small, about 160 ns per call.

**Kept rather than optimised.** The obvious speed-up is to scan directionally from the
extreme end and stop at the first set bit - which is what `rescan_best` does, and would make
the oracle share the code path it exists to police. Independence is worth 22 ms in a suite
that runs in one second. This is not the hot path, and the benchmark confirms the hot path
did not move.

### Gate

167/167 (was 164; +3 ladder tests) in debug, relassert and release. Five golden files
byte-identical. Benchmark checksum unchanged. `leaks --atExit` reports zero for `demo` and
`bench` on the non-sanitised build. The success path of `validate()` allocates nothing -
the only strings built are on the failure return.
