// Task 8 — end-to-end: script parsing, the full loop, and determinism.
#include <dhft/Feed.h>
#include <dhft/MatchingEngine.h>
#include <dhft/Script.h>
#include <dhft/Sink.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>

using namespace dhft;

namespace {

const char* const kScript = R"(
# a two-sided book, a cross, a cancel, a modify, and a bad cancel
N 1 BUY  99  5
N 2 BUY  100 3
N 3 SELL 102 4
N 4 BUY  100 7
N 5 SELL 100 5    # crosses: 3 from #2 then 2 from #4
C 3
M 4 2
C 3               # already gone -> reject
)";

std::vector<InEvent> script() {
  std::istringstream in{kScript};
  return parse_script(in);
}

std::string run_to_text(const std::vector<InEvent>& events) {
  std::ostringstream out;
  PrintingSink sink{out};
  MatchingEngine engine{sink};
  for (const auto& e : events) {
    engine.process(e);
  }
  return out.str();
}

} // namespace

TEST(Script, ParsesEachOpKind) {
  std::istringstream in{"N 1 BUY 100 5\nC 1\nM 1 3\n"};
  auto ev = parse_script(in);
  ASSERT_EQ(ev.size(), 3u);
  EXPECT_EQ(ev[0].type, EventType::NewOrder);
  EXPECT_EQ(ev[0].side, Side::Buy);
  EXPECT_EQ(ev[0].price.ticks, 100);
  EXPECT_EQ(ev[0].qty.v, 5);
  EXPECT_EQ(ev[1].type, EventType::Cancel);
  EXPECT_EQ(ev[1].id.v, 1u);
  EXPECT_EQ(ev[2].type, EventType::Modify);
  EXPECT_EQ(ev[2].qty.v, 3);
}

TEST(Script, IgnoresBlankLinesAndComments) {
  std::istringstream in{"\n# just a comment\n   \nN 1 SELL 100 5   # trailing comment\n"};
  auto ev = parse_script(in);
  ASSERT_EQ(ev.size(), 1u);
  EXPECT_EQ(ev[0].side, Side::Sell);
}

TEST(Script, RejectsMalformedLine) {
  std::istringstream in{"X 1 BUY 100 5\n"};
  EXPECT_THROW((void)parse_script(in), std::runtime_error);
}

TEST(Integration, ScriptProducesExpectedTrades) {
  auto ev = script();
  CollectingSink sink;
  MatchingEngine engine{sink};
  for (const auto& e : ev) {
    engine.process(e);
  }

  auto t = sink.trades();
  ASSERT_EQ(t.size(), 2u);
  EXPECT_EQ(t[0].resting.v, 2u); // oldest at 100 fills first
  EXPECT_EQ(t[0].qty.v, 3);
  EXPECT_EQ(t[0].price.ticks, 100);
  EXPECT_EQ(t[1].resting.v, 4u);
  EXPECT_EQ(t[1].qty.v, 2);

  // #4 had 7, 2 traded away, then modified down to 2
  EXPECT_EQ(engine.book().depth(Side::Buy, 1)[0].second.v, 2);
  EXPECT_FALSE(engine.book().best_ask()); // #3 cancelled, #5 fully filled
}

// The property the whole architecture is built on: same input -> identical output.
TEST(Integration, ReplayIsDeterministic) {
  auto ev = script();
  const std::string first = run_to_text(ev);
  const std::string second = run_to_text(ev);
  EXPECT_FALSE(first.empty());
  EXPECT_EQ(first, second);
}
