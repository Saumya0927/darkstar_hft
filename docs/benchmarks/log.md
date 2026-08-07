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
| T4 lookup amplification | (this) | 81.33 | 291.7 | 416.7 | KEPT, no perf gain |

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
