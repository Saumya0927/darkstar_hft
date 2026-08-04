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

TEST(OrderBook, CancelRemovesOrderAndEmptiesLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  EXPECT_FALSE(b.best_bid()); // last order gone -> level erased
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
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 3); // order 2 remains
}

TEST(OrderBook, ModifyDecreaseShrinksLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}).has_value());
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 7); // 10 -> 7
}

TEST(OrderBook, ModifyIncreaseKeepsTotal) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{9}).has_value());
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 9);
  EXPECT_TRUE(b.best_bid()); // still resting at the same price
}

TEST(OrderBook, ModifyUnknownRejects) {
  OrderBook b;
  auto r = b.modify(OrderId{999}, Quantity{5});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::UnknownOrder);
}

TEST(OrderBook, ModifyBadQuantityRejects) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  auto r = b.modify(OrderId{1}, Quantity{0});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadQuantity);
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 5); // untouched
}

// The iterator-stability proof: cancel the middle order, then its neighbours.
// If the stored list iterators were invalidated, ASan would fire here.
TEST(OrderBook, CancelMiddleThenNeighboursStaysValid) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Buy, Price{100}, Quantity{2}, Sequence{3}});
  ASSERT_TRUE(b.cancel(OrderId{2}).has_value()); // middle
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());
  EXPECT_FALSE(b.best_bid()); // all gone
}
