#include <dhft/bench/Harness.h>
#include <dhft/bench/Samples.h>
#include <dhft/testkit/Generate.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dhft;
using namespace dhft::bench;

namespace {

std::vector<InEvent> script(std::size_t n, std::uint64_t seed = 1) {
  testkit::GenConfig cfg;
  cfg.seed = seed;
  cfg.events = n;
  return testkit::generate(cfg);
}

}

TEST(Samples, EmptyIsSafe) {
  Samples s{16};
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.count(), 0u);
  EXPECT_DOUBLE_EQ(s.percentile_ns(0.99), 0.0);
  EXPECT_DOUBLE_EQ(s.max_ns(), 0.0);
}

TEST(Samples, PercentilesPickTheRightElement) {
  Samples s{128};
  for (std::uint32_t i = 1; i <= 100; ++i) {
    s.add(i);
  }
  s.finalise();
  EXPECT_EQ(s.count(), 100u);
  EXPECT_DOUBLE_EQ(s.percentile_ns(0.5), Samples::ticks_to_ns(50));
  EXPECT_DOUBLE_EQ(s.percentile_ns(0.0), Samples::ticks_to_ns(1));
  EXPECT_DOUBLE_EQ(s.percentile_ns(1.0), Samples::ticks_to_ns(100));
  EXPECT_DOUBLE_EQ(s.max_ns(), Samples::ticks_to_ns(100));
}

TEST(Samples, PercentileArgumentIsClamped) {
  Samples s{8};
  s.add(5);
  s.add(9);
  s.finalise();
  EXPECT_DOUBLE_EQ(s.percentile_ns(-1.0), Samples::ticks_to_ns(5));
  EXPECT_DOUBLE_EQ(s.percentile_ns(2.0), Samples::ticks_to_ns(9));
}

TEST(Samples, SortsIndependentOfInsertionOrder) {
  Samples a{8};
  Samples b{8};
  for (std::uint32_t v : {7u, 1u, 5u, 3u}) {
    a.add(v);
  }
  for (std::uint32_t v : {1u, 3u, 5u, 7u}) {
    b.add(v);
  }
  a.finalise();
  b.finalise();
  EXPECT_DOUBLE_EQ(a.percentile_ns(0.5), b.percentile_ns(0.5));
  EXPECT_DOUBLE_EQ(a.max_ns(), b.max_ns());
}

TEST(Samples, CountsOverThreshold) {
  Samples s{64};
  for (std::uint32_t i = 1; i <= 50; ++i) {
    s.add(i);
  }
  s.finalise();
  EXPECT_EQ(s.count_over_ns(0.0), 50u);
  EXPECT_LT(s.count_over_ns(100.0), 50u);
  EXPECT_EQ(s.count_over_ns(1e9), 0u);
}

TEST(Samples, TicksConvertUsingTheHostTimebase) {
  EXPECT_NEAR(Samples::ticks_to_ns(3.0), 125.0, 1e-9);
  EXPECT_NEAR(Samples::ticks_to_ns(0.0), 0.0, 1e-9);
}

TEST(ChecksumSink, IdenticalStreamsHashIdentically) {
  ChecksumSink a;
  ChecksumSink b;
  const auto e1 = OutEvent::ack(OrderId{1});
  const auto e2 = OutEvent::trade(OrderId{2}, OrderId{1}, Price{100}, Quantity{3});
  a.on_event(e1);
  a.on_event(e2);
  b.on_event(e1);
  b.on_event(e2);
  EXPECT_EQ(a.value(), b.value());
}

TEST(ChecksumSink, AnyFieldChangeChangesTheHash) {
  const auto base = OutEvent::trade(OrderId{2}, OrderId{1}, Price{100}, Quantity{3});
  ChecksumSink ref;
  ref.on_event(base);

  const std::vector<OutEvent> variants = {
      OutEvent::trade(OrderId{3}, OrderId{1}, Price{100}, Quantity{3}),
      OutEvent::trade(OrderId{2}, OrderId{9}, Price{100}, Quantity{3}),
      OutEvent::trade(OrderId{2}, OrderId{1}, Price{101}, Quantity{3}),
      OutEvent::trade(OrderId{2}, OrderId{1}, Price{100}, Quantity{4}),
      OutEvent::ack(OrderId{2}),
      OutEvent::reject(OrderId{2}, RejectReason::UnknownOrder),
  };
  for (const auto& v : variants) {
    ChecksumSink s;
    s.on_event(v);
    EXPECT_NE(s.value(), ref.value());
  }
}

TEST(ChecksumSink, OrderOfEventsMatters) {
  const auto e1 = OutEvent::ack(OrderId{1});
  const auto e2 = OutEvent::ack(OrderId{2});
  ChecksumSink a;
  a.on_event(e1);
  a.on_event(e2);
  ChecksumSink b;
  b.on_event(e2);
  b.on_event(e1);
  EXPECT_NE(a.value(), b.value());
}

TEST(Harness, BatchAndTailAgreeOnBehaviour) {
  const auto s = script(20000);
  const auto batch = run_batch(s, 5000);
  const auto tail = run_tail(s, 5000);
  EXPECT_EQ(batch.checksum, tail.checksum);
  EXPECT_EQ(batch.atStart.quantity, tail.atStart.quantity);
  EXPECT_EQ(batch.atStart.levels, tail.atStart.levels);
  EXPECT_EQ(batch.atEnd.quantity, tail.atEnd.quantity);
  EXPECT_EQ(batch.atEnd.levels, tail.atEnd.levels);
}

TEST(Harness, IsDeterministicAcrossRuns) {
  const auto s = script(20000);
  EXPECT_EQ(run_batch(s, 5000).checksum, run_batch(s, 5000).checksum);
}

TEST(Harness, WarmupIsExcludedFromMeasurement) {
  const auto s = script(10000);
  EXPECT_EQ(run_batch(s, 0).events, 10000u);
  EXPECT_EQ(run_batch(s, 4000).events, 6000u);
  EXPECT_EQ(run_tail(s, 4000).all.count(), 6000u);
}

TEST(Harness, WarmupBeyondScriptLengthIsSafe) {
  const auto s = script(1000);
  const auto r = run_batch(s, 5000);
  EXPECT_EQ(r.events, 0u);
  EXPECT_DOUBLE_EQ(r.nsPerEvent, 0.0);
}

TEST(Harness, PerTypeSamplesPartitionTheWhole) {
  const auto s = script(20000);
  const auto t = run_tail(s, 5000);
  EXPECT_EQ(t.newOrder.count() + t.cancel.count() + t.modify.count(), t.all.count());
}

TEST(Harness, WarmupChangesStartingDepthNotFinalDepth) {
  const auto s = script(40000);
  const auto shallow = run_batch(s, 100);
  const auto deep = run_batch(s, 30000);

  EXPECT_EQ(shallow.atEnd.quantity, deep.atEnd.quantity);
  EXPECT_EQ(shallow.atEnd.levels, deep.atEnd.levels);
  EXPECT_GT(deep.atStart.quantity, shallow.atStart.quantity);
  EXPECT_GT(deep.atStart.levels, shallow.atStart.levels);
}

TEST(Harness, ZeroWarmupStartsFromAnEmptyBook) {
  const auto s = script(10000);
  const auto r = run_batch(s, 0);
  EXPECT_EQ(r.atStart.quantity, 0u);
  EXPECT_EQ(r.atStart.levels, 0u);
  EXPECT_GT(r.atEnd.levels, 0u);
}

TEST(Harness, ReportsAPlausibleCost) {
  const auto s = script(50000);
  const auto r = run_batch(s, 10000);
  EXPECT_GT(r.nsPerEvent, 1.0);
  EXPECT_LT(r.nsPerEvent, 100000.0);
}
