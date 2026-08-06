#include <dhft/testkit/Shrink.h>

#include <algorithm>
#include <utility>

namespace dhft::testkit {

std::vector<InEvent> shrink(std::vector<InEvent> script, const FailurePredicate& fails) {
    if (script.empty() || !fails(script)) {
        return script;
    }

    // Start by removing large chunks, then halve the granularity down to single events.
    // Every accepted removal strictly shortens the script, so this always terminates.
    std::size_t chunk = std::max<std::size_t>(script.size() / 2, 1);
    while (true) {
        bool progress = true;
        while (progress) {
            progress = false;
            std::size_t start = 0;
            while (start + chunk <= script.size()) {
                auto candidate = script;
                candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(start),
                                candidate.begin() + static_cast<std::ptrdiff_t>(start + chunk));
                if (fails(candidate)) {
                    script = std::move(candidate);
                    progress = true;
                } else {
                    start += chunk;
                }
            }
        }
        if (chunk == 1) {
            break;
        }
        chunk /= 2;
    }

    return script;
}

} // namespace dhft::testkit
