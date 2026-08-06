#pragma once

#include <dhft/Events.h>

#include <cstdint>
#include <vector>

namespace dhft::testkit {

// Weights need not sum to anything in particular; they are relative.
struct GenConfig {
    std::uint64_t seed{1};
    std::size_t events{200};
    std::int64_t minPrice{95};
    std::int64_t maxPrice{105};
    std::int64_t maxQty{10};
    int weightNew{60};
    int weightCancel{25};
    int weightModify{15};
    int pctInvalidQty{3};
    int pctDuplicateId{3};
};

// Deterministic: the same config always yields the same script.
[[nodiscard]] std::vector<InEvent> generate(const GenConfig& cfg);

} // namespace dhft::testkit
