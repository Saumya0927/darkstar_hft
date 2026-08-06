#include <dhft/MatchingEngine.h>
#include <dhft/OrderBook.h>
#include <dhft/Sink.h>
#include <gtest/gtest.h>

#include <vector>

using namespace dhft;

namespace {

InEvent newOrder(std::uint64_t id, Side side, std::int64_t px, std::int64_t qty) {
  return InEvent::new_order(OrderId{id}, side, Price{px}, Quantity{qty});
}

InEvent cancelOrder(std::uint64_t id) {
  return InEvent::cancel(OrderId{id});
}

InEvent modifyOrder(std::uint64_t id, std::int64_t qty) {
  return InEvent::modify(OrderId{id}, Quantity{qty});
}

void mustAdd(OrderBook& b, const Order& o) { ASSERT_TRUE(b.add(o).has_value()); }

} // namespace

TEST(Validate, EmptyBookIsValid) {
  OrderBook b;
  EXPECT_TRUE(b.validate().has_value());
}

TEST(Validate, PopulatedBookIsValid) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Buy, Price{99}, Quantity{4}, Sequence{3}});
  mustAdd(b, Order{OrderId{4}, Side::Sell, Price{101}, Quantity{7}, Sequence{4}});
  EXPECT_TRUE(b.validate().has_value());
}

TEST(Validate, StaysValidAfterCancels) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Buy, Price{100}, Quantity{2}, Sequence{3}});

  ASSERT_TRUE(b.cancel(OrderId{2}).has_value());
  EXPECT_TRUE(b.validate().has_value());
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  EXPECT_TRUE(b.validate().has_value());
  ASSERT_TRUE(b.cancel(OrderId{3}).has_value());
  EXPECT_TRUE(b.validate().has_value());
}

TEST(Validate, StaysValidAfterModifyDecrease) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}, Sequence{9}).has_value());
  EXPECT_TRUE(b.validate().has_value());
}

TEST(Validate, StaysValidAfterModifyIncrease) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{5}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Buy, Price{100}, Quantity{5}, Sequence{3}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{9}, Sequence{10}).has_value());
  EXPECT_TRUE(b.validate().has_value());
}

TEST(Validate, RejectedOperationsLeaveBookValid) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  EXPECT_FALSE(b.cancel(OrderId{99}).has_value());
  EXPECT_FALSE(b.modify(OrderId{99}, Quantity{5}, Sequence{9}).has_value());
  EXPECT_FALSE(b.modify(OrderId{1}, Quantity{0}, Sequence{9}).has_value());
  EXPECT_TRUE(b.validate().has_value());
}

TEST(Validate, HoldsAfterEveryEngineEvent) {
  const std::vector<InEvent> script = {
      newOrder(1, Side::Buy, 99, 5),   newOrder(2, Side::Buy, 100, 3),
      newOrder(3, Side::Sell, 102, 4), newOrder(4, Side::Buy, 100, 7),
      newOrder(5, Side::Sell, 100, 5), cancelOrder(3),
      modifyOrder(4, 2),               modifyOrder(4, 12),
      newOrder(6, Side::Sell, 98, 20), cancelOrder(3),
      newOrder(7, Side::Buy, 105, 9),  newOrder(8, Side::Sell, 95, 30),
  };

  CollectingSink sink;
  MatchingEngine engine{sink};

  for (std::size_t i = 0; i < script.size(); ++i) {
    engine.process(script[i]);
    auto r = engine.book().validate();
    ASSERT_TRUE(r.has_value()) << "after event " << i << ": " << r.error();
  }
}

TEST(TotalQuantity, EmptyBookIsZero) {
  OrderBook b;
  EXPECT_EQ(b.total_quantity(Side::Buy).v, 0);
  EXPECT_EQ(b.total_quantity(Side::Sell).v, 0);
}

TEST(TotalQuantity, SumsAcrossAllLevels) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{100}, Quantity{3}, Sequence{2}});
  mustAdd(b, Order{OrderId{3}, Side::Buy, Price{99}, Quantity{4}, Sequence{3}});
  mustAdd(b, Order{OrderId{4}, Side::Sell, Price{101}, Quantity{7}, Sequence{4}});

  EXPECT_EQ(b.total_quantity(Side::Buy).v, 12);
  EXPECT_EQ(b.total_quantity(Side::Sell).v, 7);
}

TEST(TotalQuantity, ReflectsCancelAndModify) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}});
  mustAdd(b, Order{OrderId{2}, Side::Buy, Price{99}, Quantity{5}, Sequence{2}});
  ASSERT_TRUE(b.modify(OrderId{1}, Quantity{2}, Sequence{9}).has_value());
  EXPECT_EQ(b.total_quantity(Side::Buy).v, 7);
  ASSERT_TRUE(b.cancel(OrderId{2}).has_value());
  EXPECT_EQ(b.total_quantity(Side::Buy).v, 2);
}

TEST(TotalQuantity, GoesToZeroWhenBookEmpties) {
  OrderBook b;
  mustAdd(b, Order{OrderId{1}, Side::Sell, Price{101}, Quantity{5}, Sequence{1}});
  ASSERT_TRUE(b.cancel(OrderId{1}).has_value());
  EXPECT_EQ(b.total_quantity(Side::Sell).v, 0);
}
