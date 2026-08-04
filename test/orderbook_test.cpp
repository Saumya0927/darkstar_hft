// Task 3 — OrderBook: add + best_bid/best_ask (pass 3a). depth added in 3b.
#include <dhft/OrderBook.h>
#include <gtest/gtest.h>

using namespace dhft;

TEST(OrderBook, AddRestsAndReportsBest) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{99}, Quantity{5}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Sell, Price{101}, Quantity{5}, Sequence{3}});
  b.add(Order{OrderId{4}, Side::Sell, Price{102}, Quantity{5}, Sequence{4}});

  ASSERT_TRUE(b.best_bid());
  EXPECT_EQ(b.best_bid()->ticks, 100); // highest bid is best
  ASSERT_TRUE(b.best_ask());
  EXPECT_EQ(b.best_ask()->ticks, 101); // lowest ask is best
}

TEST(OrderBook, EmptyBookHasNoBest) {
  OrderBook b;
  EXPECT_FALSE(b.best_bid());
  EXPECT_FALSE(b.best_ask());
}

TEST(OrderBook, DepthAggregatesLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  auto d = b.depth(Side::Buy, 1);
  ASSERT_EQ(d.size(), 1u);
  EXPECT_EQ(d[0].first.ticks, 100);
  EXPECT_EQ(d[0].second.v, 8); // 5 + 3 summed at the one level
}

TEST(OrderBook, DepthReturnsTopLevelsInOrder) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Sell, Price{102}, Quantity{2}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Sell, Price{100}, Quantity{5}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Sell, Price{101}, Quantity{4}, Sequence{3}});
  auto d = b.depth(Side::Sell, 2); // top 2 of 3 levels
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0].first.ticks, 100); // lowest ask first
  EXPECT_EQ(d[1].first.ticks, 101);
}
