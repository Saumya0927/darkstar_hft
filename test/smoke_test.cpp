// Throwaway smoke test — proves the toolchain (clang C++26 + gtest + sanitizers) works.
// Delete once Task 1's real tests exist.
#include <gtest/gtest.h>

TEST(Smoke, Builds) { ASSERT_EQ(1 + 1, 2); }
