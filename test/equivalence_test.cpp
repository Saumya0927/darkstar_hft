#include <dhft/MatchingEngine.h>
#include <dhft/Sink.h>
#include <dhft/io/Script.h>
#include <dhft/reference/NaiveEngine.h>
#include <dhft/testkit/Generate.h>
#include <dhft/testkit/Golden.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using namespace dhft;
using namespace dhft::testkit;

namespace {

constexpr std::uint64_t kSeeds = 500;

std::vector<OutEvent> runFast(const std::vector<InEvent>& script) {
  CollectingSink sink;
  MatchingEngine engine{sink};
  for (const auto& e : script) {
    engine.process(e);
  }
  return sink.all();
}

std::vector<OutEvent> runReference(const std::vector<InEvent>& script) {
  CollectingSink sink;
  reference::NaiveEngine engine{sink};
  for (const auto& e : script) {
    engine.process(e);
  }
  return sink.all();
}

std::string describe(const OutEvent& e) {
  std::ostringstream s;
  s << "kind=" << static_cast<int>(e.kind) << " id=" << e.id.v << " resting=" << e.resting.v
    << " price=" << e.price.ticks << " qty=" << e.qty.v << " reason=" << static_cast<int>(e.reason);
  return s.str();
}

void expectSameStream(const std::vector<InEvent>& script, const std::string& label) {
  const auto fast = runFast(script);
  const auto slow = runReference(script);

  const std::size_t n = std::min(fast.size(), slow.size());
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_EQ(fast[i], slow[i]) << label << " diverged at output " << i << "\n  fast: "
                                << describe(fast[i]) << "\n  ref:  " << describe(slow[i]);
  }
  ASSERT_EQ(fast.size(), slow.size()) << label << ": output stream lengths differ";
}

void expectSameLadder(const OrderBook& fast, const reference::NaiveEngine& slow, Side side,
                      const std::string& label) {
  const auto a = fast.depth(side, 100);
  const auto b = slow.depth(side, 100);
  ASSERT_EQ(a.size(), b.size()) << label << ": level count differs";
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(a[i].first.ticks, b[i].first.ticks) << label << ": price differs at level " << i;
    ASSERT_EQ(a[i].second.v, b[i].second.v) << label << ": quantity differs at level " << i;
  }
}

} // namespace

TEST(Equivalence, AgreesOnGoldenScripts) {
  const auto names = golden_names(DHFT_GOLDEN_DIR);
  ASSERT_GE(names.size(), 5u);
  for (const auto& name : names) {
    const auto script = io::parse_script_file(std::string{DHFT_GOLDEN_DIR} + "/" + name + ".script");
    expectSameStream(script, "golden " + name);
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST(Equivalence, AgreesOnRandomScripts) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    GenConfig cfg;
    cfg.seed = seed;
    cfg.events = 200;
    expectSameStream(generate(cfg), "seed=" + std::to_string(seed));
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST(Equivalence, AgreesOnNarrowPriceBandWithHeavyCrossing) {
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    GenConfig cfg;
    cfg.seed = seed;
    cfg.events = 300;
    cfg.minPrice = 99;
    cfg.maxPrice = 101;
    cfg.maxQty = 3;
    expectSameStream(generate(cfg), "narrow seed=" + std::to_string(seed));
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST(Equivalence, AgreesOnModifyHeavyScripts) {
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    GenConfig cfg;
    cfg.seed = seed;
    cfg.events = 300;
    cfg.weightNew = 40;
    cfg.weightCancel = 10;
    cfg.weightModify = 50;
    expectSameStream(generate(cfg), "modify-heavy seed=" + std::to_string(seed));
    if (HasFatalFailure()) {
      return;
    }
  }
}

TEST(Equivalence, BooksAgreeOnFullLadder) {
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    GenConfig cfg;
    cfg.seed = seed;
    cfg.events = 200;
    const auto script = generate(cfg);

    CollectingSink fastSink;
    MatchingEngine fast{fastSink};
    CollectingSink slowSink;
    reference::NaiveEngine slow{slowSink};

    for (const auto& e : script) {
      fast.process(e);
      slow.process(e);
    }

    const std::string label = "seed=" + std::to_string(seed);
    EXPECT_EQ(fast.book().total_quantity(Side::Buy).v, slow.total_quantity(Side::Buy).v) << label;
    EXPECT_EQ(fast.book().total_quantity(Side::Sell).v, slow.total_quantity(Side::Sell).v) << label;
    EXPECT_EQ(fast.book().best_bid().has_value(), slow.best_bid().has_value()) << label;
    EXPECT_EQ(fast.book().best_ask().has_value(), slow.best_ask().has_value()) << label;

    expectSameLadder(fast.book(), slow, Side::Buy, label + " bids");
    expectSameLadder(fast.book(), slow, Side::Sell, label + " asks");
    if (HasFatalFailure()) {
      return;
    }
  }
}
