#include <dhft/bench/Counters.h>
#include <gtest/gtest.h>

#include <unistd.h>

using namespace dhft::bench;

TEST(Counters, ConstructionNeverThrowsAndAlwaysReportsStatus) {
  Counters c;
  EXPECT_FALSE(c.status().empty());
}

TEST(Counters, AvailabilityMatchesPrivilege) {
  Counters c;
  if (geteuid() != 0) {
    EXPECT_FALSE(c.available()) << "unprivileged process must not claim PMU access";
  }
  SUCCEED();
}

TEST(Counters, UnavailableCountersDegradeQuietly) {
  Counters c;
  if (c.available()) {
    GTEST_SKIP() << "running with PMU access; degradation path not exercised";
  }
  EXPECT_FALSE(c.begin());
  const auto s = c.end();
  EXPECT_FALSE(s.valid);
  EXPECT_EQ(s.cycles, 0u);
  EXPECT_EQ(s.instructions, 0u);
}

TEST(Counters, StatusExplainsWhyWhenUnavailable) {
  Counters c;
  if (c.available()) {
    GTEST_SKIP() << "counters available";
  }
  EXPECT_NE(c.status(), "ok");
  EXPECT_GT(c.status().size(), 10u) << "status must be a usable explanation, not a code";
}

TEST(Counters, ResolvesEventsEvenWithoutRoot) {
  Counters c;
  const auto& events = c.resolved_events();
  if (events.empty()) {
    GTEST_SKIP() << "no PMU event database on this machine: " << c.status();
  }
  EXPECT_LE(events.size(), 8u);
  for (const auto& e : events) {
    EXPECT_FALSE(e.empty());
  }
}

TEST(Counters, RepeatedUseIsSafe) {
  Counters c;
  for (int i = 0; i < 3; ++i) {
    c.begin();
    const auto s = c.end();
    EXPECT_EQ(s.valid, c.available());
  }
}

TEST(Counters, MeasuresRealWorkWhenAvailable) {
  Counters c;
  if (!c.available()) {
    GTEST_SKIP() << c.status();
  }
  ASSERT_TRUE(c.begin());
  volatile std::uint64_t acc = 0;
  for (std::uint64_t i = 0; i < 5'000'000; ++i) {
    acc += i;
  }
  const auto s = c.end();
  ASSERT_TRUE(s.valid);
  EXPECT_GT(s.cycles, 0u);
  EXPECT_GT(s.instructions, 0u);
}
