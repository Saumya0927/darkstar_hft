#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>


namespace dhft {

    enum class Side : std::uint8_t {Buy, Sell};

    struct Price {
        std::int64_t ticks{};
        [[nodiscard]] constexpr auto operator<=>(const Price&) const = default;
    };

    struct Quantity {
        std::int64_t v{};
        [[nodiscard]] constexpr auto operator<=>(const Quantity&) const = default;
        [[nodiscard]] constexpr Quantity operator+(Quantity o) const noexcept { return Quantity{v + o.v}; }
        [[nodiscard]] constexpr Quantity operator-(Quantity o) const noexcept { return Quantity{v - o.v}; }
        [[nodiscard]] constexpr bool positive() const noexcept { return v > 0; }
    };

    struct OrderId {
         std::uint64_t v{};
         [[nodiscard]] constexpr bool operator==(const OrderId&) const = default;
    };

    struct Sequence {
        std::uint64_t v{};
        [[nodiscard]] constexpr Sequence next() const noexcept {return Sequence{v + 1}; }
        [[nodiscard]] constexpr auto operator<=>(const Sequence&) const = default;
    };

    struct Order { OrderId id{}; Side side{}; Price price{}; Quantity qty{}; Sequence seq{}; };
    struct Trade { OrderId aggressor{}; OrderId resting{}; Price price{}; Quantity qty{}; };

}

template<>
struct std::hash<dhft::OrderId> {
    std::size_t operator()(const dhft::OrderId &id) const noexcept {
        return std::hash<std::uint64_t>{}(id.v);
    }
};
