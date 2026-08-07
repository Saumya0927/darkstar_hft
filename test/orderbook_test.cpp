#include <dhft/OrderBook.h>
#include <gtest/gtest.h>

using namespace dhft;

namespace {

void mustAdd(OrderBook& b, const Order& o) { ASSERT_TRUE(b.add(o).has_value()); }

} // namespace

TEST(OrderBook, AddRestsAndReportsBest) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{99}, Quantity{5}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Sell, Price{101}, Quantity{5}, Sequence{3}});
  mustAdd(b, Order{OrderId{4}, Side::Sell, Price{102}, Quantity{5}, Sequence{4}});

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
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  auto d = b.depth(Side::Buy, 1);
  ASSERT_EQ(d.size(), 1u);
  EXPECT_EQ(d[0].first.ticks, 100);
  EXPECT_EQ(d[0].second.v, 8);
}

TEST(OrderBook, DepthReturnsTopLevelsInOrder) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Sell, Price{102}, Quantity{2}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Sell, Price{100}, Quantity{5}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Sell, Price{101}, Quantity{4}, Sequence{3}});
  auto d = b.depth(Side::Sell, 2);
  ASSERT_EQ(d.size(), 2u);
  EXPECT_EQ(d[0].first.ticks, 100);
  EXPECT_EQ(d[1].first.ticks, 101);
}

TEST(OrderBook, CancelRemovesOrderAndEmptiesLevel) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
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
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  ASSERT_TRUE(b.best_bid());
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 3);
}

TEST(OrderBook, ModifyDecreaseShrinksLevel) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}, Sequence{50}).has_value());
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 7);
}

TEST(OrderBook, ModifyIncreaseKeepsTotal) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
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
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  auto r = b.modify(OrderId{1}, Quantity{0}, Sequence{50});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadQuantity);
  EXPECT_EQ(b.depth(Side::Buy, 1)[0].second.v, 5);
}

TEST(OrderBook, ModifyIncreaseRestampsSequence) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Buy, Price{100}, Quantity{5}, Sequence{3}});

  ASSERT_EQ(b.front_at(Side::Buy, Price{100})->id.v, 1u);

  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{9}, Sequence{10}).has_value());
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->id.v, 2u);

  ASSERT_TRUE(b.cancel(OrderId{2}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());

  const auto moved = b.front_at(Side::Buy, Price{100});
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved->id.v, 1u);
  EXPECT_EQ(moved->qty.v, 9);
  EXPECT_EQ(moved->seq.v, 10u);
}

TEST(OrderBook, ModifyDecreaseKeepsSequenceAndPosition) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});

  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}, Sequence{99}).has_value());

  const auto front = b.front_at(Side::Buy, Price{100});
  ASSERT_TRUE(front);
  EXPECT_EQ(front->id.v, 1u);
  EXPECT_EQ(front->qty.v, 2);
  EXPECT_EQ(front->seq.v, 1u);
}

TEST(OrderBook, CancelMiddleThenNeighboursStaysValid) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Buy, Price{100}, Quantity{2}, Sequence{3}});
  ASSERT_TRUE(b.cancel(OrderId{2}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());
  EXPECT_FALSE(b.best_bid());
}

TEST(OrderBook, AddRejectsDuplicateId) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  auto r = b.add(Order{OrderId{1}, Side::Sell, Price{200}, Quantity{9}, Sequence{2}});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::DuplicateOrderId);
  EXPECT_TRUE(b.validate().has_value());
  EXPECT_FALSE(b.best_ask());
}

TEST(OrderBook, AddRejectsNonPositiveQuantity) {
  OrderBook b;
  EXPECT_EQ(b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{0}, Sequence{1}}).error(),
            RejectReason::BadQuantity);
  EXPECT_EQ(b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{-3}, Sequence{2}}).error(),
            RejectReason::BadQuantity);
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, ContainsTracksLiveOrders) {
  OrderBook b;
  EXPECT_FALSE(b.contains(OrderId{1}));
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  EXPECT_TRUE(b.contains(OrderId{1}));
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  EXPECT_FALSE(b.contains(OrderId{1}));
}
