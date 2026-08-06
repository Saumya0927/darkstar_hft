#include <dhft/testkit/Generate.h>
#include <dhft/testkit/Shrink.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace dhft;
using namespace dhft::testkit;

namespace {

std::vector<InEvent> sampleScript(std::size_t events) {
  GenConfig cfg;
  cfg.seed = 4;
  cfg.events = events;
  return generate(cfg);
}

bool containsId(const std::vector<InEvent>& script, std::uint64_t id) {
  return std::any_of(script.begin(), script.end(),
                     [id](const InEvent& e) { return e.id.v == id; });
}

} // namespace

TEST(Shrink, ReducesToTheSingleOffendingEvent) {
  auto script = sampleScript(200);
  script.push_back(InEvent::new_order(OrderId{99999}, Side::Buy, Price{100}, Quantity{7}));

  const auto minimal =
      shrink(script, [](const std::vector<InEvent>& s) { return containsId(s, 99999); });

  ASSERT_EQ(minimal.size(), 1u);
  EXPECT_EQ(minimal[0].id.v, 99999u);
}

TEST(Shrink, KeepsExactlyTheEventsTheFailureNeeds) {
  const auto script = sampleScript(200);

  // Fails only while at least three events remain.
  const auto minimal =
      shrink(script, [](const std::vector<InEvent>& s) { return s.size() >= 3; });

  EXPECT_EQ(minimal.size(), 3u);
}

TEST(Shrink, ReturnsInputUnchangedWhenItDoesNotFail) {
  const auto script = sampleScript(50);
  const auto minimal = shrink(script, [](const std::vector<InEvent>&) { return false; });
  EXPECT_EQ(minimal.size(), script.size());
}

TEST(Shrink, ReducesToEmptyWhenEverythingFails) {
  const auto script = sampleScript(50);
  const auto minimal = shrink(script, [](const std::vector<InEvent>&) { return true; });
  EXPECT_TRUE(minimal.empty());
}

TEST(Shrink, HandlesEmptyInput) {
  const std::vector<InEvent> empty;
  EXPECT_TRUE(shrink(empty, [](const std::vector<InEvent>&) { return true; }).empty());
}

TEST(Shrink, PreservesRelativeOrder) {
  auto script = sampleScript(200);
  script.push_back(InEvent::new_order(OrderId{7001}, Side::Buy, Price{100}, Quantity{1}));
  script.push_back(InEvent::new_order(OrderId{7002}, Side::Sell, Price{100}, Quantity{1}));

  const auto minimal = shrink(script, [](const std::vector<InEvent>& s) {
    return containsId(s, 7001) && containsId(s, 7002);
  });

  ASSERT_EQ(minimal.size(), 2u);
  EXPECT_EQ(minimal[0].id.v, 7001u);
  EXPECT_EQ(minimal[1].id.v, 7002u);
}
