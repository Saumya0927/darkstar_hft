// Task 1 — Side, Price, Quantity (1a) + OrderId, Sequence, Order, Trade (1b).
#include <dhft/Types.h>
#include <gtest/gtest.h>

#include <unordered_map>

using namespace dhft;

TEST(Types, SideHasTwoDistinctValues) { EXPECT_NE(Side::Buy, Side::Sell); }

TEST(Types, PriceOrders) {
  EXPECT_TRUE(Price{100} < Price{101});
  EXPECT_TRUE(Price{100} == Price{100});
  EXPECT_FALSE(Price{101} < Price{100});
}

TEST(Types, QuantityArithmetic) {
  EXPECT_EQ((Quantity{5} - Quantity{2}).v, 3);
  EXPECT_EQ((Quantity{5} + Quantity{2}).v, 7);
  EXPECT_TRUE(Quantity{3} == Quantity{3});
}

TEST(Types, QuantityPositive) {
  EXPECT_TRUE(Quantity{1}.positive());
  EXPECT_FALSE(Quantity{0}.positive());
  EXPECT_FALSE(Quantity{-2}.positive());
}

// ---- pass 1b ----

TEST(Types, OrderIdEquality) {
  EXPECT_TRUE(OrderId{7} == OrderId{7});
  EXPECT_FALSE(OrderId{7} == OrderId{8});
}

TEST(Types, OrderIdWorksAsMapKey) {
  std::unordered_map<OrderId, int> m;
  m[OrderId{42}] = 100;
  m[OrderId{43}] = 200;
  EXPECT_EQ(m.at(OrderId{42}), 100);
  EXPECT_EQ(m.size(), 2u);
}

TEST(Types, SequenceOrdersAndAdvances) {
  EXPECT_TRUE(Sequence{1} < Sequence{2});
  EXPECT_EQ(Sequence{5}.next().v, 6u);
}

TEST(Types, OrderAndTradeConstruct) {
  Order o{OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1}};
  EXPECT_EQ(o.id.v, 1u);
  EXPECT_EQ(o.price.ticks, 100);
  EXPECT_EQ(o.qty.v, 5);

  Trade t{OrderId{1}, OrderId{2}, Price{100}, Quantity{3}};
  EXPECT_EQ(t.aggressor.v, 1u);
  EXPECT_EQ(t.resting.v, 2u);
  EXPECT_EQ(t.qty.v, 3);
}
