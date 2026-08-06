# Optimization research notes (for Milestone 3)

**Date:** 2026-08-05. Researched against live sources; claims below carry their evidence
quality. **Nothing here is actioned yet** — M2 (verification) comes first, then
benchmarking, then optimization. Re-verify before relying on any of it.

Target: single-threaded matching engine, C++26, Homebrew LLVM Clang 22.1.4 + libc++,
macOS arm64 (Apple M1, 128-byte cache lines, 64 KB L1d), CMake 3.31.

---

## 0. The biggest lever is not a library

If `OrderId`s are monotonic, a **slot map** — a dense vector indexed by `id - base_id`
with a free list for cancelled slots — removes hashing and probing entirely and beats
every hash table on cache behaviour. Whether this applies depends on how ids are
assigned (exchange sequence numbers usually are monotonic; client-assigned ids often are
not). Decide this before optimising a container that might be deleted.

---

## 1. Hash maps (replacing `std::unordered_map` for `index_`)

**MEASURED 2026-08-05 — do NOT mix the hash while `std::unordered_map` is in use.**
libc++ sizes its bucket array to a *prime* (102877 for 100k elements). With identity
hashing, sequential `OrderId`s land in distinct buckets: 100000 buckets used, worst chain
1. Adding a murmur3 finalizer randomises them and, by the birthday paradox, *creates*
collisions: 63813 buckets used, worst chain 7, and a measured 7.7% loss in engine
throughput. A mixed hash is only correct for a **power-of-two open-addressing** table.
Ship the mixing together with the map swap, never before it.


**No cross-library hash-map benchmark exists on ARM64.** Every credible comparative
benchmark found (martinus' 29-map suite 2022, Boost's Bannalia post, JacksonAllan's
suite) is x86-only. Published rankings do not transfer to Apple Silicon. The ARM paths
below were verified by reading each library's source, not by benchmark.

| Library | ARM64 status (source-verified) | Notes |
|---|---|---|
| `ankerl::unordered_dense` | **No SIMD anywhere** — architecture-neutral by construction | Single header, MIT, zero deps, active (pushed 2026-08). Recommended first try |
| `boost::unordered_flat_map` | Native NEON, **group width 15 on ARM and x86 alike** | Best SIMD design; but 7 transitive Boost header libs make FetchContent awkward |
| `absl::flat_hash_map` | Real NEON path, but **`kWidth` 8 on ARM vs 16 on x86** | Verified asymmetry: roughly twice the group-probe iterations. Full library build |
| `emhash5-8` | Pure scalar, no SIMD | Fastest insert/erase in the 2022 x86 suite. Single maintainer; stability undocumented |
| `folly::F14ValueMap` | Meta documents NEON as baseline on aarch64 | Strongest ARM commitment, but drags in fmt/glog/gflags/double-conversion/boost |
| `tsl::robin_map` | No SIMD | Mid-pack; `hopscotch`/`sparse` variants stale since Nov 2025 |
| **`gtl` / `parallel-hashmap`** | **Zero NEON — grepped both repos.** Falls through to the portable SWAR group abseil uses only as a last resort | **Avoid on this platform.** Verified absent, not merely unverified |

Caveat on all published numbers: Google's "2-3x" SwissTable claim and Boost's
"2.6-4.3x" are both self-published by the authors of the container being measured.
Directionally consistent across independent authors, individually untrustworthy.

---

## 2. Containers and allocation

- **`std::hive` (P0447):** in the C++26 IS, but **libc++ has not started** — tracking
  issue llvm/llvm-project#127886 open, unassigned, no PR. Do not design around it.
- **`flat_map` and `absl::btree_map` are disqualified** as `std::map` replacements here:
  both invalidate handles on insert/erase (btree_map moves values between nodes on
  rebalance). The current O(1) cancel depends on stored handles surviving. `std::map` is
  the only ordered container studied that guarantees insert never invalidates existing
  iterators and erase invalidates only the erased element.
- **`plf::list`** — closest drop-in for `std::list<Order>`: same stability contract,
  block allocation (~2048-element chunks) plus per-block free lists instead of one `new`
  per order. zlib, header-only. Author's own benchmark claims ~293% faster insertion
  (vendor-published, unverified).
- **`boost::intrusive::list` + a `std::vector<Order>` slab with index links** — larger
  potential win (removes allocation entirely, 4-byte links instead of 8-byte pointers),
  but the free-list logic is yours to write and test. Unmeasured.
- **Allocators:** `jemalloc` has documented Apple Silicon page-size crashes as recently
  as 2025; Google's modern `tcmalloc` is Linux-only per its own platform doc. `mimalloc`
  has a documented zero-code-change macOS path (`DYLD_INSERT_LIBRARIES` + Mach-O
  interpose), but its published wins are multi-threaded contention benchmarks that do
  not apply to a single-threaded hot path.
- **Bounded output buffers:** `boost::container::static_vector` (never allocates) or
  `absl::InlinedVector` / `llvm::SmallVector` for `depth()`-style top-N snapshots.

---

## 3. Struct layout — free, no dependencies

```
clang++ -Xclang -fdump-record-layouts   # exact field offsets, sizes, and holes
clang++ -Wpadded                        # warns per inserted padding byte
```
`pahole` is DWARF-based and unverified on macOS — the Clang flags above supersede it.
There is also the static analyser's `optin.performance.Padding` checker.

---

## 4. Build flags (Release config)

Live-tested against this exact toolchain:

- **`-mcpu=native` or `-mcpu=apple-m1`, NOT `-march=native`.** Measured on this machine:
  `-march=native` resolves to 12 target features, `-mcpu=native` to 25 — the difference
  includes fp16, crypto, and the perfmon bit. Both select `apple-m1` as the CPU.
  `--print-supported-cpus` lists apple-m1 through apple-m5.
- **`-O2` first; A/B against `-O3`.** Documented cases of `-O3` regressing branchy code
  (one microbenchmark 22x worse from inlining blowing past L1 instruction cache).
- **`-flto=thin` set directly** via `target_compile_options`/`target_link_options`. Do
  not use `CMAKE_INTERPROCEDURAL_OPTIMIZATION` — it emits full `-flto` on Clang
  (CMake bug 22905).
- **`-Wl,-dead_strip`** — the Mach-O equivalent of `--gc-sections`, and not on by
  default. `-ffunction-sections`/`-fdata-sections` are no-ops on Mach-O.
- **Do not use `-fomit-frame-pointer`** — the arm64 ABI mandates frame records and
  Clang keeps them deliberately; dropping them breaks Instruments symbolication.
- `-fno-exceptions`/`-fno-rtti` are modest, safe wins if the codebase stays free of
  both. `-ffast-math` is irrelevant to integer/pointer code.
- **PGO:** instrumentation only (`-fprofile-generate` -> `llvm-profdata merge` ->
  `-fprofile-use`). Sampling PGO / AutoFDO requires Linux `perf` and has no macOS path.

---

## 5. Benchmarking and profiling on Apple Silicon

- **Benchmark libraries:** `nanobench` (single header, autotuning, flags unstable runs)
  for micro-benchmarks; `google/benchmark` (already Homebrew-installed) for whole-engine
  throughput with JSON export. **Neither reports P50/P99 natively** — capture raw
  per-iteration samples and compute percentiles yourself.
- **Profilers that actually work here:**
  - `xcrun xctrace record --template 'Time Profiler' ...` — fully supported, scriptable,
    no SIP changes. The default choice.
  - Instruments **CPU Counters** template (kperf-backed) — the only route to cache-miss
    data. Limited to roughly 8-10 simultaneous counters; a March 2026 report describes
    `kpc_set_config` failing for configurable counters on M4 (unverified for other chips).
  - `sample`(1) for quick triage.
- **Dead ends:** Valgrind/callgrind does not run natively on arm64 macOS. `dtrace`
  requires a partial SIP carve-out and has reported M1 stability issues.
- **Unresolved:** there is **no way to pin CPU frequency on Apple Silicon** (no
  `cpupower` equivalent), so thermal/DVFS variance is unavoidable benchmark noise. QoS
  classes influence P-core placement but do not guarantee affinity.
- **Methodology:** use `benchmark::DoNotOptimize`/`ClobberMemory` against dead-code
  elimination; warm up before measuring; build the replay harness **open-loop** (fixed
  send rate) to avoid coordinated omission, which otherwise erases exactly the tail
  events that matter.

---

## Suggested M3 order

1. Padding audit (free, zero deps).
2. Decide the slot-map question — it may delete the hash-map problem entirely.
3. Stand up Release config + benchmark harness. Measure. **Only then** change anything.
4. Cheapest structural wins first: `plf::list` for per-level storage; hash map swap if
   still warranted.
5. Bigger redesigns (intrusive list over a slab, array-indexed price ladder) last, each
   gated by the M2 reference model proving behaviour is unchanged.
