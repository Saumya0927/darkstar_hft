#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dhft::bench {

class Samples {
public:
    explicit Samples(std::size_t capacity) { ticks_.reserve(capacity); }

    // Clearing sorted_ here means a late add() can never leave percentile_ns() reading a
    // half-sorted vector; the alternative was a convention nothing enforced.
    void add(std::uint32_t ticks) {
        ticks_.push_back(ticks);
        sorted_ = false;
    }

    [[nodiscard]] std::size_t count() const noexcept { return ticks_.size(); }
    [[nodiscard]] bool empty() const noexcept { return ticks_.empty(); }

    void finalise();

    [[nodiscard]] double percentile_ns(double p) const;
    [[nodiscard]] double max_ns() const;
    [[nodiscard]] std::size_t count_over_ns(double ns) const;

    static double ticks_to_ns(double ticks) noexcept;

private:
    std::vector<std::uint32_t> ticks_;
    bool sorted_{false};
};

}
