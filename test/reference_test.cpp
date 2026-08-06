#include <dhft/Sink.h>
#include <dhft/reference/NaiveEngine.h>
#include <gtest/gtest.h>

using namespace dhft;
using namespace dhft::reference;

namespace {

InEvent no(std::uint64_t id, Side side, std::int64_t px, std::int64_t qty) {
  return InEvent::new_order(OrderId{id}, side, Price{px}, Quantity{qty});
}

} // namespace

TEST(Reference, FullFillAtRestingPrice) {
  CollectingSink s;
  NaiveEngine e{s};
  e.process(no(1, Side::Sell, 100, 5));
  e.process(no(2, Side::Buy, 101, 5));
  auto t = s.trades();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].price.ticks, 100);
  EXPECT_EQ(t[0].resting.v, 1u);
}

TEST(Reference, FifoWithinLevel) {
  CollectingSink s;
  NaiveEngine e{s};
  e.process(no(1, Side::Sell, 100, 5));
  e.process(no(2, Side::Sell, 100, 5));
  e.process(no(3, Side::Buy, 100, 5));
  ASSERT_EQ(s.trades().size(), 1u);
  EXPECT_EQ(s.trades()[0].resting.v, 1u);
}

TEST(Reference, MultiLevelSweepBestFirst) {
  CollectingSink s;
  NaiveEngine e{s};
  e.process(no(1, Side::Sell, 100, 5));
  e.process(no(2, Side::Sell, 101, 5));
  e.process(no(3, Side::Buy, 101, 8));
  auto t = s.trades();
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].price.ticks, 100);
  EXPECT_EQ(t[1].price.ticks, 101);
  EXPECT_EQ(t[1].qty.v, 3);
}

TEST(Reference, PartialFillRestsResidual) {
  CollectingSink s;
  NaiveEngine e{s};
  e.process(no(1, Side::Sell, 100, 3));
  e.process(no(2, Side::Buy, 100, 5));
  EXPECT_EQ(s.trades().size(), 1u);
  ASSERT_TRUE(e.best_bid());
  EXPECT_EQ(e.best_bid()->ticks, 100);
  EXPECT_EQ(e.total_quantity(Side::Buy).v, 2);
  EXPECT_FALSE(e.best_ask());
}

TEST(Reference, ModifyIncreaseLosesQueuePosition) {
  CollectingSink s;
  NaiveEngine e{s};
  e.process(no(1, Side::Buy, 100, 5));
  e.process(no(2, Side::Buy, 100, 5));
  e.process(InEvent::modify(OrderId{1}, Quantity{9}));
  e.process(no(3, Side::Sell, 100, 5));
  ASSERT_EQ(s.trades().size(), 1u);
  EXPECT_EQ(s.trades()[0].resting.v, 2u);
}

TEST(Reference, RejectsDuplicateAndBadQuantity) {
  CollectingSink s;
  NaiveEngine e{s};
  e.process(no(1, Side::Buy, 100, 5));
  e.process(no(1, Side::Buy, 101, 5));
  e.process(no(2, Side::Buy, 100, 0));
  ASSERT_EQ(s.all().size(), 3u);
  EXPECT_EQ(s.all()[1].reason, RejectReason::DuplicateOrderId);
  EXPECT_EQ(s.all()[2].reason, RejectReason::BadQuantity);
}
