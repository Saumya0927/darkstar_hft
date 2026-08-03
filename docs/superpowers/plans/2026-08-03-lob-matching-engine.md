# LOB + Matching Engine (Milestone 1) — Learning Curriculum

> **COACH MODE.** This is not a normal execution plan. The **user writes every line of
> implementation** to learn C++. For each task the assistant: (1) teaches the concept +
> the C++, (2) provides the interface and a **failing gtest** as the executable spec,
> (3) the user writes the implementation until the test passes, (4) the assistant reviews
> the code for correctness + best practices, then commits. The assistant does NOT write
> the user's implementation code. Tests are provided by the assistant as the target
> (the user may opt to write tests on any lesson). There is NO subagent/inline execution
> handoff — lessons run interactively.

**Goal:** A correct, single-threaded limit order book + FIFO price-time matching engine
in modern C++, driven by a scripted feed, fully tested.

**Architecture:** Single-threaded event loop. `Feed` -> `MatchingEngine` (owns an
`OrderBook`) -> `Sink`. Book is a pure container; matcher is the algorithm; feed/sink are
abstract seams. Deterministic: same input sequence -> identical output.

**Tech Stack:** C++26 (`-std=c++2c`), upstream LLVM clang + libc++ (Homebrew), CMake,
GoogleTest (FetchContent), AddressSanitizer + UBSanitizer, clang-format + clang-tidy.

## Global Constraints (every task inherits these)

- **Prices are integer ticks, never floats.** Strong types (`Price`/`Quantity`/`OrderId`/
  `Sequence`) so they cannot be mixed (Core Guideline I.4).
- **Hot path is `noexcept`; no exceptions in the matching loop** (F.6). Fallible book ops
  return `std::expected<void, RejectReason>`.
- **Rule of Zero (C.20)** for value types; no naked `new`/`delete` (R.11); STL owns memory.
- `enum class` (Enum.3); `const`-correct queries + `[[nodiscard]]` (Con.1/Con.2); always
  brace-init (ES.20/ES.23).
- **`Level` is `std::list`** (stable iterators for O(1) cancel) — NOT deque/vector.
- Invariants via a `DHFT_CHECK` macro + ASan/UBSan (NOT C++26 contracts — not in clang yet).
- Namespace `dhft`. No emojis anywhere.

## File structure (locked)

```
CPP_Project/
  CMakeLists.txt            build: C++26, libc++, warnings, sanitizers, gtest, tidy hook
  .clang-format             BasedOnStyle: LLVM
  .clang-tidy               bugprone/performance/cppcoreguidelines/modernize/readability
  .gitignore                build/, compile_commands.json
  include/dhft/
    Types.h                 strong types, Side, Order, Trade                 (Task 1)
    Events.h                EventType/OutKind, InEvent, OutEvent, RejectReason (Task 2)
    OrderBook.h             container interface                              (Tasks 3-5)
    MatchingEngine.h        matcher interface                               (Task 6)
    Feed.h                  abstract Feed + ScriptedFeed                    (Task 7)
    Sink.h                  abstract Sink + Collecting/Printing sinks       (Task 7)
  src/
    OrderBook.cpp  MatchingEngine.cpp  Feed.cpp  Sink.cpp
  apps/demo.cpp             wire Feed -> Engine -> Sink, run a script       (Task 8)
  test/
    CMakeLists.txt
    types_test.cpp  events_test.cpp  orderbook_test.cpp
    matching_test.cpp  integration_test.cpp
  data/scripts/simple.txt  a scripted order sequence                       (Task 8)
```

---

## Task 0: Toolchain + skeleton (build harness)

**Deliverable:** the repo builds with C++26 on Homebrew LLVM clang + libc++, a trivial
gtest runs green, sanitizers active in Debug, clang-format/tidy configured.

**Concept:** CMake is the build system every C++ firm uses. `FetchContent` pulls
GoogleTest at configure time (no manual checkout). `compile_commands.json` powers clangd.
Sanitizers (ASan/UBSan) catch memory/UB bugs at runtime.

**Coach note:** the build harness is infrastructure, not C++ language learning. Default:
the assistant provides a working `CMakeLists.txt`, `.clang-format`, `.clang-tidy`,
`.gitignore`, and `test/CMakeLists.txt`, and explains every line so you understand it.
If you want to hand-write the CMake as a lesson instead, say so.

**C++/tooling you learn:** CMake targets & linking, `FetchContent`, libc++ selection,
warning/sanitizer flags, how a gtest target is registered.

- [ ] Install upstream LLVM: `brew install llvm` (confirm `$(brew --prefix llvm)/bin/clang++ --version`).
- [ ] Add `CMakeLists.txt`, `.clang-format`, `.clang-tidy`, `.gitignore`, `test/CMakeLists.txt`.
- [ ] Add a throwaway `test/smoke_test.cpp` with `TEST(Smoke, Builds){ ASSERT_EQ(1+1,2); }`.
- [ ] Configure + build + run:
  ```
  cmake -S . -B build -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build build && ctest --test-dir build --output-on-failure
  ```
  Expected: `smoke_test` passes; `build/compile_commands.json` exists.
- [ ] Commit: `chore: project skeleton, C++26 toolchain, gtest, sanitizers`.

---

## Task 1: `Types.h` — strong types + core value types

**Deliverable:** `Price`, `Quantity`, `OrderId`, `Sequence`, `Side`, `Order`, `Trade`,
all compiling and unit-tested.

**Concept:** "Primitive obsession" (everything an `int64`) lets you pass a quantity where
a price belongs — a real bug class here. Strong types make that not compile (I.4).

**C++ you learn:** `enum class` with underlying type, `<cstdint>` fixed-width ints,
`explicit` constructors, `constexpr`, `noexcept`, `[[nodiscard]]`, `operator<=>`
(three-way comparison) + `operator==`, operator overloading (only what each type needs),
`std::hash` specialization, aggregate/value semantics, brace-init.

**Interface you must expose (write the bodies yourself):**
```cpp
namespace dhft {
  enum class Side : std::uint8_t { Buy, Sell };

  struct Price {
    std::int64_t ticks{};
    [[nodiscard]] constexpr auto operator<=>(const Price&) const = default;
    [[nodiscard]] constexpr bool operator==(const Price&) const = default;
  };                                  // add: Price - Price -> tick delta if you need it

  struct Quantity { std::int64_t v{}; /* ==, <=>, +, -, and a >0 helper */ };
  struct OrderId  { std::uint64_t v{}; /* ==  (for lookups) */ };
  struct Sequence { std::uint64_t v{}; /* ==, <, and a next()/++ */ };

  struct Order { OrderId id{}; Side side{}; Price price{}; Quantity qty{}; Sequence seq{}; };
  struct Trade { OrderId aggressor{}; OrderId resting{}; Price price{}; Quantity qty{}; };
}
// std::hash<dhft::OrderId> specialization (needed by unordered_map in Task 4)
```

**Failing test I provide** (`test/types_test.cpp`) — you make it pass:
```cpp
#include <dhft/Types.h>
#include <gtest/gtest.h>
#include <unordered_map>
using namespace dhft;

TEST(Types, PriceOrders) {
  EXPECT_TRUE(Price{100} < Price{101});
  EXPECT_TRUE(Price{100} == Price{100});
  EXPECT_FALSE(Price{101} < Price{100});
}
TEST(Types, QuantityArithmetic) {
  EXPECT_EQ((Quantity{5} - Quantity{2}).v, 3);
  EXPECT_TRUE(Quantity{1}.positive());
  EXPECT_FALSE(Quantity{0}.positive());
}
TEST(Types, OrderIdHashesInMap) {
  std::unordered_map<OrderId, int> m;
  m[OrderId{42}] = 7;
  EXPECT_EQ(m.at(OrderId{42}), 7);
}
TEST(Types, SequenceIncrements) {
  Sequence s{0};
  EXPECT_TRUE(s.next() < s.next());   // or however you model monotonic issue
}
```
(We may adjust helper names together — the point is the behavior.)

**Done when:** `ctest` green, clean under ASan/UBSan, clang-tidy quiet. Commit.

---

## Task 2: `Events.h` — POD event types

**Deliverable:** `InEvent`, `OutEvent`, `EventType`, `OutKind`, `RejectReason`.

**Concept:** events flow through the loop as **trivially-copyable POD structs** (not
`std::variant`/virtual) so Phase-6's lock-free ring buffer can `memcpy` them later. A
tagged struct + `switch` is the idiom.

**C++ you learn:** POD / `std::is_trivially_copyable`, tagged unions via an enum tag,
`switch` on `enum class`.

**Interface:**
```cpp
namespace dhft {
  enum class EventType : std::uint8_t { NewOrder, Cancel, Modify };
  enum class OutKind   : std::uint8_t { Trade, Ack, Reject };
  enum class RejectReason : std::uint8_t { None, UnknownOrder, BadQuantity };

  struct InEvent  { EventType type{}; OrderId id{}; Side side{}; Price price{}; Quantity qty{}; };
  struct OutEvent { OutKind kind{}; OrderId id{}; OrderId resting{}; Price price{};
                    Quantity qty{}; RejectReason reason{RejectReason::None}; };
}
```

**Failing test I provide** (`test/events_test.cpp`):
```cpp
#include <dhft/Events.h>
#include <gtest/gtest.h>
#include <type_traits>
using namespace dhft;

TEST(Events, ArePod) {
  static_assert(std::is_trivially_copyable_v<InEvent>);
  static_assert(std::is_trivially_copyable_v<OutEvent>);
  SUCCEED();
}
TEST(Events, ConstructNewOrder) {
  InEvent e{EventType::NewOrder, OrderId{1}, Side::Buy, Price{100}, Quantity{5}};
  EXPECT_EQ(e.type, EventType::NewOrder);
  EXPECT_EQ(e.qty.v, 5);
}
```

**Done when:** green + clean. Commit.

---

## Task 3: `OrderBook` — structure + `add` + best bid/ask + depth

**Deliverable:** a book that accepts resting orders and answers best-bid/ask and depth.
No cancel/modify/match yet.

**Concept:** the book holds resting liquidity as price levels, each a FIFO queue. Bids
sorted high->low, asks low->high, so the best is always `begin()`.

**C++ you learn:** `std::map` with a custom comparator (`std::greater<>` for bids),
`std::list` as the per-level FIFO, references, `const`-correct `[[nodiscard]]` queries,
`std::optional` return, the interface/impl split (`.h` declares, `.cpp` defines).

**Interface (`include/dhft/OrderBook.h`):**
```cpp
namespace dhft {
  class OrderBook {
  public:
    void add(const Order& o);                                   // append to its level
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    // aggregated resting qty per price for the top `levels` prices of a side:
    [[nodiscard]] std::vector<std::pair<Price, Quantity>> depth(Side side, int levels) const;
  private:
    using Level = std::list<Order>;
    std::map<Price, Level, std::greater<>> bids_;   // best bid = begin()
    std::map<Price, Level> asks_;                   // best ask = begin()
    // (Task 4 adds the id -> location index)
  };
}
```

**Failing test I provide** (`test/orderbook_test.cpp`, first cases):
```cpp
TEST(OrderBook, AddRestsAndReportsBest) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy,  Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy,  Price{ 99}, Quantity{5}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Sell, Price{101}, Quantity{5}, Sequence{3}});
  ASSERT_TRUE(b.best_bid());  EXPECT_EQ(b.best_bid()->ticks, 100);   // highest bid
  ASSERT_TRUE(b.best_ask());  EXPECT_EQ(b.best_ask()->ticks, 101);   // lowest ask
}
TEST(OrderBook, DepthAggregatesLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  auto d = b.depth(Side::Buy, 1);
  ASSERT_EQ(d.size(), 1u);
  EXPECT_EQ(d[0].first.ticks, 100);
  EXPECT_EQ(d[0].second.v, 8);        // 5 + 3 at the same level
}
TEST(OrderBook, EmptyBookHasNoBest) {
  OrderBook b;
  EXPECT_FALSE(b.best_bid());
  EXPECT_FALSE(b.best_ask());
}
```

**Done when:** green + clean. Commit.

---

## Task 4: `OrderBook::cancel` — O(1) via id index + empty-level cleanup

**Deliverable:** cancel any resting order in O(1); unknown/filled id -> reject; a level
that empties is erased.

**Concept:** cancel is the most frequent op in real markets, so it must be O(1). Store,
per order id, a **location = (iterator to its price level, iterator to its list node)**.
`std::map` and `std::list` iterators stay valid when *other* elements change — this is
why `Level` must be `std::list`. On cancel, erase the node; if the level's list is now
empty, erase the level from the map (or `best_*` will read a dead level).

**C++ you learn:** `std::unordered_map`, **iterator stability guarantees**, storing
iterators as handles, `std::expected<void, RejectReason>`, careful erase ordering.

**Interface additions:**
```cpp
    std::expected<void, RejectReason> cancel(OrderId id);   // in OrderBook public
  private:
    struct Location { std::map<Price, Level, std::greater<>>::iterator bidLvl;
                      /* or ask side */ Level::iterator node; Side side; };
    std::unordered_map<OrderId, Location> index_;   // id -> where it lives
```
(You will design `Location` so it can point into either the bid or ask map — discuss the
cleanest representation with me before you code it; there are two reasonable designs.)

**Failing test I provide:**
```cpp
TEST(OrderBook, CancelRemovesOrder) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  EXPECT_FALSE(b.best_bid());                 // level emptied and erased
}
TEST(OrderBook, CancelUnknownRejects) {
  OrderBook b;
  auto r = b.cancel(OrderId{999});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::UnknownOrder);
}
TEST(OrderBook, CancelOneOfTwoKeepsLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  ASSERT_TRUE(b.best_bid());
  EXPECT_EQ(b.depth(Side::Buy,1)[0].second.v, 3);   // order 2 remains
}
```

**Done when:** green, and **ASan reports nothing** (this task is where a wrong container
choice would blow up — the sanitizer is your proof). Commit.

---

## Task 5: `OrderBook::modify` — qty change with queue-position rule

**Deliverable:** modify a resting order's quantity. Decrease keeps queue position;
increase loses it (cancel + re-add at the back). `newQty <= 0` -> reject `BadQuantity`;
unknown id -> reject `UnknownOrder`.

**Concept:** real exchanges let a size *decrease* keep your place in line, but a size
*increase* sends you to the back (fairness). M1 modifies quantity only; price changes are
a later milestone (a client would cancel + new).

**C++ you learn:** reusing `cancel`/`add` to express increase; in-place mutation for
decrease; more `std::expected`.

**Interface addition:** `std::expected<void, RejectReason> modify(OrderId id, Quantity newQty);`

**Failing test I provide:**
```cpp
TEST(OrderBook, ModifyDecreaseKeepsPosition) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}).has_value());
  // order 1 still first in queue at 100 with qty 2 -> verified in matching tests;
  // here just check total depth dropped 10 -> 7
  EXPECT_EQ(b.depth(Side::Buy,1)[0].second.v, 7);
}
TEST(OrderBook, ModifyBadQtyRejects) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  EXPECT_EQ(b.modify(OrderId{1}, Quantity{0}).error(), RejectReason::BadQuantity);
}
```

**Done when:** green + clean. Commit.

---

## Task 6: `MatchingEngine` — price-time priority matching

**Deliverable:** the matcher. On each `InEvent` it matches against the book, emits
`OutEvent`s (Trade/Ack/Reject), and rests the residual. Assigns each order the next
`Sequence`. `noexcept`.

**Concept:** a new order first *matches* (price priority: it crosses; time priority:
oldest at a level fills first) then *rests* the remainder. Fill price = the resting
order's price. Partial-filled resting orders keep their place. Never leave a crossed book.

**C++ you learn:** algorithms over containers, ownership (who holds the order mid-match),
move semantics, `const`-correctness, `noexcept` hot path, calling into the book's API.

**Interface (`include/dhft/MatchingEngine.h`):**
```cpp
namespace dhft {
  class MatchingEngine {
  public:
    explicit MatchingEngine(Sink& sink) noexcept;   // Sink from Task 7 (fwd-declare/order tasks)
    void process(const InEvent& e) noexcept;         // hot path: no throw
  private:
    OrderBook book_;
    Sink& sink_;
    Sequence next_{0};
  };
}
```
(We will sequence Task 7's `Sink` interface before you wire the engine's output; for
now the engine can take a `Sink&` you stub, or we do Task 7 first — your call.)

**Failing tests I provide** (`test/matching_test.cpp`) — the core of the whole milestone:
```cpp
// helper: a CollectingSink (from Task 7) records OutEvents into a vector.
TEST(Matching, FullFillAtRestingPrice) {
  CollectingSink s; MatchingEngine e{s};
  e.process({EventType::NewOrder, OrderId{1}, Side::Sell, Price{100}, Quantity{5}});
  e.process({EventType::NewOrder, OrderId{2}, Side::Buy,  Price{101}, Quantity{5}});
  auto trades = s.trades();
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].price.ticks, 100);        // resting price, aggressor improved
  EXPECT_EQ(trades[0].qty.v, 5);
}
TEST(Matching, PartialFillRestsResidual) {
  CollectingSink s; MatchingEngine e{s};
  e.process({EventType::NewOrder, OrderId{1}, Side::Sell, Price{100}, Quantity{3}});
  e.process({EventType::NewOrder, OrderId{2}, Side::Buy,  Price{100}, Quantity{5}});
  // 3 traded, 2 rest as a bid at 100
  EXPECT_EQ(s.trades().size(), 1u);
  EXPECT_EQ(s.trades()[0].qty.v, 3);
  // residual bid present -> verified via an ack/book query helper we agree on
}
TEST(Matching, FifoWithinLevel) {
  CollectingSink s; MatchingEngine e{s};
  e.process({EventType::NewOrder, OrderId{1}, Side::Sell, Price{100}, Quantity{5}});
  e.process({EventType::NewOrder, OrderId{2}, Side::Sell, Price{100}, Quantity{5}});
  e.process({EventType::NewOrder, OrderId{3}, Side::Buy,  Price{100}, Quantity{5}});
  ASSERT_EQ(s.trades().size(), 1u);
  EXPECT_EQ(s.trades()[0].resting.v, 1u);        // oldest (seq) fills first
}
TEST(Matching, MultiLevelSweep) {
  CollectingSink s; MatchingEngine e{s};
  e.process({EventType::NewOrder, OrderId{1}, Side::Sell, Price{100}, Quantity{5}});
  e.process({EventType::NewOrder, OrderId{2}, Side::Sell, Price{101}, Quantity{5}});
  e.process({EventType::NewOrder, OrderId{3}, Side::Buy,  Price{101}, Quantity{8}});
  auto t = s.trades();
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].price.ticks, 100);              // best level first
  EXPECT_EQ(t[1].price.ticks, 101);
  EXPECT_EQ(t[0].qty.v + t[1].qty.v, 8);
}
TEST(Matching, NoCrossJustRests) {
  CollectingSink s; MatchingEngine e{s};
  e.process({EventType::NewOrder, OrderId{1}, Side::Sell, Price{101}, Quantity{5}});
  e.process({EventType::NewOrder, OrderId{2}, Side::Buy,  Price{100}, Quantity{5}});
  EXPECT_TRUE(s.trades().empty());               // 100 < 101, no cross
}
```

**Done when:** all matching tests green, clean under ASan/UBSan. Commit. This is the
milestone's heart — expect the most review back-and-forth here.

---

## Task 7: `Feed` + `Sink` — the seams

**Deliverable:** abstract `Feed`/`Sink` plus `ScriptedFeed`, `CollectingSink`,
`PrintingSink`. (Do this task before, or interleaved with, Task 6 — the matcher needs
`Sink`, and the matching tests need `CollectingSink`.)

**Concept:** the seams make the system extensible: swap the feed (scripted -> real
venue) or the sink (collect -> strategy) without touching the matcher.

**C++ you learn:** abstract base classes, pure-virtual methods, virtual destructors,
runtime polymorphism, dependency injection, `override`.

**Interface:**
```cpp
namespace dhft {
  struct Feed { virtual ~Feed() = default;
                virtual bool next(InEvent& out) = 0; };        // false when exhausted
  class ScriptedFeed : public Feed {
  public: explicit ScriptedFeed(std::vector<InEvent> evs);
          bool next(InEvent& out) override; /* ... */ };

  struct Sink { virtual ~Sink() = default;
                virtual void on_event(const OutEvent& e) = 0; };
  class CollectingSink : public Sink {
  public: void on_event(const OutEvent& e) override;
          [[nodiscard]] std::vector<Trade> trades() const;    // convenience for tests
          const std::vector<OutEvent>& all() const noexcept; };
  class PrintingSink : public Sink {
  public: void on_event(const OutEvent& e) override; };       // prints to stdout
}
```

**Failing test I provide:**
```cpp
TEST(Feed, ScriptedYieldsInOrder) {
  ScriptedFeed f{{ InEvent{EventType::NewOrder, OrderId{1}, Side::Buy, Price{100}, Quantity{1}},
                   InEvent{EventType::Cancel,   OrderId{1}, Side::Buy, Price{0},   Quantity{0}} }};
  InEvent e;
  ASSERT_TRUE(f.next(e));  EXPECT_EQ(e.id.v, 1u);  EXPECT_EQ(e.type, EventType::NewOrder);
  ASSERT_TRUE(f.next(e));  EXPECT_EQ(e.type, EventType::Cancel);
  EXPECT_FALSE(f.next(e));                          // exhausted
}
TEST(Sink, CollectingRecords) {
  CollectingSink s;
  s.on_event(OutEvent{OutKind::Ack, OrderId{1}});
  EXPECT_EQ(s.all().size(), 1u);
}
```

**Done when:** green + clean. Commit.

---

## Task 8: `demo.cpp` + determinism (integration)

**Deliverable:** a runnable demo that reads a script, drives the engine, prints trades/
acks/rejects and the final book. Plus a determinism test: same script twice -> identical
output.

**Concept:** wiring the loop end-to-end; proving determinism (the property that makes the
whole design trustworthy for later backtesting).

**C++ you learn:** `int main(argc, argv)`, `std::ifstream` parsing, wiring components,
running a `Feed` through the engine into a `Sink`.

**Interface:** `apps/demo.cpp` — `main` builds a `ScriptedFeed` from
`data/scripts/simple.txt` (or a path in `argv[1]`), a `PrintingSink`, a `MatchingEngine`,
then loops `while (feed.next(e)) engine.process(e);` and prints the final book.

**Script format** (`data/scripts/simple.txt`), one event per line:
```
N 1 BUY  100 5     # NewOrder id side price qty
N 2 SELL 101 5
N 3 SELL 100 3     # crosses -> trade
C 2                # Cancel id
```

**Failing test I provide** (`test/integration_test.cpp`):
```cpp
TEST(Integration, DeterministicReplay) {
  std::vector<InEvent> script = { /* a fixed sequence with crosses, cancels, rests */ };
  CollectingSink s1; MatchingEngine e1{s1};
  for (auto& e : script) e1.process(e);
  CollectingSink s2; MatchingEngine e2{s2};
  for (auto& e : script) e2.process(e);
  ASSERT_EQ(s1.all().size(), s2.all().size());
  for (size_t i = 0; i < s1.all().size(); ++i) {
    EXPECT_EQ(s1.all()[i].kind,  s2.all()[i].kind);
    EXPECT_EQ(s1.all()[i].qty.v, s2.all()[i].qty.v);
    EXPECT_EQ(s1.all()[i].price.ticks, s2.all()[i].price.ticks);
  }
}
```

**Done when:** demo runs and prints correct output for `simple.txt`; determinism test
green; full suite clean under ASan/UBSan; clang-tidy quiet. Commit. **Milestone 1 done.**

---

## Self-review (assistant)

- **Spec coverage:** every spec section maps to a task — types+strong types (T1),
  events/POD (T2), book add/query (T3), O(1) cancel + iterator stability (T4), modify
  rule (T5), price-time matching + all semantics (T6), seams/abstract interfaces (T7),
  wiring + determinism + demo (T8), toolchain/format/tidy/sanitizers (T0).
- **Coach adaptation:** implementation bodies intentionally withheld (user writes them);
  each task instead ships a concept, an interface, and a failing test as the executable
  spec — this is by design, not a placeholder gap.
- **Type consistency:** `Price.ticks`, `Quantity.v`, `OrderId.v`, `Sequence.v`,
  `Side::{Buy,Sell}`, `EventType::{NewOrder,Cancel,Modify}`, `OutKind::{Trade,Ack,Reject}`,
  `RejectReason::{None,UnknownOrder,BadQuantity}`, `std::expected<void,RejectReason>` used
  identically across T1-T8. `CollectingSink.trades()/all()` used by T6 tests, defined T7
  (T7 runs with/before T6).
- **Scope:** M1 only; market/IOC/FOK, price-modify, real data, strategy, risk,
  low-latency rewrite, threading all deferred to later milestones per the spec.
