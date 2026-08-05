#include <dhft/Feed.h>
#include <dhft/Sink.h>
#include <gtest/gtest.h>

using namespace dhft;

TEST(Feed, ScriptedYieldsEventsInOrderThenStops) {
  ScriptedFeed f{{InEvent{EventType::NewOrder, OrderId{1}, Side::Buy, Price{100}, Quantity{5}},
                  InEvent{EventType::Cancel, OrderId{1}, Side::Buy, Price{0}, Quantity{0}}}};
  InEvent e{};
  ASSERT_TRUE(f.next(e));
  EXPECT_EQ(e.id.v, 1u);
  EXPECT_EQ(e.type, EventType::NewOrder);
  ASSERT_TRUE(f.next(e));
  EXPECT_EQ(e.type, EventType::Cancel);
  EXPECT_FALSE(f.next(e));
}

TEST(Feed, EmptyScriptYieldsNothing) {
  ScriptedFeed f{{}};
  InEvent e{};
  EXPECT_FALSE(f.next(e));
}

TEST(Sink, CollectingRecordsEverything) {
  CollectingSink s;
  s.on_event(OutEvent{OutKind::Ack, OrderId{1}});
  s.on_event(OutEvent{OutKind::Trade, OrderId{2}, OrderId{1}, Price{100}, Quantity{3}});
  EXPECT_EQ(s.all().size(), 2u);
}

TEST(Sink, CollectingFiltersTrades) {
  CollectingSink s;
  s.on_event(OutEvent{OutKind::Ack, OrderId{1}});
  s.on_event(OutEvent{OutKind::Trade, OrderId{2}, OrderId{1}, Price{100}, Quantity{3}});
  s.on_event(OutEvent{OutKind::Reject, OrderId{9}, OrderId{0}, Price{0}, Quantity{0},
                      RejectReason::UnknownOrder});
  auto t = s.trades();
  ASSERT_EQ(t.size(), 1u);
  EXPECT_EQ(t[0].aggressor.v, 2u);
  EXPECT_EQ(t[0].resting.v, 1u);
  EXPECT_EQ(t[0].price.ticks, 100);
  EXPECT_EQ(t[0].qty.v, 3);
}

TEST(Sink, PolymorphicThroughBaseReference) {
  CollectingSink s;
  Sink& base = s;
  base.on_event(OutEvent{OutKind::Ack, OrderId{7}});
  EXPECT_EQ(s.all().size(), 1u);
}
