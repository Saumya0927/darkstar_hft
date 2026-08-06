#include <dhft/MatchingEngine.h>
#include <dhft/Sink.h>
#include <dhft/testkit/Generate.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>

using namespace dhft;
using namespace dhft::testkit;

TEST(Generator, SameSeedYieldsIdenticalScript) {
  GenConfig cfg;
  cfg.seed = 12345;
  cfg.events = 300;
  const auto a = generate(cfg);
  const auto b = generate(cfg);

  ASSERT_EQ(a.size(), b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].type, b[i].type) << "at " << i;
    EXPECT_EQ(a[i].id.v, b[i].id.v) << "at " << i;
    EXPECT_EQ(a[i].side, b[i].side) << "at " << i;
    EXPECT_EQ(a[i].price.ticks, b[i].price.ticks) << "at " << i;
    EXPECT_EQ(a[i].qty.v, b[i].qty.v) << "at " << i;
  }
}

TEST(Generator, DifferentSeedsDiffer) {
  GenConfig a;
  a.seed = 1;
  GenConfig b;
  b.seed = 2;
  EXPECT_NE(generate(a).size() == 0, true);
  EXPECT_FALSE(std::equal(generate(a).begin(), generate(a).end(), generate(b).begin(),
                          [](const InEvent& x, const InEvent& y) {
                            return x.type == y.type && x.id.v == y.id.v && x.qty.v == y.qty.v;
                          }));
}

TEST(Generator, ProducesRequestedEventCount) {
  GenConfig cfg;
  cfg.events = 137;
  EXPECT_EQ(generate(cfg).size(), 137u);
}

TEST(Generator, RespectsPriceAndQuantityBounds) {
  GenConfig cfg;
  cfg.seed = 7;
  cfg.events = 500;
  cfg.minPrice = 50;
  cfg.maxPrice = 60;
  cfg.maxQty = 4;

  for (const auto& e : generate(cfg)) {
    if (e.type == EventType::NewOrder) {
      EXPECT_GE(e.price.ticks, 50);
      EXPECT_LE(e.price.ticks, 60);
      EXPECT_GE(e.qty.v, 1);
      EXPECT_LE(e.qty.v, 4);
    }
    if (e.type == EventType::Modify) {
      EXPECT_GE(e.qty.v, 1);
      EXPECT_LE(e.qty.v, 4);
    }
  }
}

TEST(Generator, NewOrderIdsAreUnique) {
  GenConfig cfg;
  cfg.seed = 99;
  cfg.events = 500;
  std::set<std::uint64_t> seen;
  for (const auto& e : generate(cfg)) {
    if (e.type == EventType::NewOrder) {
      EXPECT_TRUE(seen.insert(e.id.v).second) << "duplicate new-order id " << e.id.v;
    }
  }
}

TEST(Generator, MixHonoursWeights) {
  GenConfig cfg;
  cfg.seed = 3;
  cfg.events = 4000;
  cfg.weightNew = 60;
  cfg.weightCancel = 25;
  cfg.weightModify = 15;

  std::size_t news = 0;
  std::size_t cancels = 0;
  std::size_t modifies = 0;
  for (const auto& e : generate(cfg)) {
    switch (e.type) {
    case EventType::NewOrder: ++news; break;
    case EventType::Cancel: ++cancels; break;
    case EventType::Modify: ++modifies; break;
    }
  }

  EXPECT_GT(news, cancels);
  EXPECT_GT(cancels, modifies);
  EXPECT_GT(modifies, 0u);
}

TEST(Generator, ScriptsAreMostlyValid) {
  GenConfig cfg;
  cfg.seed = 42;
  cfg.events = 1000;

  CollectingSink sink;
  MatchingEngine engine{sink};
  for (const auto& e : generate(cfg)) {
    engine.process(e);
  }

  const auto rejects = std::count_if(sink.all().begin(), sink.all().end(),
                                     [](const OutEvent& e) { return e.kind == OutKind::Reject; });
  EXPECT_LT(static_cast<double>(rejects) / static_cast<double>(sink.all().size()), 0.5)
      << rejects << " rejects out of " << sink.all().size() << " events";
  EXPECT_TRUE(engine.book().validate().has_value());
}

TEST(Generator, ExercisesMatchingNotJustResting) {
  GenConfig cfg;
  cfg.seed = 8;
  cfg.events = 1000;

  CollectingSink sink;
  MatchingEngine engine{sink};
  for (const auto& e : generate(cfg)) {
    engine.process(e);
  }
  EXPECT_GT(sink.trades().size(), 50u) << "generator should produce crossing orders";
}
