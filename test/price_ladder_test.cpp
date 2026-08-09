#include <dhft/PriceLadder.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace dhft;

namespace {

struct Lvl {
  std::uint32_t head{};
  std::uint32_t tail{};
};

using Bid = PriceLadder<Side::Buy, Lvl>;
using Ask = PriceLadder<Side::Sell, Lvl>;

std::vector<std::int64_t> visitPrices(const auto& ladder) {
  std::vector<std::int64_t> out;
  ladder.for_each([&out](Price p, const Lvl&) { out.push_back(p.ticks); });
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

TEST(PriceLadder, EmptyLadderFindsNothing) {
  Bid b{Price{100}, Price{102}};
  EXPECT_EQ(b.find(Price{100}), nullptr);
  EXPECT_EQ(b.find(Price{101}), nullptr);
  EXPECT_EQ(b.find(Price{9999}), nullptr);
  EXPECT_FALSE(b.best());
  EXPECT_TRUE(visitPrices(b).empty());
}

TEST(PriceLadder, InsertThenFindRoundTrips) {
  Bid b{Price{100}, Price{102}};
  b.insert(Price{101}).head = 7;
  ASSERT_NE(b.find(Price{101}), nullptr);
  EXPECT_EQ(b.find(Price{101})->head, 7u);
  EXPECT_EQ(b.find(Price{100}), nullptr);
  EXPECT_EQ(b.find(Price{102}), nullptr);
}

TEST(PriceLadder, InsertOnALiveLevelDoesNotReset) {
  Bid b{Price{100}, Price{102}};
  b.insert(Price{101}).head = 7;
  b.insert(Price{101}).head = 9;
  EXPECT_EQ(b.find(Price{101})->head, 9u);
}

TEST(PriceLadder, ReinsertAfterEraseResetsTheLevel) {
  Bid b{Price{100}, Price{102}};
  b.insert(Price{101}).head = 7;
  b.erase(Price{101});
  EXPECT_EQ(b.find(Price{101}), nullptr);
  EXPECT_EQ(b.insert(Price{101}).head, 0u);
}

TEST(PriceLadder, BandEndsAreIncluded) {
  Bid b{Price{100}, Price{102}};
  b.insert(Price{100}).head = 1;
  b.insert(Price{102}).head = 2;
  ASSERT_NE(b.find(Price{100}), nullptr);
  ASSERT_NE(b.find(Price{102}), nullptr);
  EXPECT_EQ(b.find(Price{100})->head, 1u);
  EXPECT_EQ(b.find(Price{102})->head, 2u);
}

TEST(PriceLadder, OutOfBandPricesStillWork) {
  Bid b{Price{100}, Price{102}};
  b.insert(Price{99}).head = 1;
  b.insert(Price{103}).head = 2;
  b.insert(Price{-7}).head = 3;
  b.insert(Price{500000}).head = 4;
  EXPECT_EQ(b.find(Price{99})->head, 1u);
  EXPECT_EQ(b.find(Price{103})->head, 2u);
  EXPECT_EQ(b.find(Price{-7})->head, 3u);
  EXPECT_EQ(b.find(Price{500000})->head, 4u);
  b.erase(Price{-7});
  EXPECT_EQ(b.find(Price{-7}), nullptr);
  EXPECT_EQ(b.find(Price{99})->head, 1u);
}

TEST(PriceLadder, EraseOfAnAbsentPriceIsHarmless) {
  Bid b{Price{100}, Price{102}};
  b.insert(Price{101}).head = 7;
  b.erase(Price{100});
  b.erase(Price{9999});
  EXPECT_EQ(b.find(Price{101})->head, 7u);
  EXPECT_EQ(visitPrices(b).size(), 1u);
}

TEST(PriceLadder, BidBestIsTheHighestPrice) {
  Bid b{Price{100}, Price{110}};
  (void)b.insert(Price{105});
  ASSERT_TRUE(b.best());
  EXPECT_EQ(b.best()->ticks, 105);
  (void)b.insert(Price{103});
  EXPECT_EQ(b.best()->ticks, 105);
  (void)b.insert(Price{108});
  EXPECT_EQ(b.best()->ticks, 108);
}

TEST(PriceLadder, AskBestIsTheLowestPrice) {
  Ask a{Price{100}, Price{110}};
  (void)a.insert(Price{105});
  (void)a.insert(Price{108});
  ASSERT_TRUE(a.best());
  EXPECT_EQ(a.best()->ticks, 105);
  (void)a.insert(Price{102});
  EXPECT_EQ(a.best()->ticks, 102);
}

TEST(PriceLadder, ErasingTheBestRecomputesToTheNextBest) {
  Bid b{Price{100}, Price{110}};
  (void)b.insert(Price{103});
  (void)b.insert(Price{105});
  (void)b.insert(Price{108});
  ASSERT_EQ(b.best()->ticks, 108);
  b.erase(Price{105});
  EXPECT_EQ(b.best()->ticks, 108) << "erasing a non-best level must not move best";
  b.erase(Price{108});
  EXPECT_EQ(b.best()->ticks, 103) << "erasing the best must recompute, not go stale";
  b.erase(Price{103});
  EXPECT_FALSE(b.best());
}

TEST(PriceLadder, BestSurvivesEmptyingAndRefilling) {
  Ask a{Price{100}, Price{110}};
  (void)a.insert(Price{105});
  a.erase(Price{105});
  EXPECT_FALSE(a.best());
  (void)a.insert(Price{107});
  ASSERT_TRUE(a.best());
  EXPECT_EQ(a.best()->ticks, 107);
}

TEST(PriceLadder, OutOfBandLevelsParticipateInBest) {
  Bid b{Price{100}, Price{110}};
  (void)b.insert(Price{105});
  (void)b.insert(Price{500});
  EXPECT_EQ(b.best()->ticks, 500) << "a better out-of-band level must win";
  (void)b.insert(Price{50});
  EXPECT_EQ(b.best()->ticks, 500) << "a worse out-of-band level must not win";
  b.erase(Price{500});
  EXPECT_EQ(b.best()->ticks, 105) << "falls back to the in-band level";
  b.erase(Price{105});
  EXPECT_EQ(b.best()->ticks, 50) << "in-band empty, out-of-band supplies best";
}

TEST(PriceLadder, AskOutOfBandLevelsParticipateInBest) {
  Ask a{Price{100}, Price{110}};
  (void)a.insert(Price{105});
  (void)a.insert(Price{50});
  EXPECT_EQ(a.best()->ticks, 50) << "lower is better for asks";
  (void)a.insert(Price{500});
  EXPECT_EQ(a.best()->ticks, 50);
}

TEST(PriceLadder, BestCrossesWordBoundaries) {
  Bid b{Price{0}, Price{199}};
  (void)b.insert(Price{5});
  (void)b.insert(Price{63});
  (void)b.insert(Price{64});
  (void)b.insert(Price{130});
  EXPECT_EQ(b.best()->ticks, 130);
  b.erase(Price{130});
  EXPECT_EQ(b.best()->ticks, 64) << "rescan must cross from word 2 back to word 1";
  b.erase(Price{64});
  EXPECT_EQ(b.best()->ticks, 63) << "and from word 1 to the top of word 0";
  b.erase(Price{63});
  EXPECT_EQ(b.best()->ticks, 5);
}

TEST(PriceLadder, AskBestCrossesWordBoundaries) {
  Ask a{Price{0}, Price{199}};
  (void)a.insert(Price{130});
  (void)a.insert(Price{64});
  (void)a.insert(Price{63});
  (void)a.insert(Price{5});
  EXPECT_EQ(a.best()->ticks, 5);
  a.erase(Price{5});
  EXPECT_EQ(a.best()->ticks, 63);
  a.erase(Price{63});
  EXPECT_EQ(a.best()->ticks, 64);
  a.erase(Price{64});
  EXPECT_EQ(a.best()->ticks, 130);
}

TEST(PriceLadder, ForEachVisitsEveryLevelExactlyOnce) {
  Bid b{Price{0}, Price{199}};
  for (std::int64_t p : {5, 63, 64, 65, 127, 128, 199}) {
    (void)b.insert(Price{p});
  }
  (void)b.insert(Price{500});
  (void)b.insert(Price{-7});
  const std::vector<std::int64_t> want{-7, 5, 63, 64, 65, 127, 128, 199, 500};
  EXPECT_EQ(visitPrices(b), want);
}

TEST(PriceLadder, ForEachHandsBackTheRealLevels) {
  Bid b{Price{0}, Price{199}};
  b.insert(Price{42}).head = 99;
  b.insert(Price{500}).head = 77;
  std::uint32_t inBand = 0;
  std::uint32_t outOfBand = 0;
  b.for_each([&](Price p, const Lvl& l) {
    if (p.ticks == 42) inBand = l.head;
    if (p.ticks == 500) outOfBand = l.head;
  });
  EXPECT_EQ(inBand, 99u);
  EXPECT_EQ(outOfBand, 77u);
}

TEST(PriceLadder, ForEachSkipsErasedLevels) {
  Bid b{Price{0}, Price{199}};
  (void)b.insert(Price{10});
  (void)b.insert(Price{20});
  (void)b.insert(Price{500});
  b.erase(Price{20});
  b.erase(Price{500});
  const std::vector<std::int64_t> want{10};
  EXPECT_EQ(visitPrices(b), want);
}

TEST(PriceLadder, AFullyOccupiedWordIsVisitedCompletely) {
  Bid b{Price{0}, Price{199}};
  for (std::int64_t p = 0; p < 64; ++p) {
    (void)b.insert(Price{p});
  }
  EXPECT_EQ(visitPrices(b).size(), 64u);
  EXPECT_EQ(b.best()->ticks, 63);
}

TEST(PriceLadder, ASingleTickBandWorks) {
  Bid b{Price{100}, Price{100}};
  EXPECT_FALSE(b.best());
  b.insert(Price{100}).head = 5;
  ASSERT_TRUE(b.best());
  EXPECT_EQ(b.best()->ticks, 100);
  EXPECT_EQ(b.find(Price{100})->head, 5u);
  EXPECT_EQ(b.find(Price{101}), nullptr);
  b.erase(Price{100});
  EXPECT_FALSE(b.best());
}

TEST(PriceLadder, TheDefaultSizedBandBehaves) {
  Bid b{Price{0}, Price{16383}};
  (void)b.insert(Price{9900});
  (void)b.insert(Price{10100});
  EXPECT_EQ(b.best()->ticks, 10100);
  b.erase(Price{10100});
  EXPECT_EQ(b.best()->ticks, 9900);
  EXPECT_EQ(visitPrices(b).size(), 1u);
}
