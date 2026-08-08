# darkstar_hft — T8: the order-id map

**Date:** 2026-08-08
**Package:** `darkstar_hft` (namespace `dhft`)
**Follows:** T5+T6 (pooled records, commit 0a91b60) and the tail diagnosis (commit 9a62f91)

## Goal

Replace `std::unordered_map<OrderId, std::uint32_t> index_` with a structure that is fast
on the hot path, has no periodic stall, and works with the order ids a real exchange feed
actually carries.

## Why this task, and why now

Not a hypothesis. Measured.

T5+T6 removed every per-order `malloc` and the worst case did not move. The cause was
found by experiment: **the id map rehashing.** `index_.reserve()` alone, changing nothing
else, gave **-19.5% throughput and -90.3% worst case**, both with U=0, p=0.000155 and
complete separation between the sample groups.

Two competing hypotheses were falsified first and are recorded in `docs/benchmarks/log.md`:
the pool's own vector growth (no effect), and OS preemption (implied clock 3.09 GHz against
a 3.20 GHz maximum, so the thread held the core ~97% of the time).

## Verified facts this design rests on

Each was checked, not assumed.

### Real exchange order ids are sparse 64-bit, not dense counting numbers

Nasdaq ITCH order reference numbers are 64-bit, and an L3 book "can contain millions of
orders at times." CME assigns its OrderID sequentially **across all orders on the venue**,
so for any single instrument the ids observed are large and full of gaps.

**This rules out a flat array indexed by `id - base`**, which was the originally specced
T8. It works only because the test generator issues ids 1, 2, 3. It would have to be
thrown away the moment a real feed arrived, and the whole point of this project is that it
eventually connects to one.

### Real book builders use a dense hash map

A published ITCH order-book implementation found the bulk of its time in hash map
insert/delete, and replacing `std::unordered_map` with `google::dense_hash_map` cut runtime
by over 30% - the same structure and the same conclusion, reached independently.

### Tombstones would poison this table

Cancels dominate a real feed. Tombstone-based deletion never reclaims slots, so the table
degrades until a rehash is forced - reintroducing the exact stall being removed. Deletion
must be **backward-shift**, which in turn requires linear/robin-hood probing.

### Measured: `ankerl::unordered_dense` beats `std::unordered_map` on this access pattern

400,000 operations, 60% insert / 25% erase / 15% find, steady state held at 27,600 live
entries to match the engine. Best of three runs:

| configuration | ns/op | p99 | p99.9 | worst |
|---|---|---|---|---|
| `std::unordered_map`, no reserve (today) | 33.9 | 83 | 83 | 107167 |
| `std::unordered_map`, reserved | 29.7 | 83 | 125 | 10958 |
| `ankerl::unordered_dense`, no reserve | 16.0 | 42 | 42 | 56542 |
| **`ankerl::unordered_dense`, reserved** | **14.8** | **42** | **42** | **250** |

**2.3x faster per operation, and a worst case ~430x smaller.** The library's documented
"deletion requires two lookups and is relatively slow" caveat did not bite at a 25% erase
rate.

## Decision: use the library first

**Adopt `ankerl::unordered_dense`. Hand-roll only if it fails to deliver.**

The M3 plan says "hand-roll first, then A/B against a mature library." That was written
when this was primarily a C++ learning exercise. The stated goal is now a system that could
carry real money, and that changes the calculus:

- The library already implements exactly the design this spec would have specified: open
  addressing, robin-hood probing, **backward-shift deletion**, `reserve()`, wyhash with
  proper avalanching, MIT licence, no SIMD paths so it is architecture-neutral on ARM64.
- Backward-shift wraparound is genuinely error-prone. martinus - the author, whose maps are
  among the most benchmarked in C++ - **shipped a wraparound bug in `erase`**
  (robin-hood-hashing issue #42). A silent bug here corrupts order lookup, which in a live
  system means acting on a book you do not actually have.

Hand-rolling remains a worthwhile exercise later, with the library available as a working
oracle to diff against. It is not the right thing to ship first.

## Integration facts

Verified against the repository, not the README.

- **Version to pin: `v4.9.0`** (latest tag). Do not track `main`.
- **Not header-only on `main`.** `include/ankerl/unordered_dense.h` now `#include "stl.h"`,
  a companion header. The README's "requires compilation" note is accurate. Use CMake
  rather than vendoring a single file.
- **C++17 minimum.** This project is C++26, so no constraint.
- **MIT licence.**
- **CMake:** `FetchContent`, following the pattern already used for GoogleTest, then link
  `unordered_dense::unordered_dense`.
- **API is `std::unordered_map`-compatible** for everything `OrderBook` uses:
  `try_emplace`, `find`, `erase`, `contains`, `size`, `reserve`, `end`.

### The custom hash

`OrderId` is a strong type, so it needs a hash specialisation. The library's own hash is
specialised, not `std::hash`:

```cpp
template <> struct ankerl::unordered_dense::hash<dhft::OrderId> {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const dhft::OrderId& id) const noexcept -> std::uint64_t {
        return ankerl::unordered_dense::detail::wyhash::hash(id.v);
    }
};
```

`is_avalanching` tells the library the hash is already high quality so it skips its own
extra mixing. Marking it wrongly costs correctness of distribution, not compilation - so it
must be justified, and wyhash is what the library itself uses for integers.

**This finally resolves the hash-mixing question.** Early in this project a murmur3
finalizer was added to `std::hash<OrderId>` and measured **7.7% slower**, so it was
reverted. The recorded reason: libc++ uses *prime* bucket counts, where identity hashing of
sequential ids is already optimal. An open-addressed table is the opposite - it selects
slots from the **low bits** - so mixing becomes necessary. The revert was correct; the idea
was simply premature. `std::hash<dhft::OrderId>` stays as it is, because other code may
still use it.

## Capacity policy

**Reserve to a declared capacity at construction. Grow if exceeded; never abort.**

An earlier draft of this design proposed `DHFT_CHECK` on exhaustion. That was wrong.
Killing the process because the book got busy is worse than a rare stall, and real engines
reject orders at capacity rather than crashing - which would be a behaviour change
requiring `NaiveEngine` to adopt the identical rule.

- Capacity is an explicit `OrderBook` construction parameter with a documented default, not
  a magic number buried in the class.
- Exceeding it rehashes once and continues. Rare by construction.
- **Count growth events** so the log can show whether it ever happened.

## Scope

```
CMakeLists.txt            FetchContent for unordered_dense, link into dhft
include/dhft/OrderBook.h  index_ type; capacity parameter; the hash specialisation
src/OrderBook.cpp         14 use sites across add / contains / cancel / modify /
                          take_from_front / validate - mechanical, the API is compatible
test/                     0 changes
NaiveEngine               0 changes
golden files              0 changes - MUST stay byte-identical
```

Plus a new `test/idmap_test.cpp` covering the hash specialisation and the capacity policy.

## The hazard to design around

`std::unordered_map::erase` invalidates **only** the erased element. Open-addressed maps
invalidate **everything** on any erase, because entries physically move.

`add()` currently holds an iterator across other work and then writes through it:

```cpp
auto [entry, inserted] = index_.try_emplace(o.id, kNull);
...                                    // alloc_slot(), m[o.price], link_back()
entry->second = idx;
```

This is safe today only because nothing in between mutates the map. It is silently fragile:
reordering one line turns it into memory corruption with no diagnostic. **`add()` must be
restructured so no map iterator is held across other work** - erase by key on the failure
path, and insert the final value without reusing a stale handle.

## Risks

1. **A new third-party dependency** in the core engine, which until now had none. Mitigated
   by: MIT, pinned tag, `FetchContent` (no vendored code), and a documented fallback to
   hand-rolling.
2. **Beyond cache at real scale.** 27,600 orders is ~0.8 MB and fits in the 12 MB L2. One
   million orders is ~24 MB and does not. At production volumes every lookup becomes a
   memory access and no map design changes that. This bounds what T8 can achieve on live
   data.
3. **The gain may be smaller in the engine than in the probe.** The probe isolates the map;
   the engine does other work per event. Expect **at least** the reserve result (-19.5%).
4. **The map may stop being the bottleneck.** Counters put branch misses at 4.19/event,
   roughly 27% of the 219-cycle budget. That is the likely next target, and T8 should not
   be presented as the last word.

## Verification gate

Unchanged from M3, and non-negotiable:

1. 143/143 tests in all three build configurations.
2. `MatchingEngine` and `NaiveEngine` agree on every golden script and all random scripts.
3. `validate()` holds after every event of every property run.
4. **The five golden files are byte-identical.** This is the strongest single check that
   behaviour did not change, and it has caught nothing so far precisely because nothing has
   changed - which is the point.
5. The benchmark checksum is unchanged.
6. `leaks(1)` reports zero on a non-sanitised build.
7. The measured improvement exceeds the 6.08% noise floor and survives a Mann-Whitney U
   test at alpha 0.05, measured as a same-session A/B rather than against a logged number.

## Success criteria

1. `index_` is an open-addressed map, reserved to a declared capacity, with no rehash during
   a normal run.
2. Throughput improves by at least the reserve experiment's -19.5%, with statistics.
3. The worst case stays in the tens of microseconds rather than the hundreds.
4. No stale-iterator hazard remains in `add()`.
5. The benchmark log carries a before/after entry with an explicit keep-or-revert verdict.
6. Nothing outside `OrderBook.h`, `OrderBook.cpp` and `CMakeLists.txt` changes.

## Explicitly out of scope

- **Engine-assigned handles / the gateway-core split.** Real exchanges keep client-id
  resolution out of the matching core entirely, and that is the right long-term
  architecture. It changes the public API and belongs in its own milestone, not in an M3
  data-structure task.
- **Hand-rolling the map.** Deferred, with the library as the oracle.
- Anything touching the price ladder (T9) or branch behaviour (T10).
