#include <dhft/bench/Samples.h>

#include <mach/mach_time.h>

#include <algorithm>
#include <cmath>

namespace dhft::bench {

namespace {

double tick_ns() {
    static const double ns = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
    }();
    return ns;
}

}

double Samples::ticks_to_ns(double ticks) noexcept { return ticks * tick_ns(); }

void Samples::finalise() {
    if (!sorted_) {
        std::sort(ticks_.begin(), ticks_.end());
        sorted_ = true;
    }
}

double Samples::percentile_ns(double p) const {
    if (ticks_.empty()) {
        return 0.0;
    }
    const double clamped = std::clamp(p, 0.0, 1.0);
    auto idx = static_cast<std::size_t>(clamped * static_cast<double>(ticks_.size() - 1));
    idx = std::min(idx, ticks_.size() - 1);
    return ticks_to_ns(static_cast<double>(ticks_[idx]));
}

double Samples::max_ns() const {
    if (ticks_.empty()) {
        return 0.0;
    }
    return ticks_to_ns(static_cast<double>(ticks_.back()));
}

std::size_t Samples::count_over_ns(double ns) const {
    const auto threshold = static_cast<std::uint32_t>(std::ceil(ns / tick_ns()));
    return static_cast<std::size_t>(
        std::distance(std::upper_bound(ticks_.begin(), ticks_.end(), threshold), ticks_.end()));
}

}
