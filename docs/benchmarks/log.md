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
| T5+T6 record layout + object pool | (this) | 68.75 | 250.0 | 333.3 | KEPT, -15.4% and two ticks off p99.9 |

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
