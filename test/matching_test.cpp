// Task 6 — MatchingEngine: price-time priority matching.
#include <dhft/MatchingEngine.h>
#include <dhft/Sink.h>
#include <gtest/gtest.h>

using namespace dhft;

namespace {
InEvent newOrder(std::uint64_t id, Side side, std::int64_t px, std::int64_t qty) {
  return InEvent{EventType::NewOrder, OrderId{id}, side, Price{px}, Quantity{qty}};
}
}


TEST(Matching, RestingOrderIsAcked) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, 5));
  ASSERT_EQ(s.all().size(), 1u);
  EXPECT_EQ(s.all()[0].kind, OutKind::Ack);
  EXPECT_EQ(s.all()[0].id.v, 1u);
}

TEST(Matching, CancelUnknownEmitsReject) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(InEvent{EventType::Cancel, OrderId{99}, Side::Buy, Price{0}, Quantity{0}});
  ASSERT_EQ(s.all().size(), 1u);
  EXPECT_EQ(s.all()[0].kind, OutKind::Reject);
  EXPECT_EQ(s.all()[0].reason, RejectReason::UnknownOrder);
}

TEST(Matching, CancelKnownEmitsAck) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, 5));
  e.process(InEvent{EventType::Cancel, OrderId{1}, Side::Buy, Price{0}, Quantity{0}});
  ASSERT_EQ(s.all().size(), 2u);
  EXPECT_EQ(s.all()[1].kind, OutKind::Ack);
}

// --- pass 6c: the matching algorithm ---

TEST(Matching, NoCrossJustRests) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 101, 5));
  e.process(newOrder(2, Side::Buy, 100, 5)); // 100 < 101, no cross
  EXPECT_TRUE(s.trades().empty());
}

TEST(Matching, FullFillAtRestingPrice) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 5));
  e.process(newOrder(2, Side::Buy, 101, 5)); // crosses; fills at the resting 100
  auto t = s.trades();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].aggressor.v, 2u);
  EXPECT_EQ(t[0].resting.v, 1u);
  EXPECT_EQ(t[0].price.ticks, 100); // price improvement goes to the aggressor
  EXPECT_EQ(t[0].qty.v, 5);
}

TEST(Matching, PartialFillRestsResidual) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 3));
  e.process(newOrder(2, Side::Buy, 100, 5)); // 3 trade, 2 rest as a bid
  auto t = s.trades();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].qty.v, 3);
  ASSERT_TRUE(e.book().best_bid());
  EXPECT_EQ(e.book().best_bid()->ticks, 100);
  EXPECT_EQ(e.book().depth(Side::Buy, 1)[0].second.v, 2); // residual rested
  EXPECT_FALSE(e.book().best_ask());                      // ask fully consumed
}

TEST(Matching, AggressorFullyFilledDoesNotRest) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 9));
  e.process(newOrder(2, Side::Buy, 100, 4));
  EXPECT_FALSE(e.book().best_bid());                      // nothing left over to rest
  EXPECT_EQ(e.book().depth(Side::Sell, 1)[0].second.v, 5); // 9 - 4 remains resting
}

TEST(Matching, FifoWithinLevel) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 5)); // arrived first
  e.process(newOrder(2, Side::Sell, 100, 5));
  e.process(newOrder(3, Side::Buy, 100, 5));
  auto t = s.trades();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].resting.v, 1u); // oldest at the level fills first
}

TEST(Matching, MultiLevelSweepBestFirst) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 5));
  e.process(newOrder(2, Side::Sell, 101, 5));
  e.process(newOrder(3, Side::Buy, 101, 8)); // eats 5@100 then 3@101
  auto t = s.trades();
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].price.ticks, 100); // best level first
  EXPECT_EQ(t[0].qty.v, 5);
  EXPECT_EQ(t[1].price.ticks, 101);
  EXPECT_EQ(t[1].qty.v, 3);
}

TEST(Matching, SellAggressorMirrorsBuy) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, 5));
  e.process(newOrder(2, Side::Sell, 99, 5)); // crosses down into the bid
  auto t = s.trades();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].price.ticks, 100); // resting bid's price
  EXPECT_EQ(t[0].qty.v, 5);
}

TEST(Matching, BookNeverCrosses) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 5));
  e.process(newOrder(2, Side::Buy, 105, 3)); // aggressive, fully filled
  e.process(newOrder(3, Side::Buy, 105, 9)); // eats the rest, residual rests at 105
  if (e.book().best_bid() && e.book().best_ask()) {
    EXPECT_LT(e.book().best_bid()->ticks, e.book().best_ask()->ticks);
  }
  SUCCEED();
}

TEST(Matching, DuplicateOrderIdIsRejected) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, 5));
  e.process(newOrder(1, Side::Buy, 101, 7));
  ASSERT_EQ(s.all().size(), 2u);
  EXPECT_EQ(s.all()[1].kind, OutKind::Reject);
  EXPECT_EQ(s.all()[1].reason, RejectReason::DuplicateOrderId);
  EXPECT_TRUE(e.book().validate().has_value());
  EXPECT_EQ(e.book().depth(Side::Buy, 5).size(), 1u);
}

TEST(Matching, DuplicateIdRejectedBeforeAnyMatching) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Sell, 100, 5));
  e.process(newOrder(1, Side::Buy, 100, 5));
  EXPECT_TRUE(s.trades().empty());
  EXPECT_EQ(s.all()[1].reason, RejectReason::DuplicateOrderId);
}

TEST(Matching, ZeroQuantityIsRejected) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, 0));
  ASSERT_EQ(s.all().size(), 1u);
  EXPECT_EQ(s.all()[0].kind, OutKind::Reject);
  EXPECT_EQ(s.all()[0].reason, RejectReason::BadQuantity);
  EXPECT_FALSE(e.book().best_bid());
}

TEST(Matching, NegativeQuantityIsRejected) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, -5));
  ASSERT_EQ(s.all().size(), 1u);
  EXPECT_EQ(s.all()[0].kind, OutKind::Reject);
  EXPECT_EQ(s.all()[0].reason, RejectReason::BadQuantity);
  EXPECT_FALSE(e.book().best_bid());
}

TEST(Matching, IdIsReusableAfterCancel) {
  CollectingSink s;
  MatchingEngine e{s};
  e.process(newOrder(1, Side::Buy, 100, 5));
  e.process(InEvent{EventType::Cancel, OrderId{1}, Side::Buy, Price{0}, Quantity{0}});
  e.process(newOrder(1, Side::Buy, 101, 3));
  EXPECT_EQ(s.all().back().kind, OutKind::Ack);
  ASSERT_TRUE(e.book().best_bid());
  EXPECT_EQ(e.book().best_bid()->ticks, 101);
}
