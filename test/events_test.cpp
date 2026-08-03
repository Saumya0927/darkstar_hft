// Task 2 — Events.h: InEvent / OutEvent as plain, trivially-copyable messages.
#include <dhft/Events.h>
#include <gtest/gtest.h>

#include <type_traits>

using namespace dhft;

TEST(Events, EventsAreTriviallyCopyable) {
  static_assert(std::is_trivially_copyable_v<InEvent>);
  static_assert(std::is_trivially_copyable_v<OutEvent>);
  SUCCEED();
}

TEST(Events, InEventCarriesNewOrder) {
  InEvent e{EventType::NewOrder, OrderId{1}, Side::Buy, Price{100}, Quantity{5}};
  EXPECT_EQ(e.type, EventType::NewOrder);
  EXPECT_EQ(e.id.v, 1u);
  EXPECT_EQ(e.side, Side::Buy);
  EXPECT_EQ(e.price.ticks, 100);
  EXPECT_EQ(e.qty.v, 5);
}

TEST(Events, OutEventCarriesTrade) {
  OutEvent o{OutKind::Trade, OrderId{2}, OrderId{1}, Price{100}, Quantity{3}, RejectReason::None};
  EXPECT_EQ(o.kind, OutKind::Trade);
  EXPECT_EQ(o.id.v, 2u);
  EXPECT_EQ(o.resting.v, 1u);
  EXPECT_EQ(o.qty.v, 3);
}

TEST(Events, OutEventRejectHasReason) {
  OutEvent o{OutKind::Reject, OrderId{9}, OrderId{0}, Price{0}, Quantity{0}, RejectReason::UnknownOrder};
  EXPECT_EQ(o.kind, OutKind::Reject);
  EXPECT_EQ(o.reason, RejectReason::UnknownOrder);
}
