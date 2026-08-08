# darkstar_hft — T9: the price ladder

**Date:** 2026-08-08
**Package:** `darkstar_hft` (namespace `dhft`)
**Follows:** T8 (open-addressed id map, commit 4fce4bc)

## Goal

Replace `std::map<Price, Level>` on each side of the book with an array indexed by price,
keeping a `std::map` only for prices outside the array's range. No behaviour change.

## Why, with the evidence

The price maps are the last untouched structure. Measured on the engine's access pattern at
its real ~101-level steady state, 350000 operations, best of two runs:

| level churn | `std::map` | flat array + bitmap | ratio |
|---|---|---|---|
| 25% | 39.9 ns/op | 8.5 | 4.7x |
| 10% | 23.5 | 6.3 | 3.7x |
| **2% (roughly this engine)** | **18.6** | **5.6** | **3.3x** |
| 0% (pure lookup, no allocation) | 17.3 | 5.5 | 3.1x |

The first probe used 25% churn and was **not representative** - your book holds 100-103
levels across 350000 events, so level creation and destruction is rare. Re-measured across
churn rates: even at **zero** churn the array wins 3.1x, so this is not an allocation
artefact. Walking a red-black tree genuinely costs about 12 ns more per operation than
indexing an array.

### Why T4 measured nothing and this should

T4 removed roughly one price-map lookup **per fill** and measured zero. That looked like
evidence against this task. It is not:

- Fills are a **minority** of events. The book grows from 19338 to 151874 in quantity over
  the measured window, so most orders rest rather than trade.
- If fills are around 10% of events, removing 17 ns from 10% of them is under 2 ns/event
  against a then-80 ns baseline - **comfortably inside the 6.08% noise floor.**
- T9 removes price-map cost from **every** event: every add, every cancel, every best-price
  query.

Different denominators, not a contradiction.

### It should also help the branch problem

Counters after T8 put branch misses at **2.97/event, roughly 31% of the 136-cycle budget** -
the largest identified remaining cost. A red-black tree walk is precisely the
unpredictable-branch pattern: each step is a comparison the predictor cannot learn. An array
index and a `countr_zero` have no such branches. T9 may therefore deliver part of what T10
was going to chase, and T10 should be re-scoped only after T9 is measured.

## Decision: array plus a map tail (option B)

An array covers a fixed price band. Prices outside it fall back to a `std::map`.

Four options were considered:

| option | what happens out of range | why not |
|---|---|---|
| A. reject the order | new `RejectReason` | **behaviour change** - `NaiveEngine` must adopt the identical rule, and the golden files change |
| **B. array + map tail** | falls back to the map | **chosen** |
| C. slide the window | array re-centres as the market moves | re-centring is a large copy at an unpredictable moment - the exact stall T8 removed |
| D. one huge array | covers everything | spans far past cache; back to memory latency |

**Real exchanges do A and C together.** CME's matching engine rejects orders outside a price
band, and the band is recalculated dynamically from the last price. That is genuine
production practice.

**It is still wrong for this project.** The stated destination is a system that consumes a
live exchange feed. A **book builder cannot reject anything** - the feed states what
happened and the book's job is to represent it. Refusing a price makes the book wrong. So A
is a dead end here despite being what exchanges do, and C is premature: the window would be
sized against a synthetic generator rather than real market data.

**The decisive argument for B is not speed.** B changes no behaviour at all. Every price
still works exactly as today. Therefore:

- `NaiveEngine` needs no change
- the five golden files must stay **byte-identical**
- the verification gate that caught every mistake in T5 through T8 stays fully intact

**And B degrades gracefully.** If the market leaves the band entirely, every lookup goes to
the map and performance returns to today's - never worse.

## What the code actually requires

Read off the call sites, not assumed. The side container must support exactly six
operations:

| operation | call sites | hot? |
|---|---|---|
| insert-or-find a level | `add` (line 36) | yes |
| find a level | `cancel` (116), `front_at` (165), `take_from_front` (323) | yes |
| erase a level | `cancel` (121), `take_from_front` (344) | yes |
| best price | `best_bid` (64), `best_ask` (71) | yes |
| empty? | `best_bid` (61), `best_ask` (68) | yes |
| visit every level | `depth_into` (80), `total_quantity` (186), `validate` (275) | no |

### The finding that shapes the design

**Only one of the three traversal sites needs sorted order.**

- `depth_into` reports the top N levels best-first - **order matters**
- `total_quantity` sums every level - order irrelevant
- `validate` checks each level independently - order irrelevant

Merging a sorted array with a sorted map is the fiddly, expensive part of a hybrid. It is
needed in exactly **one** place, and that place is not on the hot path: `depth_into` is used
by tests, `io::dump_book`, and the benchmark's `measure_depth` (twice per run, not per
event). So the ordered merge can be simple and unhurried; the other two traversals can visit
the array and the map in any order they like.

## Architecture

A new type encapsulating the hybrid, so `OrderBook.cpp` barely changes:

```cpp
template <Side Sd, typename LevelT>
class PriceLadder {   // Side::Buy -> best is highest;  Side::Sell -> best is lowest
public:
    explicit PriceLadder(Price min, Price max);

    [[nodiscard]] LevelT* find(Price p) noexcept;         // nullptr when absent
    [[nodiscard]] LevelT& insert(Price p);                // insert-or-find
    void erase(Price p) noexcept;
    [[nodiscard]] std::optional<Price> best() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    template <typename F> void for_each(F&& f) const;           // any order
    template <typename F> void for_each_from_best(F&& f) const; // sorted, depth_into only
};
```

**Corrected after audit: the ladder must be templated on the level type.** An earlier draft
of this spec wrote `template <Side Sd>` and had the ladder hold `Level` directly. That does
not compile: `Level` is a **private nested type of `OrderBook`**, so a separate class cannot
name it. Templating on `LevelT` fixes it and is better design anyway - a ladder of things
indexed by price does not care what a level is.

Verified by compiling it: `Side` works as a non-type template parameter, and a class **can**
use its own private nested type as a template argument, so `Level` stays private.

**The tail's comparator also depends on the side**, which the first draft missed. Ordered
iteration for bids is descending and for asks ascending, so:

```cpp
using TailCmp = std::conditional_t<Sd == Side::Buy, std::greater<>, std::less<>>;
std::map<Price, LevelT, TailCmp> tail_;
```

`find` returning `LevelT*` rather than an iterator also simplifies every call site: no
comparison against `end()`, no `->second`.

`Side` as a non-type template parameter keeps bids and asks distinct types with **zero
runtime cost** - direction only affects `best()` and `for_each_from_best`. This also
preserves the existing situation where the two sides are different types, so the generic
lambdas in `OrderBook.cpp` keep working; the `detail::PriceLevelMap` concept is rewritten to
describe this interface instead of a `std::map`.

This is P.11 from *Beautiful C++* - **encapsulate messy constructs rather than spreading
them through the code.** The array, the bitmap, the cached best and the map tail live behind
one boundary; `OrderBook` never sees them.

### Internals

```cpp
std::vector<LevelT>        levels_;    // one per tick in [min, max]
std::vector<std::uint64_t> occupied_;  // bitmap, one bit per tick
std::uint32_t              bestIdx_;   // cached; kNull when the array side is empty
std::map<Price, LevelT, TailCmp> tail_; // out-of-band prices only
```

Verified against the probe: out-of-band prices, including **negative** ones, route to the
tail and are found again correctly.

## Design decisions

### The band is a constructor parameter, and the default is measured

Same reasoning as T8's capacity: a magic constant buried in the class is not a design.
`explicit PriceLadder(Price min, Price max)`, and `OrderBook` forwards it from its own
constructor with a documented default.

**The default must be chosen against what the code actually uses.** Measured:

| source | price range |
|---|---|
| unit tests | 0 .. 200 |
| golden scripts | 90 .. 105 |
| `data/scripts` | 99 .. 103 |
| generator default | 95 .. 105 |
| benchmark default | 9900 .. 10100 |

**A default band of `[0, 16383]` covers every one of them.** That matters: if the default
were too narrow, the benchmark's 9900-10100 would fall entirely into the map tail and T9
would measure no improvement at all - a silently meaningless result.

**Construction cost, measured** (both sides of one `OrderBook`):

| band | per side | ctor | across ~3000 books in the suite |
|---|---|---|---|
| 4096 | 32 KB | 1.68 us | 5 ms |
| **16384** | **128 KB** | **5.16 us** | **15 ms** |
| 65536 | 512 KB | 12.87 us | 39 ms |

`property_test` builds 3000 engines and runs in 120 ms today, so a 16384-tick default adds
roughly 12%. Acceptable. 65536 would add 33% for no benefit at these price ranges.

**Blast radius the first draft missed:** the benchmark constructs a `MatchingEngine`, not an
`OrderBook`, so if a future instrument needs a different band, `MatchingEngine`'s
constructor must forward it. Not needed for T9 because the default covers the benchmark, but
it is a real coupling and is recorded here rather than discovered later.

### The best price must be cached

**This is not optional, and my probe hid it.** The probe rescanned the whole bitmap on every
best-price query. At 201 ticks that is four 64-bit words and effectively free - which is why
it measured 5.5 ns. A production band of tens of thousands of ticks would be a linear scan
of hundreds of words on the hottest operation in the engine.

So `bestIdx_` is cached and only recomputed when the level at `bestIdx_` empties. The
recomputation scans outward with `countr_zero`/`countl_zero`.

**A stale cached best is a silent wrong answer** - the engine would match at a price with
nothing behind it. This is exactly what `validate()` exists to catch, and it gets a new
invariant below.

### `best()` must consider both structures

The overall best is the better of the array's best and the tail's best. When the tail is
empty - the normal case - this is one comparison against a cached value.

### A price is in exactly one structure

In-band prices live in the array, out-of-band in the map, decided solely by the range check.
The same price can never be in both. That is an invariant, and `validate()` checks it.

## New invariants for `validate()`

On top of the existing ones:

1. **Bitmap agreement**: bit `i` is set if and only if `levels_[i]` has a non-empty chain.
2. **Cached best is correct**: `bestIdx_` names the best occupied index, or `kNull` when the
   array side holds nothing.
3. **No overlap**: no price appears in both the array and the tail.
4. **Tail is out-of-band only**: every key in `tail_` lies outside `[min, max]`.
5. **Level count**: array-occupied plus tail size equals the number of populated levels the
   book believes it has.

Each must be **mutation-tested** - deliberately broken, confirmed to fire, restored - per
`docs/REVIEW_LOOP.md` step 3. A checker that has never failed has not been tested.

## Scope

```
include/dhft/PriceLadder.h   NEW   the class, header-only (templated)
include/dhft/OrderBook.h           bids_/asks_ types; band parameters on the constructor;
                                   rewrite the detail::PriceLevelMap concept
src/OrderBook.cpp                  ~10 call sites; m[p] -> insert(p), m.find -> find,
                                   m.erase -> erase, begin()->first -> best(),
                                   3 range-for loops -> for_each / for_each_from_best
test/price_ladder_test.cpp   NEW   unit tests for the ladder in isolation
test/CMakeLists.txt                register it
tests (existing)             0     public API unchanged
NaiveEngine                  0
golden files                 0     MUST stay byte-identical
```

Larger than T8: a new type with its own tests, rather than a container swap.

## Risks

1. **Bigger blast radius than T8.** T8 was a type swap behind an identical API. This is a new
   data structure with its own invariants. Mitigated by: the ladder is unit-tested in
   isolation first, `NaiveEngine` equivalence is untouched, and the goldens must not move.
2. **The cached best is the most dangerous piece of state in the engine.** Every other error
   so far has been caught by a test. A stale best price produces a *plausible* wrong answer.
   Invariant 2 plus mutation testing is the mitigation, and it is not optional.
3. **The measured 3.1x may not reach the engine.** The probe isolates the structure. Expect
   the direction to hold and the magnitude to be smaller - T4's lesson.
4. **Band sizing is a guess until real market data exists.** Mitigated by graceful
   degradation: an entirely out-of-band market performs like today, not worse.
5. **`for_each_from_best` is new logic** merging two sorted sequences in two directions
   (descending for bids, ascending for asks). Off-by-one and direction errors here would
   surface as wrong `depth()` output - which the golden files do check.

## Verification gate

Unchanged and non-negotiable:

1. 143/143 (plus the new ladder tests) in all three build configurations.
2. `MatchingEngine` and `NaiveEngine` agree on every golden script and all random scripts.
3. `validate()` holds after every event of every property run, including the five new
   invariants.
4. **The five golden files are byte-identical.**
5. The benchmark checksum stays `dc015ae88f2b6dd0`.
6. `leaks(1)` reports zero on a non-sanitised build.
7. Improvement exceeds the 6.08% noise floor and survives a Mann-Whitney U test at alpha
   0.05, measured as a same-session A/B.
8. Every new invariant mutation-tested.

## Success criteria

1. In-band lookups are one array index; out-of-band still works, unchanged.
2. Throughput improves measurably, with statistics.
3. Branch misses per event fall - re-measured under `sudo`.
4. `validate()` carries all five new invariants, each proven to fire.
5. The benchmark log carries a before/after entry with an explicit keep-or-revert verdict.
6. Nothing outside `PriceLadder.h`, `OrderBook.h`, `OrderBook.cpp` and the test files changes.

## Explicitly out of scope

- **A sliding or re-centring window (option C).** Correct eventually, premature now: the
  window would be sized against a synthetic generator. Revisit with real market data.
- **Rejecting out-of-band prices (option A).** Real exchange practice, wrong for a system
  intended to consume someone else's feed.
- T10 micro-tuning, which should be re-scoped only after this is measured.
