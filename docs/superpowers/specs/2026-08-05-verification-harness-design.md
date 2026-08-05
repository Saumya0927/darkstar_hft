# darkstar_hft — Milestone 2: Correctness Verification Harness

**Date:** 2026-08-05
**Package:** `darkstar_hft` (namespace `dhft`), repo `/Users/saumyapatel/Dark Star Technologies/Random/CPP_Project`
**Follows:** Milestone 1 (order book + matching engine, 46 tests green)

## Goal

Prove the matching engine is correct on inputs nobody wrote by hand, and build the
safety net that makes the future low-latency rewrite safe rather than speculative.

## Why this milestone exists

M1's evidence of correctness is 46 hand-written tests. They only cover cases we thought
of. Two problems follow:

1. **Unknown unknowns.** A subtle FIFO or partial-fill bug in an untested interleaving
   would sit silently in the engine.
2. **The rewrite is unsafe.** M3 will replace `std::map`/`std::list` with flat arrays and
   object pools (see the M1 performance review). That is a large, subtle change. Without
   an independent oracle there is no way to prove the fast version behaves identically to
   the version we trust today.

M2 fixes both. Its output is not a feature; it is confidence.

## Working mode

Same COACH MODE as M1, with an agreed split:

- **The user writes the core engineering:** `OrderBook::validate()` (Layer 1), the
  property assertions (Layer 3), `OutEvent::operator==` and the reference model
  (Layer 4).
- **The assistant writes the test plumbing:** the assertion macro, the golden-file
  harness, the random-script generator, and the equivalence driver.

The assistant does not write the user's core code. Reviewing and correcting it is wanted.

## Scope

**In scope:** invariant validation, golden-file regression tests, property-based
randomized testing, and a brute-force reference model with an equivalence test.

**Out of scope (deferred to M3):** benchmarking, latency measurement, and all
optimization. Also out of scope: new order types, threading, real market data.

---

## Architecture

Four layers, each catching what the previous cannot:

```
  Layer 4  reference model      is the engine's OUTPUT right?      (independent oracle)
  Layer 3  property testing     does it hold on random inputs?     (unknown unknowns)
  Layer 2  golden files         did behavior change accidentally?  (regression)
  Layer 1  invariants           is internal state self-consistent? (foundation)
```

Layer 1 is the foundation: Layers 3 and 4 both call it after every event, so a corrupt
book is caught at the exact event that corrupted it rather than surfacing later as a
wrong answer.

New build targets, following the `io/` module pattern established at the end of M1:

```
  dhft            core engine                    (unchanged)
  dhft_io         text I/O                       (unchanged)
  dhft_reference  brute-force reference model    NEW, test-only
  dhft_testkit    generator + harness plumbing   NEW, test-only
```

The core library gains exactly one new public method (`OrderBook::validate()`) and one
new header (`Check.h`). Everything else lives outside the engine.

---

## Layer 1 — Invariants

### `include/dhft/Check.h`

A minimal assertion facility. `DHFT_CHECK(cond)` evaluates `cond`; on failure it prints
the failing expression with file and line to `stderr` and calls `std::abort()`.
`DHFT_CHECK_MSG(cond, msg)` adds a message. The failure path is a `[[noreturn]]`
function so the compiler knows control does not continue.

Rationale for abort rather than an exception: a violated invariant means the book's state
is already corrupt, so unwinding and continuing would propagate corruption. Stopping at
the point of damage is the correct behavior, and it is what the M1 spec called for.
(C++26 contracts would be the modern mechanism but are not implemented in clang.)

### `OrderBook::validate()`

```cpp
[[nodiscard]] std::expected<void, std::string> validate() const;
```

Returns success, or a human-readable description of the **first** violation found. It
returns rather than aborts so tests can assert on it and so callers choose the policy;
`DHFT_CHECK(book.validate().has_value())` is the aborting form.

It must be a member because the invariants span private state (`bids_`, `asks_`,
`index_`).

**Invariants checked:**

1. **Not crossed.** If both sides are non-empty, `best_bid < best_ask`.
2. **No empty levels.** Every level present in either map has a non-empty order list.
3. **Positive quantities.** Every resting order has `qty > 0`.
4. **Index completeness.** `index_.size()` equals the total number of orders across all
   levels of both sides.
5. **Index correctness.** For every order in every level, `index_` contains its id, and
   that entry's `side`, `price`, and node all match where the order actually sits.
6. **Field agreement.** Every order's `price` equals its level's price, and its `side`
   matches which map it is in.
7. **FIFO integrity.** Within a level, orders appear in strictly ascending `seq` order.

Invariant 7 is the one that directly tests time priority, and is the reason `Sequence`
is stored on each order.

Cost is O(total orders), so it is a test-time tool, not something the hot path calls.

---

## Layer 2 — Golden-file regression

For each pair `test/golden/<name>.script` and `test/golden/<name>.expected`, a test
parses the script, runs it through the engine with `dhft::io::TextSink`, appends a dump
of the final book, and compares the whole text to the `.expected` file. Any difference
fails the test and prints a diff-style report.

**Regeneration:** running the test binary with the environment variable
`DHFT_UPDATE_GOLDEN=1` set rewrites the `.expected` files instead of comparing. This is
the standard workflow — after an intentional behavior change you regenerate, then read
the diff in `git diff` to confirm the change is what you meant.

**Book dumping:** `dhft_io` gains `dump_book(const OrderBook&, std::ostream&)` so the
golden captures final state, not just the event stream. The demo app reuses it.

Golden scripts to include: a basic two-sided book, a multi-level sweep, a cancel/modify
sequence, an all-cross liquidation, and an edge-case script (cancel unknown, modify to
zero, self-crossing prices).

**Path handling:** the test locates its data via a compile-time path injected by CMake
(`target_compile_definitions`), so the tests do not depend on the working directory.

---

## Layer 3 — Property-based randomized testing

### Generator

`dhft_testkit` provides a random script generator:

```cpp
struct GenConfig {
    std::uint64_t seed;
    int events;            // how many input events to produce
    std::int64_t minPrice, maxPrice;
    std::int64_t maxQty;
    int pctNew, pctCancel, pctModify;   // mix, must sum to 100
};
[[nodiscard]] std::vector<InEvent> generate(const GenConfig& cfg);
```

It tracks which ids are live so cancels and modifies mostly target real orders (a
generator that only produced rejects would test nothing). It is seeded and fully
deterministic: the same config always produces the same script, so any failure replays
exactly. On failure, tests print the seed.

### Properties asserted

Over many random scripts:

- **P1 — Invariants.** `validate()` succeeds after every single event.
- **P2 — No over-fill.** No order's cumulative traded quantity exceeds the quantity it
  was submitted with (accounting for modifies).
- **P3 — Limit respected.** Every trade price satisfies the aggressor's limit: for a buy
  aggressor `trade.price <= limit`, for a sell `trade.price >= limit`.
- **P4 — Quantity conservation.** Track expected total resting quantity incrementally
  (adds increase it, fills and cancels decrease it, modifies adjust it) and assert it
  equals the book's actual total after every event. Nothing is created or lost.
- **P5 — Determinism.** The same generated script run twice produces byte-identical
  output.

---

## Layer 4 — Reference model

### `dhft_reference`

A second matching engine that is deliberately slow and obviously correct:

```
  NaiveEngine
    std::vector<Order> resting_;      // that is the entire data structure
    best price   -> linear scan for min/max on the relevant side
    front at px  -> linear scan for the lowest seq at that price
    cancel       -> linear scan, erase
    modify       -> linear scan, adjust in place or erase+push_back
```

No index, no maps, no iterator tricks. It is O(n) everywhere and it does not matter. Its
only job is to be so simple that it is hard to get wrong.

It emits the same `OutEvent` stream into the same `Sink` interface, so both engines are
driven identically.

### Equivalence test

```
                 ┌──▶ [ MatchingEngine ] ──▶ events A
   same input ───┤                                      assert A == B, event by event
                 └──▶ [ NaiveEngine    ] ──▶ events B
```

Run over the golden scripts and over many random scripts from Layer 3. Any divergence
prints the seed, the event index, and both events.

This requires `OutEvent::operator==`, a defaulted memberwise comparison added to
`Events.h`.

**Why this is the milestone's most valuable artifact:** in M3 the fast engine's internals
get replaced wholesale. The reference model does not change. If the two still agree on
thousands of random scripts after the rewrite, the rewrite preserved behavior. That is
the difference between refactoring and gambling.

---

## Repository layout after M2

```
CPP_Project/
  include/dhft/        core headers (+ Check.h)
  src/                 core engine
  io/                  text I/O module (+ dump_book)
  reference/           brute-force reference model      NEW
    include/dhft/reference/NaiveEngine.h
    src/NaiveEngine.cpp
  testkit/             generator + harness plumbing     NEW
    include/dhft/testkit/Generate.h
    include/dhft/testkit/Golden.h
    src/Generate.cpp
    src/Golden.cpp
  apps/                demo
  test/
    golden/            .script and .expected pairs      NEW
    validate_test.cpp  property_test.cpp  golden_test.cpp  equivalence_test.cpp
  data/scripts/
  docs/
```

---

## Testing plan

New test executables:

- `validate_test` — `validate()` accepts a well-formed book; each invariant is exercised
  by a scripted sequence that would violate it if the engine were wrong.
- `golden_test` — every `.script`/`.expected` pair matches.
- `property_test` — P1 through P5 over many seeds.
- `equivalence_test` — fast engine and reference agree on golden scripts and on many
  random scripts.

All continue to run under AddressSanitizer and UndefinedBehaviorSanitizer.

---

## The C++ curriculum in this milestone

| Piece | C++ learned |
|---|---|
| `Check.h` | preprocessor macros, `__FILE__`/`__LINE__`, stringification, `[[noreturn]]`, `std::abort`, conditional compilation |
| `validate()` | iterating nested containers, cross-referencing two structures, `std::expected` with a payload, building diagnostic strings, `const` traversal |
| Golden harness | `std::filesystem`, reading/writing files, environment variables, CMake `target_compile_definitions` |
| Generator | `<random>` engines vs distributions, deterministic seeding, reproducible test data |
| Properties | invariant reasoning, accounting/conservation logic, writing assertions that hold for *all* inputs rather than one |
| Reference model | designing to an interface, test oracles, deliberately choosing the naive algorithm |
| `operator==` | defaulted comparison on an aggregate (ties back to Task 1's `operator<=>`) |

---

## Success criteria

1. `validate()` passes after every event of every test, golden, and random script.
2. Golden files exist for at least five scripts and all match; `DHFT_UPDATE_GOLDEN=1`
   regenerates them.
3. Property tests run at least 500 random scripts with all five properties holding.
4. The fast engine and the reference model produce identical output on every golden
   script and on at least 500 random scripts.
5. Everything clean under ASan and UBSan.
6. **The user wrote `validate()`, the properties, and the reference model**, and can
   explain each.
