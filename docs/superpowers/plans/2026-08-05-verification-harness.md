# Milestone 2 — Verification Harness: Learning Curriculum

> **COACH MODE, with an agreed split.** The user writes the core engineering:
> `validate()` (T2), the properties (T5), and `operator==` + the reference model (T6).
> The assistant writes the plumbing: the assertion macro (T1), the golden harness (T3),
> the generator (T4), the equivalence driver (T7), and all CMake. For user tasks the
> assistant supplies the concept, the interface, and a failing test — never the
> implementation. Lessons run interactively; there is no subagent handoff.

**Goal:** prove the engine correct on inputs nobody wrote by hand, and build the oracle
that makes M3's rewrite safe.

**Spec:** `docs/superpowers/specs/2026-08-05-verification-harness-design.md`

## Global Constraints (inherited from M1, still binding)

- Prices are integer ticks. Strong types everywhere. `enum class`. Brace-init.
- Hot path stays `noexcept`; no exceptions in matching. `std::expected` for failures.
- Rule of Zero for value types; no naked `new`/`delete`.
- Namespace `dhft`; sub-modules use `dhft::io`, `dhft::reference`, `dhft::testkit`.
- No emojis. No comments added to the user's files by the assistant.
- Everything runs under ASan + UBSan.
- Test-only code lives outside the core `dhft` library.

## Build targets after this milestone

```
dhft            core engine       (gains Check.h + OrderBook::validate)
dhft_io         text I/O          (gains dump_book)
dhft_reference  NaiveEngine       NEW, test-only
dhft_testkit    generator/golden  NEW, test-only
```

---

## Task 1: `Check.h` — the assertion macro   [ASSISTANT]

**Deliverable:** `include/dhft/Check.h` with `DHFT_CHECK(cond)` and
`DHFT_CHECK_MSG(cond, msg)`; on failure print expression + file + line to `stderr` and
`std::abort()`.

**Why the assistant writes it:** macros are fiddly preprocessor mechanics, not engine
logic. The assistant explains every line — stringification (`#x`), `__FILE__`/`__LINE__`,
why the body is wrapped in `do { } while (false)`, and `[[noreturn]]` — so the user can
read and modify it.

**Done when:** a scratch test confirms it aborts on false and is silent on true.

---

## Task 2: `OrderBook::validate()`   [USER]

**Deliverable:**
```cpp
[[nodiscard]] std::expected<void, std::string> validate() const;
```
Returns success, or a description of the first violation.

**Concept:** an invariant is a statement that must be true of the book's state at all
times, no matter what sequence of events led there. Checking them after every event turns
"wrong answer six events later" into "abort at the event that broke it."

**The seven invariants** (each becomes a check in the body):
1. Not crossed — if both sides non-empty, `best_bid < best_ask`.
2. No empty levels in either map.
3. Every resting order has `qty > 0`.
4. `index_.size()` equals the total order count across all levels.
5. Every order in every level has an `index_` entry whose side/price/node match.
6. Every order's `price` matches its level, and `side` matches which map it is in.
7. Within a level, `seq` is strictly ascending (FIFO integrity).

**C++ you learn:** nested container traversal, cross-referencing two structures,
`std::expected` carrying a `std::string` payload, building diagnostic messages,
`const`-correct iteration.

**Failing test provided (assistant):** `test/validate_test.cpp` — a well-formed book
validates; a book built by scripted sequences (adds, cancels, partial fills, modifies,
full-level consumption) still validates after every event.

**Done when:** `validate_test` green, and `validate()` is called after every event in the
existing integration test without firing.

---

## Task 3: Golden-file regression harness   [ASSISTANT]

**Deliverable:**
- `dhft_io` gains `dump_book(const OrderBook&, std::ostream&)`; the demo reuses it.
- `dhft_testkit` gains a golden runner: run a `.script`, capture events + final book,
  compare to `.expected`, or rewrite it when `DHFT_UPDATE_GOLDEN=1` is set.
- `test/golden/` gains at least five script/expected pairs: basic two-sided book,
  multi-level sweep, cancel/modify sequence, full liquidation, edge cases.
- `test/golden_test.cpp` runs every pair.
- CMake injects the golden directory path via `target_compile_definitions` so tests do not
  depend on the working directory.

**Done when:** `golden_test` green; deliberately breaking a rule in the engine makes it
fail; `DHFT_UPDATE_GOLDEN=1` regenerates cleanly.

---

## Task 4: Random script generator   [ASSISTANT]

**Deliverable:** `dhft_testkit` gains
```cpp
struct GenConfig { std::uint64_t seed; int events;
                   std::int64_t minPrice, maxPrice, maxQty;
                   int pctNew, pctCancel, pctModify; };
[[nodiscard]] std::vector<InEvent> generate(const GenConfig& cfg);
```
It tracks live ids so cancels and modifies mostly hit real orders, and is fully
deterministic for a given config.

**The assistant explains** `<random>` (engine vs distribution, why `std::mt19937_64` and
explicit seeding rather than `random_device`), and why reproducibility is non-negotiable
in a test generator.

**Done when:** a test confirms the same seed yields identical scripts and different seeds
differ; generated scripts are mostly-valid (few rejects).

---

## Task 5: The properties   [USER]

**Deliverable:** `test/property_test.cpp` assertions, run over hundreds of seeds.

**Concept:** a property is something true for *every* input, not one expected output. You
stop asserting "this script gives these trades" and start asserting "no matter what
happens, quantity is conserved."

**The five properties:**
- **P1 Invariants** — `validate()` succeeds after every event.
- **P2 No over-fill** — no order's cumulative fills exceed its submitted quantity.
- **P3 Limit respected** — buy aggressor: `trade.price <= limit`; sell: `>= limit`.
- **P4 Conservation** — maintain an expected total resting quantity (adds add, fills and
  cancels subtract, modifies adjust) and assert it matches the book's actual total after
  every event.
- **P5 Determinism** — the same script twice produces byte-identical output.

**C++ you learn:** accounting logic across an event stream, `std::unordered_map` for
per-id bookkeeping, writing assertions that quantify over all inputs, and reporting the
seed on failure so any bug replays exactly.

**Assistant provides:** the test scaffold (seed loop, engine setup, failure reporting).
The user writes the property checks themselves.

**Done when:** 500+ random scripts pass all five properties.

---

## Task 6: `OutEvent::operator==` and the reference model   [USER]

**Deliverable A:** a defaulted `operator==` on `OutEvent` in `Events.h` (one line — ties
back to the `operator<=>` lesson from M1 Task 1).

**Deliverable B:** `reference/` module with `dhft::reference::NaiveEngine` — a second
matching engine whose entire data structure is:
```cpp
std::vector<Order> resting_;
```
- best price on a side: linear scan for min (asks) or max (bids)
- front at a price: linear scan for the lowest `seq` at that price
- cancel: linear scan, erase
- modify: linear scan, adjust in place, or erase and push_back on increase
- matching: the same price-time algorithm as `MatchingEngine`, emitting into a `Sink&`

**Concept — the test oracle.** You deliberately write the *stupid* implementation. No
index, no maps, O(n) everywhere. Its only job is to be so simple it is obviously right.
Then two independent implementations agreeing is strong evidence both are correct — and
when M3 rewrites the fast engine's internals, this one does not change, so it proves the
rewrite preserved behavior.

**C++ you learn:** implementing to an interface, `std::vector` erase patterns and
iterator invalidation (a deliberate contrast with M1's `std::list` choice), and the
discipline of writing intentionally unoptimized code.

**Assistant provides:** the failing test — the reference model alone must pass the same
matching semantics tests the fast engine passes.

**Done when:** the reference model passes the M1 matching test suite on its own.

---

## Task 7: Equivalence driver   [ASSISTANT]

**Deliverable:** `test/equivalence_test.cpp` — drive the same input through
`MatchingEngine` and `NaiveEngine`, compare the `OutEvent` streams element by element.
Run over every golden script and 500+ random scripts. On divergence, print the seed, the
event index, and both events.

**Done when:** both engines agree everywhere; deliberately breaking one makes the test
fail with a useful message.

---

## Task 8 (stretch): Shrinking   [USER, optional]

**Deliverable:** when a random script fails, automatically minimize it — repeatedly try
removing one event and keep the shorter script if it still fails. Turns a 500-event
failure into a 3-event reproducer.

**Concept:** this is what real property-testing frameworks do, and it is the difference
between "something broke somewhere" and a debuggable case.

---

## Self-review

- **Spec coverage:** Layer 1 -> T1/T2, Layer 2 -> T3, Layer 3 -> T4/T5, Layer 4 -> T6/T7.
  All four success criteria map to a task. T8 is explicitly optional.
- **Split honored:** user owns T2, T5, T6 (and optional T8) — validate, properties,
  reference model. Assistant owns T1, T3, T4, T7 and all CMake.
- **Coach adaptation:** implementations for user tasks are intentionally withheld; each
  ships a concept, an interface, and a failing test. This is by design, not a gap.
- **Naming consistency:** `validate()`, `dump_book()`, `generate()`, `GenConfig`,
  `NaiveEngine`, `DHFT_CHECK` used identically across tasks and the spec.
- **Scope:** benchmarking and all optimization deferred to M3 per the agreed decision.
