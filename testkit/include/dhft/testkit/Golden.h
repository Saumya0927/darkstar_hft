#pragma once

#include <dhft/Events.h>

#include <string>
#include <vector>

namespace dhft::testkit {

[[nodiscard]] std::string run_to_text(const std::vector<InEvent>& events);

struct GoldenOutcome {
    bool ok{};
    std::string detail;
};

// Runs <dir>/<name>.script and compares against <dir>/<name>.expected.
// With DHFT_UPDATE_GOLDEN set in the environment, rewrites the expected file instead.
[[nodiscard]] GoldenOutcome check_golden(const std::string& dir, const std::string& name);

[[nodiscard]] std::vector<std::string> golden_names(const std::string& dir);

} // namespace dhft::testkit
