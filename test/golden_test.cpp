#include <dhft/testkit/Golden.h>
#include <gtest/gtest.h>

#include <string>

using namespace dhft::testkit;

namespace {

const std::string& goldenDir() {
  static const std::string dir = DHFT_GOLDEN_DIR;
  return dir;
}

} // namespace

TEST(Golden, DirectoryHasScripts) {
  const auto names = golden_names(goldenDir());
  EXPECT_GE(names.size(), 5u) << "expected at least five golden scripts in " << goldenDir();
}

class GoldenScript : public ::testing::TestWithParam<std::string> {};

TEST_P(GoldenScript, MatchesExpectedOutput) {
  const auto outcome = check_golden(goldenDir(), GetParam());
  EXPECT_TRUE(outcome.ok) << outcome.detail;
}

INSTANTIATE_TEST_SUITE_P(AllScripts, GoldenScript,
                         ::testing::ValuesIn(golden_names(DHFT_GOLDEN_DIR)),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                           return info.param;
                         });
