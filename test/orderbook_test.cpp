#include <dhft/OrderBook.h>
#include <gtest/gtest.h>

#include <limits>

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

TEST(OrderBook, AcceptsOrderIdsWiderThanThirtyTwoBits) {
  OrderBook b;
  const OrderId wide{(std::uint64_t{1} << 40) + 12345};
  const OrderId wider{std::numeric_limits<std::uint64_t>::max() - 7};
  ASSERT_TRUE(b.add(Order{wide, Side::Buy, Price{100}, Quantity{5}, Sequence{1}}).has_value());
  ASSERT_TRUE(b.add(Order{wider, Side::Sell, Price{101}, Quantity{3}, Sequence{2}}).has_value());
  EXPECT_TRUE(b.contains(wide));
  EXPECT_TRUE(b.contains(wider));
  EXPECT_FALSE(b.contains(OrderId{12345}));
  ASSERT_TRUE(b.front_at(Side::Buy, Price{100}).has_value());
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->id.v, wide.v);
  const auto fill = b.take_from_front(Side::Sell, Price{101}, Quantity{3});
  ASSERT_TRUE(fill.has_value());
  EXPECT_EQ(fill->restingId.v, wider.v);
  EXPECT_TRUE(b.cancel(wide).has_value());
  EXPECT_FALSE(b.contains(wide));
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, AddRejectsPriceOutsideThirtyTwoBits) {
  OrderBook b;
  const std::int64_t wide = std::int64_t{1} << 40;
  auto r = b.add(Order{OrderId{1}, Side::Buy, Price{wide}, Quantity{5}, Sequence{1}});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadPrice);
  r = b.add(Order{OrderId{1}, Side::Buy, Price{-wide}, Quantity{5}, Sequence{1}});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadPrice);
  EXPECT_FALSE(b.contains(OrderId{1}));
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, AddRejectsQuantityOutsideThirtyTwoBits) {
  OrderBook b;
  auto r = b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{std::int64_t{1} << 40}, Sequence{1}});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadQuantity);
  EXPECT_FALSE(b.contains(OrderId{1}));
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, ModifyRejectsQuantityOutsideThirtyTwoBits) {
  OrderBook b;
  ASSERT_TRUE(b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}}).has_value());
  auto r = b.modify(OrderId{1}, Quantity{std::int64_t{1} << 40}, Sequence{2});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), RejectReason::BadQuantity);
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->qty.v, 5);
}

TEST(OrderBook, ModifyToNewPriceMovesTheOrderAndLosesPriority) {
  OrderBook b;
  ASSERT_TRUE(b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}}).has_value());
  ASSERT_TRUE(b.add(Order{OrderId{2}, Side::Buy, Price{101}, Quantity{3}, Sequence{2}}).has_value());
  ASSERT_TRUE(b.modify(OrderId{1}, Price{101}, Quantity{5}, Sequence{3}).has_value());
  ASSERT_TRUE(b.validate().has_value()) << b.validate().error();
  EXPECT_FALSE(b.front_at(Side::Buy, Price{100}).has_value());
  ASSERT_TRUE(b.front_at(Side::Buy, Price{101}).has_value());
  EXPECT_EQ(b.front_at(Side::Buy, Price{101})->id.v, 2u);
  const auto first = b.take_from_front(Side::Buy, Price{101}, Quantity{3});
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->restingId.v, 2u);
  const auto second = b.take_from_front(Side::Buy, Price{101}, Quantity{5});
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->restingId.v, 1u);
  EXPECT_TRUE(second->levelEmptied);
  EXPECT_FALSE(b.best_bid().has_value());
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, ModifyToNewPriceCarriesTheNewQuantityAndSequence) {
  OrderBook b;
  ASSERT_TRUE(b.add(Order{OrderId{1}, Side::Sell, Price{105}, Quantity{5}, Sequence{1}}).has_value());
  ASSERT_TRUE(b.modify(OrderId{1}, Price{103}, Quantity{2}, Sequence{7}).has_value());
  const auto o = b.front_at(Side::Sell, Price{103});
  ASSERT_TRUE(o.has_value());
  EXPECT_EQ(o->id.v, 1u);
  EXPECT_EQ(o->qty.v, 2);
  EXPECT_EQ(o->seq.v, 7u);
  EXPECT_EQ(b.best_ask()->ticks, 103);
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, ModifyAtSamePriceThroughTheFourArgOverloadKeepsPlaceOnDecrease) {
  OrderBook b;
  ASSERT_TRUE(b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}}).has_value());
  ASSERT_TRUE(b.add(Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}}).has_value());
  ASSERT_TRUE(b.modify(OrderId{1}, Price{100}, Quantity{2}, Sequence{9}).has_value());
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->id.v, 1u);
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->seq.v, 1u);
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, ModifyToNewPriceRejectsBadInputs) {
  OrderBook b;
  ASSERT_TRUE(b.add(Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}}).has_value());
  EXPECT_EQ(b.modify(OrderId{1}, Price{std::int64_t{1} << 40}, Quantity{5}, Sequence{2}).error(),
            RejectReason::BadPrice);
  EXPECT_EQ(b.modify(OrderId{1}, Price{101}, Quantity{0}, Sequence{2}).error(), RejectReason::BadQuantity);
  EXPECT_EQ(b.modify(OrderId{9}, Price{101}, Quantity{5}, Sequence{2}).error(), RejectReason::UnknownOrder);
  EXPECT_EQ(b.front_at(Side::Buy, Price{100})->id.v, 1u);
  EXPECT_TRUE(b.validate().has_value());
}

TEST(OrderBook, TailInsertsCountOnlyOutOfBandLevels) {
  OrderBook b{16, Price{100}, Price{110}};
  EXPECT_EQ(b.tail_inserts(), 0u);
  ASSERT_TRUE(b.add(Order{OrderId{1}, Side::Buy, Price{105}, Quantity{1}, Sequence{1}}).has_value());
  ASSERT_TRUE(b.add(Order{OrderId{2}, Side::Buy, Price{105}, Quantity{1}, Sequence{2}}).has_value());
  EXPECT_EQ(b.tail_inserts(), 0u);
  ASSERT_TRUE(b.add(Order{OrderId{3}, Side::Sell, Price{500}, Quantity{1}, Sequence{3}}).has_value());
  ASSERT_TRUE(b.add(Order{OrderId{4}, Side::Buy, Price{-5}, Quantity{1}, Sequence{4}}).has_value());
  EXPECT_EQ(b.tail_inserts(), 2u);
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());
  ASSERT_TRUE(b.add(Order{OrderId{5}, Side::Sell, Price{500}, Quantity{1}, Sequence{5}}).has_value());
  EXPECT_EQ(b.tail_inserts(), 3u);
}
