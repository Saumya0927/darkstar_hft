#include <dhft/MatchingEngine.h>
#include <dhft/Sink.h>
#include <dhft/testkit/Generate.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using namespace dhft;
using namespace dhft::testkit;

namespace {

constexpr std::uint64_t kSeeds = 500;
constexpr std::size_t kEventsPerScript = 200;

GenConfig configFor(std::uint64_t seed) {
  GenConfig cfg;
  cfg.seed = seed;
  cfg.events = kEventsPerScript;
  return cfg;
}

const std::vector<InEvent>& scriptFor(std::uint64_t seed) {
  static std::unordered_map<std::uint64_t, std::vector<InEvent>> cache;
  const auto at = cache.find(seed);
  if (at != cache.end()) {
    return at->second;
  }
  return cache.emplace(seed, generate(configFor(seed))).first->second;
}

std::string context(std::uint64_t seed, std::size_t event) {
  return "seed=" + std::to_string(seed) + " event=" + std::to_string(event);
}

std::vector<OutEvent> replay(const std::vector<InEvent>& script) {
  CollectingSink sink;
  MatchingEngine engine{sink};
  for (const auto& e : script) {
    engine.process(e);
  }
  return sink.all();
}

} // namespace

TEST(Property, InvariantsHoldAfterEveryEvent) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    CollectingSink sink;
    MatchingEngine engine{sink};
    const auto& script = scriptFor(seed);

    for (std::size_t i = 0; i < script.size(); ++i) {
      engine.process(script[i]);
      const auto r = engine.book().validate();
      ASSERT_TRUE(r.has_value()) << context(seed, i) << ": " << r.error();
    }
  }
}

TEST(Property, BookNeverCrosses) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    CollectingSink sink;
    MatchingEngine engine{sink};
    const auto& script = scriptFor(seed);

    for (std::size_t i = 0; i < script.size(); ++i) {
      engine.process(script[i]);
      const auto bid = engine.book().best_bid();
      const auto ask = engine.book().best_ask();
      if (bid && ask) {
        ASSERT_LT(bid->ticks, ask->ticks) << context(seed, i);
      }
    }
  }
}

TEST(Property, NoOrderOverFills) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    CollectingSink sink;
    MatchingEngine engine{sink};
    const auto& script = scriptFor(seed);

    std::unordered_map<std::uint64_t, std::int64_t> outstanding;
    std::size_t seen = 0;

    for (std::size_t i = 0; i < script.size(); ++i) {
      const auto& in = script[i];
      engine.process(in);
      const auto& out = sink.all();

      const bool rejected = (out.size() == seen + 1 && out[seen].kind == OutKind::Reject);
      if (in.type == EventType::NewOrder && !rejected) {
        outstanding[in.id.v] = in.qty.v;
      }

      for (std::size_t k = seen; k < out.size(); ++k) {
        if (out[k].kind != OutKind::Trade) {
          continue;
        }
        outstanding[out[k].id.v] -= out[k].qty.v;
        outstanding[out[k].resting.v] -= out[k].qty.v;
        ASSERT_GE(outstanding[out[k].id.v], 0)
            << context(seed, i) << ": aggressor " << out[k].id.v << " over-filled";
        ASSERT_GE(outstanding[out[k].resting.v], 0)
            << context(seed, i) << ": resting " << out[k].resting.v << " over-filled";
      }

      if (!rejected) {
        if (in.type == EventType::Cancel) {
          outstanding[in.id.v] = 0;
        } else if (in.type == EventType::Modify) {
          outstanding[in.id.v] = in.qty.v;
        }
      }
      seen = out.size();
    }
  }
}

TEST(Property, TradesRespectTheAggressorLimit) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    CollectingSink sink;
    MatchingEngine engine{sink};
    const auto& script = scriptFor(seed);
    std::size_t seen = 0;

    for (std::size_t i = 0; i < script.size(); ++i) {
      const auto& in = script[i];
      engine.process(in);
      const auto& out = sink.all();

      for (std::size_t k = seen; k < out.size(); ++k) {
        if (out[k].kind != OutKind::Trade) {
          continue;
        }
        ASSERT_EQ(in.type, EventType::NewOrder) << context(seed, i) << ": trade from a non-order";
        if (in.side == Side::Buy) {
          ASSERT_LE(out[k].price.ticks, in.price.ticks) << context(seed, i) << ": buy paid above limit";
        } else {
          ASSERT_GE(out[k].price.ticks, in.price.ticks) << context(seed, i) << ": sell sold below limit";
        }
      }
      seen = out.size();
    }
  }
}

TEST(Property, QuantityIsConserved) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    CollectingSink sink;
    MatchingEngine engine{sink};
    const auto& script = scriptFor(seed);

    std::unordered_map<std::uint64_t, std::int64_t> outstanding;
    std::int64_t expected = 0;
    std::size_t seen = 0;

    for (std::size_t i = 0; i < script.size(); ++i) {
      const auto& in = script[i];
      engine.process(in);
      const auto& out = sink.all();

      const bool rejected = (out.size() == seen + 1 && out[seen].kind == OutKind::Reject);

      if (in.type == EventType::NewOrder && !rejected) {
        outstanding[in.id.v] = in.qty.v;
        expected += in.qty.v;
      }

      // Each trade removes its quantity twice: once from the incoming order that is not
      // yet resting, and once from the resting order it hit.
      for (std::size_t k = seen; k < out.size(); ++k) {
        if (out[k].kind == OutKind::Trade) {
          outstanding[out[k].id.v] -= out[k].qty.v;
          outstanding[out[k].resting.v] -= out[k].qty.v;
          expected -= 2 * out[k].qty.v;
        }
      }

      if (!rejected) {
        if (in.type == EventType::Cancel) {
          expected -= outstanding[in.id.v];
          outstanding[in.id.v] = 0;
        } else if (in.type == EventType::Modify) {
          expected += in.qty.v - outstanding[in.id.v];
          outstanding[in.id.v] = in.qty.v;
        }
      }

      const std::int64_t actual = engine.book().total_quantity(Side::Buy).v +
                                  engine.book().total_quantity(Side::Sell).v;
      ASSERT_EQ(expected, actual) << context(seed, i) << ": quantity was created or lost";
      seen = out.size();
    }
  }
}

TEST(Property, ReplayIsDeterministic) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    const auto& script = scriptFor(seed);
    ASSERT_EQ(replay(script), replay(script)) << "seed=" << seed;
  }
}
