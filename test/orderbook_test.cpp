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
  EXPECT_EQ(b.best_bid()->ticks, 100);
  ASSERT_TRUE(b.best_ask());
  EXPECT_EQ(b.best_ask()->ticks, 101);
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
  EXPECT_EQ(d[0].second.v, 8);
}

TEST(OrderBook, DepthReturnsTopLevelsInOrder) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Sell, Price{102}, Quantity{2}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Sell, Price{100}, Quantity{5}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Sell, Price{101}, Quantity{4}, Sequence{3}});
  auto d = b.depth(Side::Sell, 2);
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0].first.ticks, 100);
  EXPECT_EQ(d[1].first.ticks, 101);
}

TEST(OrderBook, CancelRemovesOrderAndEmptiesLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  EXPECT_FALSE(b.best_bid());
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
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 3);
}

TEST(OrderBook, ModifyDecreaseShrinksLevel) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}, Sequence{50}).has_value());
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 7);
}

TEST(OrderBook, ModifyIncreaseKeepsTotal) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{9}, Sequence{50}).has_value());
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 9);
  EXPECT_TRUE(b.best_bid());
}

TEST(OrderBook, ModifyUnknownRejects) {
  OrderBook b;
  auto r = b.modify(OrderId{999}, Quantity{5}, Sequence{50});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::UnknownOrder);
}

TEST(OrderBook, ModifyBadQuantityRejects) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  auto r = b.modify(OrderId{1}, Quantity{0}, Sequence{50});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadQuantity);
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 5);
}

TEST(OrderBook, ModifyIncreaseRestampsSequence) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Buy, Price{100}, Quantity{5}, Sequence{3}});

  ASSERT_EQ(b.front_at(Side::Buy, Price{100})->id.v, 1u);

  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{9}, Sequence{10}).has_value());
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->id.v, 2u);

  ASSERT_TRUE(b.cancel(OrderId{2}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());

  const Order* moved = b.front_at(Side::Buy, Price{100});
  ASSERT_NE(moved, nullptr);
  EXPECT_EQ(moved->id.v, 1u);
  EXPECT_EQ(moved->qty.v, 9);
  EXPECT_EQ(moved->seq.v, 10u);
}

TEST(OrderBook, ModifyDecreaseKeepsSequenceAndPosition) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});

  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}, Sequence{99}).has_value());

  const Order* front = b.front_at(Side::Buy, Price{100});
  ASSERT_NE(front, nullptr);
  EXPECT_EQ(front->id.v, 1u);
  EXPECT_EQ(front->qty.v, 2);
  EXPECT_EQ(front->seq.v, 1u);
}

TEST(OrderBook, CancelMiddleThenNeighboursStaysValid) {
  OrderBook b;
  b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  b.add(Order{OrderId{3}, Side::Buy, Price{100}, Quantity{2}, Sequence{3}});
  ASSERT_TRUE(b.cancel(OrderId{2}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());
  EXPECT_FALSE(b.best_bid());
}
