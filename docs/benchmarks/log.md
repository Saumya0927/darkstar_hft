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
