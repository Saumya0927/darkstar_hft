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

---

## L1D cache misses measured: the prefetch question, answered

`Counters` resolved only cycles, instructions, branches and branch-misses, so "should we
prefetch?" had been an opinion. Now measured, under `sudo`, 400000 events:

```
ns/event                26.54  (spread 3.2%)
cycles/event            87.09
instructions/event     267.87
IPC                      3.08
branch misses/event     1.7820
L1D miss ld/event       4.3804
L1D miss st/event       0.8292
implied clock            3.13 GHz
```

### The first derived number printed was wrong, and the other counters prove it

`bench` printed "L1D miss cycles 71.8% of budget (at ~12 cy/miss)". That figure assumes no
miss overlaps another, and it cannot be true:

```
5.21 misses x 12 cycles                        = 62.5 cycles
87.09 - 62.5                                   = 24.6 cycles left
267.87 instructions in 24.6 cycles             = IPC 10.9
M1 P-core retires at most 8/cycle               -> impossible
```

IPC had already said so: a memory-bound workload has **low** IPC, and this one rose to 3.08
after T9. The line has been relabelled `L1D miss ceiling ... if none overlapped`, with the
assumed latency printed, so it cannot be read as a measurement again.

### A defensible bound instead

```
cycles/event                                     87.09
floor: 267.9 instructions at peak 8 IPC          33.5
unexplained                                      53.6
branch misses, 1.782 at ~14 cycles               24.9   (29% of budget)
therefore memory stalls + dependency chains     <=28.7  (33% of budget, an upper bound)
naive miss cost                                  62.5
implied overlap factor                          >=2.2x
```

Memory is worth **at most a third** of the budget and shares that third with dependency
stalls. Branch misses remain the largest single identified cost at 29%.

### What 4.38 load misses per event actually means

Per event the engine touches: the id-map bucket, the map's value array, the pool slot, the
ladder level, the occupancy word, and one or two chain neighbours - about four to six
distinct cache lines. **4.38 misses per event is therefore roughly one miss per structure
touched.** The book holds on the order of 10^5 live orders, so `pool_` and `index_` together
are several MB against a 128 KB L1d. These misses are not waste; they are the floor for data
structures this size, and they hit in the 12 MB L2.

That matters for the conclusion: **prefetching cannot reduce the number of misses, only hide
their latency - and the latency is already being hidden at >=2.2x overlap.**

### Where a prefetch could still pay, and why it is not T10

The per-event chain is serial: id -> hash bucket -> slot index -> `pool_[idx]`. The second
load cannot issue until the first returns. The bucket address for the **next** event is
computable immediately, so software-pipelining across events would break the chain.

That requires a batch or lookahead API. `process(const InEvent&)` takes one event and cannot
see the next, and prefetching `script[i+1]` from inside the harness would measure a harness
trick rather than the engine. A real feed delivers packets of events, so a batch entry point
is legitimate production design - which makes this **rung 3 work, not T10 micro-tuning.**

### T10, re-scoped on evidence

- **`__builtin_prefetch` - deferred, not rejected.** Now justified in principle by a real
  measurement, but it needs an interface change. Revisit with feed ingestion.
- **`[[likely]]`/`[[unlikely]]` - still skip.** Unchanged reasoning: the branches are
  data-dependent.
- **Three build-flag experiments remain**, none requiring a code change:
  `-fwhole-program-vtables` (three `blr` indirect calls survive ThinLTO in
  `MatchingEngine::process` - disassembled and counted), `-mcpu=native`
  (`DHFT_TUNE_NATIVE` is OFF and no preset enables it), and instrumentation PGO.

### Correction to the M3 tail figures

The T9 entry records "worst 4583 ns, over 1us: 1, over 10us: 0" and the cumulative table
reports a 96% worst-case improvement. Six unprivileged runs of the same unmodified binary:

| run | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|
| worst (ns) | 15000 | 9083 | 17917 | 5667 | 34167 | 4708 |
| over 1us | 7 | 2 | 2 | 2 | 2 | 1 |
| over 10us | 3 | 0 | 1 | 0 | 1 | 0 |

**4583 ns and "over 10us: 0" were a single lucky draw, not a property of the engine.** The
direction is real - the M3 baseline was ~115000 ns and nothing now approaches it - but the
honest statement is "worst case now varies between roughly 5 and 35 us", not "4583 ns". The
handover already warned that `max_ns` is a single extreme value; this is that warning
arriving. The 15875 ns seen in the counter run above is unremarkable, not a regression.

---

## T10: three build-flag experiments, two winners and a stale noise floor

No engine code changed. Each variant was built in its own tree so all binaries existed at
once and could be A/B'd in a single session.

### The null control came first

`base` and `native` turned out **byte-identical by md5**, which makes them a free null
control: any difference measured between them is pure noise.

| comparison | median A | median B | change | U | p | verdict |
|---|---|---|---|---|---|---|
| **base vs native (byte-identical)** | 27.18 | 27.20 | +0.1% | 43 | 0.279 | no effect, as it must be |
| base vs wpvh (devirtualized) | 27.16 | 25.87 | **-4.8%** | 4 | 0.00186 | significant |
| base vs pgo | 27.41 | 24.89 | **-9.2%** | 0 | 0.00016 | significant, separated |
| base vs both | 27.38 | 24.01 | **-12.3%** | 0 | 0.00016 | significant, separated |
| pgo vs both | 25.18 | 24.12 | -4.2% | 2 | 0.00062 | devirtualization adds on top of PGO |

Eight alternating invocations per side, `--reps 5` each, sample = the batch median.

### The 6.08% noise floor was stale, and it would have thrown away a real win

Every earlier entry judged results against a 6.08% floor measured at the M3 baseline. The
null control above puts current run-to-run noise at **+0.1%, p=0.279**, with per-run spreads
of 0.2 to 3%. The engine got roughly three times faster during M3, so a fixed *percentage*
floor measured against an 80 ns baseline no longer describes a 27 ns one.

**The -4.8% devirtualization win would have been dismissed as noise under the old floor.**
Re-measure the floor whenever the baseline moves; do not inherit it.

### `-mcpu=native`: rejected, no effect whatsoever

Byte-identical binary. Verified the flag genuinely reached the compiler (`-O3 -mcpu=native
-flto=thin` in the real invocation, `DHFT_TUNE_NATIVE:BOOL=ON` in the cache) before
concluding anything - the first attempt had silently applied no flags at all, because zsh
does not word-split unquoted variables and the whole flag string arrived as one argument.

The reason it does nothing is structural: the engine is pure scalar integer code. The extra
target features `-mcpu=native` unlocks - fp16, dotprod, crypto - are simply never used.
`countr_zero`/`countl_zero` lower to `rbit`/`clz`, which are baseline ARMv8.

### `-fwhole-program-vtables`: a no-op on its own

Also byte-identical to baseline. The flag is accepted and does nothing under Apple `ld64`
(ld-1267), because with default symbol visibility the compiler must assume a class could be
overridden outside the link unit.

Paired with `-fvisibility=hidden -fvisibility-inlines-hidden` it works. Attribution was
checked with a fourth arm rather than assumed:

| variant | instructions in `process` | indirect calls |
|---|---|---|
| base | 260 | 3 |
| `-mcpu=native` | 260 | 3 |
| `-fwhole-program-vtables` alone | 260 | 3 |
| hidden visibility alone | 260 | 3 |
| **hidden + whole-program-vtables** | **307** | **0** |

Hidden visibility alone changes nothing; it only grants permission. The devirtualization is
genuinely `-fwhole-program-vtables`. The three `blr` are the `Sink::on_event` calls - the
type-erasure candidate flagged by *C++ Software Design* and recorded in the handover as
"never been measured". Now measured, and removed by a build flag rather than a redesign.

`DHFT_WHOLE_PROGRAM_VTABLES` therefore sets all three flags. An option that silently does
nothing without a companion flag is a trap, so the option expresses the intent instead.

**Adopted into the release preset.**

### PGO: the largest single win, and deliberately NOT adopted

`-fprofile-generate` -> run the benchmark -> `llvm-profdata merge` -> `-fprofile-use`.
`process` goes from 260 to **1873 instructions** as PGO inlines the hot callees. -9.2%
alone, -12.3% combined with devirtualization.

**It stays opt-in, and the number should be treated with suspicion**, because the profile
was trained on the benchmark and then measured on the same benchmark. That is
overfitting by construction. A profile from real market data would be worth adopting; a
profile from a synthetic generator tells you how fast the engine runs the generator. The
mechanism is in place (`DHFT_PGO=generate|use`, `DHFT_PGO_PROFILE`) so it can be retrained
once rung 3 supplies real traffic.

One real snag, kept as a note: `-fprofile-use` fails the build with `-Werror` because
`apps/demo.cpp`'s `main` collides with the `main` the profile was trained on - IR profiles
key on function name. The count is discarded, which is correct, but it surfaces as
`-Wbackend-plugin`, not the `profile-instr-*` warnings that were already suppressed.

### Gate

167/167 in debug, relassert and release for every variant built, including the instrumented
one. Checksum `dc015ae88f2b6dd0` on all six binaries. Golden files byte-identical. Zero
leaks in `demo` and `bench`. Release preset now measures **26.44 ns/event at 0.2% spread**.

### T10 verdict

| tool | outcome |
|---|---|
| `[[likely]]`/`[[unlikely]]` | skipped - branches are data-dependent |
| `__builtin_prefetch` | deferred to rung 3 - needs a batch API, misses are already overlapped 2.2x |
| `-mcpu=native` | rejected - byte-identical output |
| `-fwhole-program-vtables` | **adopted** with hidden visibility, -4.8% |
| instrumentation PGO | works, -9.2%, kept opt-in pending a non-synthetic profile |

---

## Full-codebase review

Every file read end to end. Findings below are what survived checking; each defect was
reproduced before being fixed.

### Defect 1: undefined behaviour in `bench --reps 0`

Every reported statistic reduces over `nsPerEvent`/`p99`/`p999`. At `--reps 0` the loops
never run, the vectors are empty, and `*std::min_element(begin, end)` dereferences `end()`.

```
bench.cpp:179: runtime error: reference binding to null pointer of type 'double'
AddressSanitizer:DEADLYSIGNAL
```

### Defect 2: `bench --reps abc` terminates

`std::stoll` throws `std::invalid_argument`; `main` has no handler.
`libc++abi: terminating due to uncaught exception`.

### Defect 3: unknown options silently ignored

`bench --typo 1` ran a completely normal benchmark. A mistyped flag was indistinguishable
from an honoured one - which could have silently invalidated any measurement in this log.

All three fixed: one conversion helper that rejects junk and trailing characters, range
checks (`--reps >= 1`, no negative counts, `--min-price <= --max-price`), and unknown
options rejected with a usage message.

### Defect 4: `Samples::add` after `finalise` left `sorted_` true

Latent - no caller does it today - but `percentile_ns` would have read a half-sorted vector
and the `DHFT_CHECK` guarding it would have passed. `add` now clears the flag, so the class
is correct by construction rather than by convention.

### Defect 5: the tail map had NO coverage through the engine

The important one. `GenConfig` defaults to prices 95-105 and every equivalence, property and
golden test stays inside the default `[0, 16383]` band. **The `PriceLadder` tail map was
never exercised through `OrderBook` or `MatchingEngine` by any test.**

That is precisely where this codebase documents its sharpest hazard: `take_from_front` and
`cancel` hold a `Level*` across `m.erase(price)`, which for an in-band price only clears a
bitmap bit but for a tail price destroys a `std::map` node.

Five tests added: all-out-of-band prices, prices straddling the band edge, negative prices, a
book-state comparison with `validate()` after every event on a straddling band, and an
explicit modify/cancel reject-path script (the generator only ever emits positive quantities
against live ids, so those rejects were unreachable from random scripts).

**Proven to reach the path, not assumed.** Injecting the documented use-after-free - reading
`level.head` after `m.erase(price)` - produces:

| test | result under ASan |
|---|---|
| `AgreesOnRandomScripts` (in-band) | **passes** - erase only clears a bit, as documented |
| `AgreesWhenEveryPriceIsOutsideTheLadderBand` | **`heap-use-after-free` at OrderBook.cpp:350** |

The old suite could not have caught it. The current code is correct - it reads everything it
needs before erasing - and that is now enforced by a test rather than by a comment.

### Verified correct, no change needed

- **Modify sequence semantics match `NaiveEngine` exactly.** An increase takes a fresh
  sequence and loses time priority; a decrease keeps its place; both consume one sequence on
  success and none on reject; both engines check quantity before identity. This is the area
  `REVIEW_LOOP.md` names as one of the two worst defects the project has had, so it was
  re-derived from both sources rather than trusted, and is now covered by an explicit test.
- **`add()` exception safety.** The catch erases the index entry, returns the slot, and
  rethrows. No path leaks a slot or leaves a half-linked level.
- **No reference outlives a `pool_` reallocation.** Links are indices; `add` holds no map
  iterator across other work.
- **Slot accounting.** `validate()` proves `live + free == pool.size()`, which catches both a
  leaked slot and a double free - neither visible to any leak detector, since the vector
  still owns the memory.
- **Edge probes, all clean under ASan+UBSan**: a one-tick band with traffic either side, the
  `int32` price extremes, saturating quantity at 2x `INT32_MAX`, the widest legal band
  (2^24-1 ticks), and 20000 orders on a single level then swept by one aggressor.
- **Ownership.** Zero `new`/`delete`/`malloc` anywhere; one `unique_ptr`, for the `Counters`
  PIMPL. Every other pointer is a non-owning observer, which is R.30/F.7, not a gap.

### Noted, deliberately not changed

- `io/Script.cpp` reads ids with `>>` into `std::uint64_t`, which accepts `-5` and wraps.
  Test-only parser fed by files in the repo.
- `Generate.cpp` short-circuits `!live.empty() && pctDist(rng) < ...`, so RNG consumption
  depends on `live` being empty. Deterministic given the seed - the generator never consults
  the engine - so reproducibility holds. A smell, not a bug.
- `TextSink::on_event` is `noexcept` and writes to an `ostream`. Streams do not throw unless
  exceptions are enabled on them, which nothing here does.

### Gate

**172/172** (was 167; +5 tail-coverage tests) in debug, relassert and release. Golden files
byte-identical. Checksum `dc015ae88f2b6dd0`. Zero leaks in `demo` and `bench`. Release
throughput unchanged at 25.8-26.3 ns/event.

---

## Final counters, post-devirtualization

`sudo ./build-release/apps/bench --reps 9` on the shipped Release build.

```
ns/event               25.46  (spread 3.1%)      p99 83.3      p99.9 125.0
cycles/event           82.74
instructions/event    245.58
IPC                     2.97
branch misses/event    1.7894
L1D miss ld/event      4.3314
L1D miss st/event      0.8130
implied clock           3.21 GHz
```

| metric | pre-devirt | final | change |
|---|---|---|---|
| ns/event | 26.54 | 25.46 | -4.1% |
| cycles/event | 87.09 | 82.74 | -5.0% |
| instructions/event | 267.87 | 245.58 | **-8.3%** |
| IPC | 3.08 | 2.97 | -3.6% |
| **branch misses/event** | 1.7820 | 1.7894 | **+0.4%** |
| L1D miss ld/event | 4.3804 | 4.3314 | -1.1% |

### The reason the devirtualization won is not the reason that was assumed

The T10 entry framed the three surviving `blr` as a cost worth removing without saying how
they cost. The counters answer it: **branch misses did not move at all.** The branch target
buffer saw one target and predicted it perfectly. The win is **instruction count** - 22 fewer
per event - from deleting three call sequences and inlining the sink body.

IPC fell from 3.08 to 2.97, which is not a regression. The instructions removed were cheap
and perfectly predicted, so the surviving mix is denser in real work. Cycles fell 5.0% and
instructions 8.3%; both moving down is the result.

Worth recording because it looks contradictory: statically `MatchingEngine::process` grew
from 260 to 307 instructions, because the callee was inlined into it. Dynamically,
instructions per event fell, because the call, the return and the callee prologue/epilogue
all vanished. A bigger function doing less work.

### Cycle budget, final

```
cycles/event                                     82.74
floor: 245.6 instructions at peak 8 IPC          30.7
branch misses, 1.7894 at ~14 cycles              25.1   30% of budget
memory stalls + dependency chains               <=27.0   33% of budget, upper bound
naive miss cost, 5.14 x 12 cycles                61.7
implied overlap                                 >=2.3x
```

The printed `L1D miss ceiling` of 74.6% remains a ceiling and remains refuted by the same
arithmetic as before: 5.14 misses stalling 12 cycles each would leave 21 cycles to retire
245.6 instructions, an IPC of 11.7 on a core that retires at most 8.

**implied clock 3.21 GHz against a 3.20 GHz P-core maximum** - the thread held the core for
effectively the whole measured window, so none of this is contaminated by descheduling.

---

## Sampling profile: the first flame graph, and a check on the benchmark's own overhead

Every M3 measurement was batch timing, PMU counters or disassembly. A sampling profile was
listed in the T3 plan and never run, so the distribution of time *within* the 25 ns was
never actually seen. Run now: `/usr/bin/sample` at 1 ms over 12 s of the release benchmark
(`--reps 1500`, since the default run finishes in about 1.2 s and cannot be sampled),
folded with the FlameGraph scripts.

### Self time by leaf frame, 10,000 samples

```
42.2%  MatchingEngine::process       (with everything inlined into it: best(), the
                                      match loop, event dispatch, the sink hash)
13.6%  OrderBook::add
11.9%  OrderBook::take_from_front
10.0%  OrderBook::cancel
 8.5%  ankerl unordered_dense table  (index_ probe, erase and emplace)
 4.4%  OrderBook::modify
 3.8%  depth_into + total_quantity   (measure_depth, benchmark bookkeeping, not per event)
 3.2%  PriceLadder::erase, both sides
 2.1%  run_batch
 0.1%  everything in libsystem_malloc, combined
```

Two things worth recording.

**Nothing allocates on the hot path.** malloc, free, bzero and memset together are eleven
samples out of ten thousand, all attributable to startup and the per-repetition depth
scan. This was asserted throughout M3 and is now observed rather than reasoned.

**Attribution inside `process` is coarse by construction.** ThinLTO plus devirtualization
inlined the ladder lookups, the sink and the match loop into one frame, so 42% self time
means "the engine's control flow and everything the compiler folded into it", not that
`process` itself does 42% of the work. Splitting it would require a build with inlining
disabled, which measures a different program. The counter-based decomposition already in
this log (branch misses ~30%, memory <=33%) is the better guide to what that 42% contains.

### Is the benchmark's checksum sink inflating the engine number?

`ChecksumSink::on_event` does six xor-multiply steps per event, and after devirtualization
it inlines into `process`. Suspected that a meaningful slice of the published 25.5 ns was
the benchmark measuring its own sink. A/B, same script, sixteen alternating runs each:

```
null sink       26.30 ns/event   [25.83, 41.67]
checksum sink   27.18 ns/event   [26.75, 34.61]
difference       0.88 ns/event   3.2% of the published figure
```

Not inflated. The sink is under a nanosecond and the published number stands.

### What the profile changes

Nothing about the engine, which is the honest result of a check like this. It confirms two
claims that were previously inferred, and it puts a ceiling on what further micro work
could return: the four `OrderBook` operations plus the index are ~48% of samples, and the
rest is control flow already shown to be branch-bound. The remaining lever is the tail on
Linux, as recorded in the T10 verdict, not the median on this machine.

---

## 64-bit order ids: slot 28 -> 32 bytes, cost measured

Prerequisite for real data: CME order ids are 64-bit and the slot held 32. Widened the
field, kept it first in the struct, static_assert moved to 32.

Same-session A/B, previous binary against the new one, 16 alternating invocations pooled
across two runs. The machine was noisier than usual (medians near 31 instead of 26, one
outlier at 46), so both sides carry it equally.

```
28-byte slot (32-bit id)   median 31.67   [27.40, 45.95]
32-byte slot (64-bit id)   median 31.55   [27.13, 40.06]
change                     -0.4%   U=126   p=0.956   no detectable difference
```

For comparison the 128-bit experiment earlier cost +8.6% at p=0.00016, because
`unsigned __int128` forces 16-byte alignment and pushed the slot to 48 bytes. 64 bits
lands on a natural 8-byte boundary and the four extra bytes are invisible at this noise
level. If there is a cost it is under about 3%, the resolution of this run.

Correctness: the new test aborts on the previous engine (width check fires) and passes on
this one. 173/173 in three configs, goldens byte-identical, checksum unchanged, zero leaks.
