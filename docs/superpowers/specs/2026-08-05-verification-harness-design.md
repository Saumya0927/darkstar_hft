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

**In scope:** one prerequisite fix to M1 (below), invariant validation, golden-file
regression tests, property-based randomized testing, and a brute-force reference model
with an equivalence test.

**Out of scope (deferred to M3):** benchmarking, latency measurement, and all
optimization. Also out of scope: new order types, threading, real market data.

---

## Prerequisite: fix sequence semantics on requeue

**This must land before Layer 4, and it changes M1 code.**

`OrderBook::modify` currently handles a quantity increase by copying the resting order,
cancelling it, and re-adding it at the back of the level. The copy retains the order's
**original** `Sequence`. Measured result: after adding orders with sequences 1, 2, 3 at
one price and increasing the first, the level reads front-to-back as seq 2, 3, 1 — list
position and sequence number disagree.

Today this is latent: `MatchingEngine` derives FIFO from *list position*, and never reads
`seq`. It becomes fatal in Layer 4. `NaiveEngine` stores everything in one flat
`std::vector` with no per-level list, so its only expression of time priority is *lowest
`seq` at that price*. With the two engines disagreeing about who is at the front of a
level, the equivalence test reports divergence on correct code.

**Resolution:** an order that loses queue position must receive a fresh, higher sequence.
This also matches real exchange behaviour — losing your place in the queue means your
effective arrival time is now. `OrderBook` does not own a sequence counter, so the
signature becomes:

```cpp
[[nodiscard]] std::expected<void, RejectReason> modify(OrderId id, Quantity newQty,
                                                       Sequence newSeq);
```

`MatchingEngine` passes `next_` and advances it. A quantity *decrease* keeps both the
position and the original sequence; only the increase path re-stamps.

After this change, "within a level, `seq` is strictly ascending" becomes true and is
checkable — it is the invariant that directly tests time priority, and the reason
`Sequence` is stored on each order.

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
`index_`). Adding it requires `#include <string>` in `OrderBook.h`.

**Structural invariants checked** (all are properties the container itself must maintain,
regardless of who is driving it):

1. **No empty levels.** Every level present in either map has a non-empty order list.
2. **Positive quantities.** Every resting order has `qty > 0`.
3. **Index completeness.** `index_.size()` equals the total number of orders across all
   levels of both sides.
4. **Index correctness.** For every order in every level, `index_` contains its id, and
   that entry's `side`, `price`, and node all refer to where the order actually sits.
5. **Field agreement.** Every order's `price` equals its level's price, and its `side`
   matches which map it is in.
6. **FIFO integrity.** Within a level, orders appear in strictly ascending `seq` order.
   Depends on the prerequisite fix above; without it this is false after a modify-increase.

**Deliberately NOT checked here: "the book is not crossed."** `OrderBook` is a passive
container with no matching logic — a caller may legitimately add a bid above an ask, and
`orderbook_test` uses the book directly. Non-crossing is a guarantee of the *matching
algorithm*, so it is asserted at the engine level in Layer 3, not inside `validate()`.

**Implementation note.** `validate()` is `const`, so iterating the maps yields
`const_iterator`s while `Location::node` is a non-const `Level::iterator`. Comparing
element *addresses* (`&*loc.node == &order`) is the simplest way to satisfy invariant 4
without const gymnastics.

Cost is O(total orders), so it is a test-time tool, not something the hot path calls.

### `OrderBook::total_quantity()`

```cpp
[[nodiscard]] Quantity total_quantity(Side side) const noexcept;
```

Sums the resting quantity across every level on a side. Required by property P4
(conservation) in Layer 3, which has no other way to read the book's total —
`depth(side, n)` reports only the top `n` levels.

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
golden captures final state, not just the event stream. `apps/demo.cpp` currently has its
own `print_book` in an anonymous namespace; that function moves into `dhft_io` and the
demo calls the shared one, so the demo and the goldens can never drift apart.

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

Over many random scripts. Note that `OutEvent` carries neither the aggressor's limit
price nor the original order size, so the property checks correlate the **output stream
against the generated input stream** — the test holds both.

- **P1 — Invariants.** `validate()` succeeds after every single event.
- **P2 — No over-fill.** No order's cumulative traded quantity exceeds the quantity it
  was submitted with (adjusted by any modify). Each `Trade` fills **two** orders — the
  aggressor (`OutEvent::id`) and the resting order (`OutEvent::resting`) — so both ids
  accumulate `trade.qty`.
- **P3 — Limit respected.** Every trade price satisfies the aggressor's limit, taken from
  the originating `InEvent`: for a buy aggressor `trade.price <= limit`, for a sell
  `trade.price >= limit`.
- **P4 — Quantity conservation.** Track expected total resting quantity incrementally
  (adds increase it, fills and cancels decrease it, modifies adjust it) and assert it
  equals `total_quantity(Buy) + total_quantity(Sell)` after every event. Nothing is
  created or lost.
- **P5 — Determinism.** The same generated script run twice produces byte-identical
  output.
- **P6 — Book never crosses.** After every event, if both sides are non-empty,
  `best_bid < best_ask`. (Lives here rather than in `validate()` because it is a property
  of the matching algorithm, not of the container.)

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
    modify       -> linear scan; decrease adjusts in place, increase re-stamps seq
```

No index, no maps, no iterator tricks. It is O(n) everywhere and it does not matter. Its
only job is to be so simple that it is hard to get wrong.

Because it has no per-level list, **time priority is expressed purely as "lowest `seq` at
that price."** This is precisely why the prerequisite sequence fix is required: without
it, the two engines would disagree about queue order after any modify-increase.

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
- `property_test` — P1 through P6 over many seeds.
- `reference_test` — the reference model alone satisfies the same matching semantics as
  the M1 suite. `matching_test.cpp` names `MatchingEngine` concretely, so it cannot be
  reused as-is; either convert it to a gtest `TYPED_TEST` over both engine types, or
  duplicate the cases in a dedicated file. Typed tests are preferred — one set of
  semantics, two implementations.
- `equivalence_test` — fast engine and reference agree on golden scripts and on many
  random scripts.

`test/CMakeLists.txt`'s `dhft_add_test` helper currently forwards `ARGN` as link
libraries only; it needs extending so `golden_test` can receive the golden-directory path
via `target_compile_definitions`.

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

1. The prerequisite sequence fix is in, and `modify`-increase re-stamps the sequence so
   list position and `seq` agree.
2. `validate()` passes after every event of every test, golden, and random script.
3. Golden files exist for at least five scripts and all match; `DHFT_UPDATE_GOLDEN=1`
   regenerates them.
4. Property tests run at least 500 random scripts with all six properties holding.
5. The fast engine and the reference model produce identical output on every golden
   script and on at least 500 random scripts.
6. Everything clean under ASan and UBSan.
7. **The user wrote the sequence fix, `validate()`, `total_quantity()`, the properties,
   and the reference model**, and can explain each.
