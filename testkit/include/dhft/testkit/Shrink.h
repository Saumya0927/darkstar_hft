#pragma once

#include <dhft/Events.h>

#include <functional>
#include <vector>

namespace dhft::testkit {

// True when the script still reproduces the failure under investigation.
using FailurePredicate = std::function<bool(const std::vector<InEvent>&)>;

// Delta-debugging style minimisation: repeatedly drop chunks of events, keeping any
// shorter script that still fails, until nothing more can be removed. If the script does
// not fail to begin with it is returned unchanged.
[[nodiscard]] std::vector<InEvent> shrink(std::vector<InEvent> script,
                                          const FailurePredicate& fails);

} // namespace dhft::testkit
