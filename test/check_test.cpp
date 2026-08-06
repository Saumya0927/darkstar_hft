#include <dhft/Check.h>
#include <gtest/gtest.h>

TEST(Check, PassesSilentlyWhenTrue) {
  DHFT_CHECK(1 + 1 == 2);
  DHFT_CHECK_MSG(true, "should not print");
  SUCCEED();
}

TEST(Check, AbortsWhenFalse) {
  EXPECT_DEATH(DHFT_CHECK(1 == 2), "DHFT_CHECK failed: 1 == 2");
}

TEST(Check, AbortsWithMessage) {
  EXPECT_DEATH(DHFT_CHECK_MSG(false, "book was crossed"), "book was crossed");
}

TEST(Check, EvaluatesConditionOnlyOnce) {
  int calls = 0;
  auto bump = [&calls] {
    ++calls;
    return true;
  };
  DHFT_CHECK(bump());
  EXPECT_EQ(calls, 1);
}

TEST(Check, WorksAsSingleStatementInUnbracedIf) {
  bool taken = false;
  if (false)
    DHFT_CHECK(1 == 2);
  else
    taken = true;
  EXPECT_TRUE(taken);
}
