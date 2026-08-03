# darkstar_hft — Limit Order Book + Matching Engine (Milestone 1) Design

**Date:** 2026-08-03
**Package:** `darkstar_hft` (namespace `dhft`), repo root `/Users/saumyapatel/Dark Star Technologies/Random/CPP_Project`

## Goal

Build the core of an electronic-trading system in C++: a correct, single-threaded
limit order book plus a FIFO price-time matching engine. This is Milestone 1 of a
larger, venue-agnostic HFT-style trading system built as a vehicle for learning
professional C++ deeply.

## Working mode: COACH (non-negotiable)

**The user writes every line of code.** This project exists so the user gets better
at C++. For each piece, the assistant:

1. Explains the concept and the C++ behind it (the "what" and "why"),
2. Hands over the spec and the mental model,
3. The user writes the code,
4. The assistant reviews, corrects, and explains fixes.

The implementation plan that follows this spec is a **learning curriculum**, not a
script the assistant executes. The assistant does not write the user's production
code. (Tests may be an exception the user delegates, decided per-task.)

## The larger vision (context — NOT this milestone)

This system grows along a 7-rung ladder; each rung is its own spec -> plan -> build:

1. **Event-driven skeleton + limit order book**  <- (part of Milestone 1)
2. **FIFO price-time matching engine**            <- (part of Milestone 1)
3. Market-data ingestion + historical replay
4. Backtester = matching engine + strategy over one interface
5. First strategy (order-book-imbalance signal or a simple market maker)
6. Risk layer + kill switch; low-latency hardening (lock-free SPSC ring buffers,
   object pools, cache-tuned array-indexed book), benchmarked against Milestone 1
7. Instrument sharding across cores; optional venue adapters (Lighter / ITCH),
   kernel bypass only if a measured latency budget justifies it

**Venue decision:** venue-agnostic core. The book/matcher/event-bus are identical
regardless of venue; a real venue is only an adapter at the `Feed`/`Sink` seams.
Milestone 1 uses a scripted feed and a collecting sink.

---

## Milestone 1 scope

Single-threaded, plain-STL, correctness-first. Limit orders only. Everything else
(market/IOC/FOK orders, real-data replay, strategy, risk, low-latency rewrite,
threading) is a later milestone.

**In scope:** order book (add/cancel/modify/best-bid-ask/depth), FIFO price-time
matching (orders cross into trades), a scripted `Feed`, a collecting/printing
`Sink`, a demo app that runs a script, and a full gtest suite.

**Out of scope for M1:** market/IOC/FOK/iceberg order types, self-trade prevention,
real-data parsing, any strategy, any risk layer, any threading, any low-latency
optimization (array-indexed levels, object pools, lock-free queues).

---

## Architecture and seams

One **single-threaded event loop**. Every input is an event flowing through in a
fixed order, giving determinism: same input sequence -> identical output, always.
This is the LMAX-Disruptor / single-writer pattern real firms use, and it is what
makes testing and later backtesting trustworthy.

```
 [ Feed ]        input events           [ MatchingEngine ]        output events        [ Sink ]
 scripted   ->  NewOrder / Cancel / ->    owns an OrderBook   ->  Trade / Ack /   ->   collect /
 orders         Modify                    price-time priority,    Reject                print
                                          rests residual
                                               |
                                               v
                                          [ OrderBook ]
                                   pure container of resting liquidity
                                   (no matching logic lives here)
```

**Four components, one job each, independently testable:**

| Component | Job | Why separate |
|---|---|---|
| `Feed` | produce a sequence of input events | The **venue seam**. M1 = scripted; later a Lighter/ITCH adapter plugs in unchanged downstream. |
| `OrderBook` | hold resting orders; add / cancel / modify / best-bid-ask / depth | Pure data structure, no algorithm. Testable in isolation. |
| `MatchingEngine` | on each input event, match against the book (price-time priority), emit trades, rest remainder | The algorithm. Owns an `OrderBook`. Deterministic: no clock, no RNG. |
| `Sink` | receive output events (trades, acks, rejects) | The **strategy seam**. M1 = collect/print; later a strategy or risk layer plugs in. |

The two seams (`Feed` in, `Sink` out) are the whole reason the system scales: every
later addition plugs into one of them without touching the book or the matcher.

---

## Data model

**Everything is integers. Prices are integer ticks, never floats** — floats break
equality and priority comparisons; every real matching engine uses integer ticks.

**Strong types (Core Guideline I.4 — avoid primitive obsession).** Each quantity is
its own type so a `Quantity` cannot be passed where a `Price` is expected. Hand-written
per type (not a generic template) for readability; each carries only the operations it
needs:

```
struct Price    { std::int64_t ticks; };   // ordered (operator<=>), subtractable to a tick delta
struct Quantity { std::int64_t v;     };   // add / subtract / compare / >0 check
struct OrderId  { std::uint64_t v;    };   // equality + std::hash specialization
struct Sequence { std::uint64_t v;    };   // increment + compare (this IS time priority)

enum class Side      : std::uint8_t { Buy, Sell };
enum class EventType : std::uint8_t { NewOrder, Cancel, Modify };

struct Order { OrderId id; Side side; Price price; Quantity qty; Sequence seq; };
struct Trade { OrderId aggressor; OrderId resting; Price price; Quantity qty; };
```

Strong types must use `explicit` construction, be `constexpr` + `noexcept` where
possible, mark accessors `[[nodiscard]]`, and use `operator<=>` (C++20 three-way
comparison) for ordering where ordering is needed.

**Events are a POD "fat struct", not `std::variant` or a virtual hierarchy:**

```
struct InEvent  { EventType type; OrderId id; Side side; Price price; Quantity qty; };
// OutEvent similarly tagged: Trade / Ack / Reject
```

Rationale: trivially copyable, so Phase-6's lock-free ring buffer can `memcpy` events
in with zero fuss; a virtual hierarchy or `std::variant` fights you there. Simpler to
learn first, too. (`std::variant` + `std::visit` is the modern alternative we
deliberately skip.)

---

## Matching semantics (price-time priority, FIFO)

A new order first tries to **match**; only the unfilled remainder **rests**. For an
incoming Buy limit at price P, qty Q:

```
while Q > 0 and best ask exists and bestAskPrice <= P:      // price priority: it crosses
    resting = oldest order at best-ask level                // time priority: FIFO front of queue
    fill = min(Q, resting.qty)
    emit Trade{ aggressor=id, resting=resting.id, price=resting.price, qty=fill }
    Q -= fill;  resting.qty -= fill
    if resting.qty == 0:  remove resting (from level and id-map); if level now empty, erase level
if Q > 0:  rest remainder at price P (add to book + id-map);  emit Ack   // did not fully fill
```

Sell is the mirror (crosses when `bestBid >= P`).

**Rules (verified correct):**
- **Fill price = the resting (passive) order's price.** Price improvement goes to the
  aggressor. (A limit is "P or better".)
- **Partial fills keep queue position** — a partially-eaten resting order keeps its
  remaining qty at the front of its level.
- **No-crossed-book invariant is strictly `bestBid < bestAsk`**, and it holds
  automatically: a buy at P only rests after every ask `<= P` is consumed, so the
  remaining best ask is `> P`. A locked book (bid == ask) can never rest either.
- **Determinism:** `Sequence` (arrival counter) is the only time source.

**Worked example (verified).** Resting asks `100@5 (seq1)`, `100@3 (seq2)`, `101@10`.
Incoming **Buy limit 101, qty 7**:
1. bestAsk 100 <= 101 -> match seq1: fill 5, Trade@100x5, Q=2, seq1 removed.
2. bestAsk still 100 -> match seq2: fill 2, Trade@100x2, Q=0, seq2 left with qty 1 at front.
3. Q=0, done. Level 100 = seq2@1; level 101 untouched. Aggressor filled 7 @100 (improved
   from its 101 limit).

**Modify precise rule** (teaches queue position): `newQty <= 0` -> treat as Cancel;
`newQty == current` -> no-op; `newQty < current` -> decrease in place, **keep** position;
`newQty > current` or any price change -> cancel + re-add, **lose** position.

**Cancel** of an unknown or already-filled id -> `Reject` (idempotent).

---

## Components and interfaces

### `OrderBook` (pure container)

Internal structures (correctness-first; behind a clean interface so the Phase-6
array-rewrite stays localized):

- **Bids:** `std::map<Price, Level, std::greater<>>` (best bid = highest, first).
- **Asks:** `std::map<Price, Level>` (best ask = lowest, first).
- **`Level`** = **`std::list<Order>`** (FIFO by arrival). MUST be `std::list`, not
  `std::deque`/`std::vector`: O(1) cancel works by storing a **list iterator** to the
  order node in the id-map; `std::list` and `std::map` iterators stay valid when other
  elements are inserted/erased, whereas `std::deque`/`std::vector` iterators do not.
- **`std::unordered_map<OrderId, Location>`** where `Location = {price-map iterator to
  level, list iterator to order}` -> O(1) cancel/modify.

Operations (query methods `const` + `[[nodiscard]]`):
- `add(Order)` — insert a resting order (append to its level's list, register in id-map).
- `cancel(OrderId) -> std::expected<void, RejectReason>` — O(1) remove; empty-level cleanup.
- `modify(OrderId, Quantity newQty) -> std::expected<void, RejectReason>` — per the modify rule.
- `best_bid() const`, `best_ask() const` — optional/empty-aware.
- `depth(...) const` — top-N levels (for tests/visualization).

### `MatchingEngine` (algorithm)

- Owns an `OrderBook`. Assigns each incoming order the next `Sequence`.
- `process(const InEvent&) noexcept` — runs the matching algorithm above; emits
  `OutEvent`s (Trade/Ack/Reject) to the `Sink`. `noexcept` (Core Guideline F.6): the
  hot path does not throw.
- Deterministic: no wall clock, no RNG.

### `Feed` (abstract, venue seam)

- Pure-virtual base: `virtual bool next(InEvent& out) = 0;` (or a pull/`std::optional`
  form). `ScriptedFeed` reads events from an in-memory list or a text file in
  `data/scripts/`.

### `Sink` (abstract, strategy seam)

- Pure-virtual base: `virtual void on_event(const OutEvent&) = 0;`. `CollectingSink`
  stores events for test assertions; `PrintingSink` prints them.

### Demo app (`apps/demo.cpp`)

- Wires `ScriptedFeed -> MatchingEngine -> Sink`, runs a script, prints resulting
  trades/acks/rejects and the final book. Teaches `argc/argv`, `ifstream`, wiring.

---

## Error handling and invariants

- **Hot path is `noexcept`; no exceptions in the matching loop** (F.6). Fallible book
  operations return **`std::expected<void, RejectReason>`** (libc++ 16+), the modern
  allocation-free approach. Exceptions are reserved for edges (file parse, startup).
- **Invariants are NOT C++26 contracts** — contracts (P2900) are not implemented in
  clang yet. Instead a small `DHFT_CHECK(cond)` macro aborts with a message in debug
  builds, backed by **AddressSanitizer + UBSanitizer**. Checked invariants: never a
  crossed book (`bestBid < bestAsk`), never a resting qty <= 0, id-map consistent with
  book contents, sum of level list quantities matches any cached level total.
- `[[assume]]` (supported, clang 19+) may hint release builds for proven invariants
  later; not required for M1.

---

## Data-structure and optimization stance

- **`std::flat_map` is deliberately NOT used for the book in M1.** It is C++23 and
  cache-friendly, but the ISO draft ([flat.map.overview]/2.2) and cppreference confirm
  it **invalidates iterators on any insert/erase**, breaking the O(1)-cancel-via-stored-
  iterator design. `std::map`/`std::list` give the stable iterators M1 needs.
- **Optimization is a deliberate later phase, not now.** The clean STL version is built
  and measured first; Phase 6 then rewrites the hot path (array-indexed price levels
  `level = (price - min) / tick`, intrusive lists, an object pool referenced by stable
  **index handles** rather than iterators) and benchmarks the speedup. Premature
  optimization on an unprofiled book is the "optimizing the wrong path perfectly is
  waste" trap.
- **Three things are future-proofed for free in M1** (they cost nothing now and prevent
  a painful rewrite): integer tick prices; a clean interface boundary around the book's
  internals; the order-id lookup map from day one.

## Threading stance

**Single-writer core, forever.** A matching engine mutating one book must be
single-writer, or it needs locks that kill latency and determinism. Real exchanges run
single-writer per book. Multithreading enters later at the **seams** (a feed-handler
thread and a persistence/logging thread connected by lock-free SPSC ring buffers,
Phase 6) and via **instrument sharding** (N independent single-threaded engines on N
cores, Phase 7) — never by threading a single book. M1 is single-threaded and that is
the correct professional choice.

---

## Tooling and build

- **Standard:** C++26, flag `-std=c++2c` (safest spelling; `-std=c++26` also works on
  current clang). Only features actually shipped in clang/libc++ 22 are used
  (`std::expected`, `std::span`, `std::print`, `operator<=>`, `[[assume]]`, etc.). NOT
  used (not yet in clang/libc++): reflection, contracts, senders, `std::simd`, `std::hive`.
- **Compiler:** upstream LLVM clang via `brew install llvm`, with **libc++** (Apple
  clang lags upstream and does not reliably carry the newest libc++). Homebrew LLVM is
  keg-only, so CMake gets explicit paths:
  `-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++`, `-stdlib=libc++`, and an
  rpath to `$(brew --prefix llvm)/lib/c++`.
- **Build system:** CMake. Target C++26 via `CMAKE_CXX_STANDARD 26` if the installed
  CMake supports it (confirm at setup; CMake >= 3.28), else an explicit `-std=c++2c`
  flag. `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` for clangd.
- **Tests:** gtest via CMake `FetchContent` (no manual checkout, no `-Wno-error` hacks).
- **Warnings:** `-Wall -Wextra -Wpedantic` (add `-Werror` once green). **Debug
  sanitizers:** `-fsanitize=address,undefined -fno-omit-frame-pointer`.
- **Formatting:** `.clang-format` at root, `BasedOnStyle: LLVM` (tweak later).
- **Linting:** `.clang-tidy` enabling `bugprone-*, performance-*, cppcoreguidelines-*,
  modernize-*, readability-*`.
- **LSP:** clangd, reading `compile_commands.json`.
- CI (GitHub Actions: build + tests + sanitizers) is a trivial later add.

## Baseline best practices applied throughout

- **Rule of Zero (C.20)** for all M1 value types (`Order`, `Trade`, events, strong
  types): no user-defined special members, trivially copyable (ring-buffer-ready).
  Rule of Five (C.21) appears only when something owns a raw resource — nothing in M1.
- **No naked `new`/`delete` (R.11); RAII (R.1)** — STL containers own all memory.
- **const-correctness (Con.1/Con.2)**, `[[nodiscard]]` on queries, `enum class`
  (Enum.3), always brace-init (ES.20/ES.23).

---

## Repository layout

```
CPP_Project/
  CMakeLists.txt
  .clang-format
  .clang-tidy
  .gitignore
  README.md
  include/dhft/
    Types.h            strong types, Side, Order, Trade
    Events.h           InEvent (POD) + OutEvent (Trade/Ack/Reject)
    OrderBook.h        container: add/cancel/modify/best-bid-ask/depth
    MatchingEngine.h   algorithm: owns an OrderBook, matches, emits OutEvents
    Feed.h             abstract Feed + ScriptedFeed
    Sink.h             abstract Sink + CollectingSink / PrintingSink
  src/                 one .cpp per component
  apps/demo.cpp        wires Feed -> Engine -> Sink, runs a script
  test/                gtest: types / orderbook / matching / determinism
    CMakeLists.txt
  data/scripts/        order scripts the demo replays
  docs/superpowers/    specs and plans
```

---

## Testing plan (gtest, TDD — test first, then implement)

- **Types:** tick math, `Side`, strong-type construction, `OrderId` hashing.
- **OrderBook in isolation:** add rests correctly; `best_bid`/`best_ask`; O(1) cancel;
  cancel-unknown -> reject; modify-decrease keeps position; modify-increase/price loses
  it; empty-level cleanup.
- **MatchingEngine:** full fill; partial-fill-aggressor-rests; partial-fill-resting-
  keeps-position; multi-level sweep; FIFO within a level; price-time priority ordering;
  no-cross rests; fill-price = resting price; marketable-limit-as-market; crossed-book
  invariant never violated.
- **Determinism:** run a script twice -> byte-identical output.

---

## The C++ learning curriculum (why this is the whole point)

Every concept below is demanded by the domain, not a toy exercise:

| Piece | C++ learned |
|---|---|
| `Types.h` (strong types) | `enum class`, `<cstdint>`, `explicit`, `constexpr`, `noexcept`, `[[nodiscard]]`, `operator<=>`, operator overloading, `std::hash` specialization, value semantics |
| `Events.h` | POD / trivially-copyable types, tagged unions, `switch` dispatch |
| `OrderBook` | `std::map` + custom comparator, `std::list`, `std::unordered_map`, **iterators and iterator stability**, references, `std::expected`, interface/impl split |
| `MatchingEngine` | algorithms over containers, ownership, move semantics, `const`-correctness, `noexcept` hot path, references vs copies |
| `Feed` / `Sink` | abstract base classes, pure virtual, polymorphism, dependency injection (the seams) |
| CMake / gtest / sanitizers | build systems, linking, unit-test discipline, ASan/UBSan memory & UB debugging |
| `demo.cpp` | `argc/argv`, `ifstream` file I/O, wiring components |

---

## Success criteria (Milestone 1 is done when)

1. The demo app runs a scripted order sequence and prints correct trades, acks,
   rejects, and the final book state.
2. The full gtest suite passes, including all matching-semantics and determinism tests.
3. Debug build runs clean under AddressSanitizer + UndefinedBehaviorSanitizer.
4. `clang-format` and `clang-tidy` (the enabled check sets) pass.
5. Prices are integer ticks throughout; strong types prevent Price/Quantity mixups at
   compile time; the matching hot path is `noexcept`.
6. **The user wrote the code** and can explain each C++ construct used.
