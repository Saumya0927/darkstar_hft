# darkstar_hft

A limit order book and matching engine written from scratch in C++26. It reconstructs an exchange's book one order at a time, matches incoming orders by price then time, and does both in about 25 nanoseconds per event. Every optimisation was proven to change nothing about the output before it was kept.

Status: the engine and its verification harness are complete. Live market data ingestion is the next piece of work.

## Why this exists

This started as a way to learn C++ and low level programming properly, by building something demanding enough that shortcuts would show. An order book turned out to be the right subject. It touches memory layout, cache behaviour, branch prediction, hashing, intrusive data structures and measurement, and it punishes every mistake with a visibly wrong answer or a visibly slow one.

The goal became to make it genuinely production grade rather than a toy, so it is built and measured the way a real trading engine would be. Every line of the engine was written by hand as part of that learning, and the repository keeps the record of what was tried, what worked, and what turned out to be wrong.

## What it does

The engine has two jobs. Live, it is a book builder: it mirrors the exchange's book from an order by order feed. Offline, it is a fill simulator: replay real history through it, inject your own order, and it tells you whether that order would actually have filled given where it sat in the queue. Most backtests guess at that. This one does not.

Every trade the engine reports names both sides by order id. That single detail is what makes the simulation honest. If your injected order shows up as the resting side of a trade, you were filled, at that moment, at that price, having waited behind exactly the orders that were really there.

## What is inside

Three data structures carry the whole engine.

Price levels live in a ladder: an array indexed by price tick, with a bitmap for occupancy and a cached best price, plus a small map for prices outside the band. Order records live in a pool of 28 byte slots linked by index rather than pointer, with a free list threaded through the dead slots. The order id index is an open addressed hash map reserved to capacity so it never rehashes during a session.

Everything else exists to prove those three are right. A deliberately slow reference engine written as a flat linear scan acts as an oracle. Five golden files pin the exact textual output. Property tests run five hundred random seeds and check invariants after every event. A delta debugging shrinker reduces any disagreement to its shortest reproducer. Every checker in the repository has been deliberately broken and watched to fail before being trusted.

## Design notes

Prices are integer ticks, never floating point. Every real exchange works this way internally, and it is the only representation where equality and ordering are exact.

Order records are linked by 32 bit index rather than by pointer. Indices survive the pool being reallocated, take half the space, and let the whole record fit in 28 bytes.

The matching engine is single threaded by design and has no clock. Its only notion of time is a sequence number. That is what makes every run of the same input produce the same output byte for byte, which in turn is what makes the golden files and the oracle meaningful.

Nothing allocates on the hot path after startup. Capacity is declared up front and the engine rejects rather than grows when it is exceeded, because a live engine stalling to reallocate is worse than a rejected order.

## Measured performance

All figures are from an Apple M1 with Clang 22.1.4, batch timed over 400,000 generated events, nine repetitions, taking the median.

```
time per event               25.5 ns
p99 latency                  83.3 ns
p99.9 latency               125.0 ns
worst case                   5 to 35 us
throughput                   about 39 million events per second, one core
```

Hardware counters on the release build, read through the kperf framework:

```
cycles per event               82.74
instructions per event        245.58
instructions per cycle          2.97
branch misses per event         1.79
L1D load misses per event       4.33
L1D store misses per event      0.81
```

The worst case is quoted as a range on purpose. It varies between roughly 5 and 35 microseconds from run to run on this machine, and a single figure would be misleading. The 41.667 ns timer granularity on this platform also means the tail percentiles are quantised to that step.

## Memory

An order costs 28 bytes in the pool plus its entry in the id index. A hundred thousand live orders use about 2.8 MB of pool and roughly 1.2 MB of index. The price ladder costs 8 bytes per tick per side, so the default band of 16,384 ticks is 128 KB per side. The leaks tool reports zero on a non sanitised build.

## Correctness

172 tests run in three configurations: debug with AddressSanitizer and UndefinedBehaviorSanitizer, an optimised build with checks on, and the release build. The five golden files have stayed byte identical through every change to the storage layer. The benchmark checksum has not moved since the first measurement.

## Building

```
cmake --preset release
cmake --build --preset release -j8
ctest --preset release
./build-release/apps/bench --reps 9
```

Requires a C++26 compiler. Developed against Homebrew Clang 22 with libc++. The hardware counter path needs root and only works on Apple Silicon; everything else is portable.

## Where this is going

The engine is the foundation; the rest of a trading system gets built on top of it, in roughly this order.

Market data ingestion comes first. A feed adapter for CME order by order data through Rithmic, a recorder that writes every message to disk in a compact fixed width format, and a replayer that feeds those recordings back through the engine so that live and historical runs share the same code path.

Then a proper backtester. The fill simulator gains latency injection, so an order decided at time T arrives at T plus your real network delay, and markout tooling to measure whether passive fills were adversely selected.

Then research. With order by order recordings across several instruments and a replay speed of tens of millions of events per second, the plan is to run the same battery of microstructure measurements cross sectionally and let the data say where an edge exists, rather than picking a strategy up front.

After that, a risk layer with hard limits and a kill switch, and deployment on a Linux server near the exchange, where the tail latency numbers matter far more than the median.

## Documents

docs/M3-REPORT.md is the performance engineering report. It covers how each of the three data structures was chosen, every hypothesis that turned out to be wrong, and the measurement method. docs/benchmarks/log.md is the full measurement history, failures included.

## License

This is a personal project and is published so that it can be read. All rights are reserved. It is not licensed for reuse, redistribution or commercial use.
