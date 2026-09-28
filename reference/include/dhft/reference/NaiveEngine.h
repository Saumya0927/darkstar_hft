#pragma once

#include <dhft/Events.h>
#include <dhft/Sink.h>
#include <dhft/Types.h>

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace dhft::reference {

class NaiveEngine {
public:
    explicit NaiveEngine(Sink& sink) noexcept;

    void process(const InEvent& e) noexcept;

    [[nodiscard]] std::size_t resting_count() const noexcept;
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] Quantity total_quantity(Side side) const noexcept;
    [[nodiscard]] std::vector<std::pair<Price, Quantity>> depth(Side side,
                                                                std::size_t levels) const;

private:
    std::vector<Order> resting_;
    Sink& sink_;
    Sequence next_{0};

    [[nodiscard]] std::optional<Price> best(Side side) const noexcept;
    [[nodiscard]] std::size_t index_of(OrderId id) const noexcept;
    [[nodiscard]] std::size_t front_at(Side side, Price price) const noexcept;
    [[nodiscard]] bool contains(OrderId id) const noexcept;
    void match_and_rest(Order o) noexcept;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
};

} // namespace dhft::reference
