#pragma once

#include <dhft/Events.h>
#include <dhft/Types.h>
#include <dhft/PriceLadder.h>

#include <concepts>
#include <cstdint>
#include <expected>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <utility>

#include <ankerl/unordered_dense.h>

template <>
struct ankerl::unordered_dense::hash<dhft::OrderId> {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const dhft::OrderId& id) const noexcept -> std::uint64_t {
        return ankerl::unordered_dense::hash<std::uint64_t>{}(id.v);
    }
};

static_assert(ankerl::unordered_dense::hash_is_avalanching_v<
                    ankerl::unordered_dense::hash<dhft::OrderId>>);

namespace dhft {

namespace detail {

template <typename M>
concept PriceLevelBook = requires(M& m, Price p) {
    { m.erase(p) } -> std::same_as<void>;
    { m.best() } -> std::same_as<std::optional<Price>>;
};

}

    class OrderBook {
    public:
        explicit OrderBook(std::size_t expectedOrders = 65536,
                           Price minPrice = Price{0},
                           Price maxPrice = Price{16383});

        struct Fill {
            OrderId restingId{};
            Quantity filled{};
            bool levelEmptied{};
        };

        [[nodiscard]] std::expected<void, RejectReason> add(const Order& o);
        [[nodiscard]] bool contains(OrderId id) const noexcept;
        [[nodiscard]] std::optional<Price> best_bid() const noexcept;
        [[nodiscard]] std::optional<Price> best_ask() const noexcept;
        [[nodiscard]] std::vector<std::pair<Price, Quantity>> depth(Side side,
                                                                    std::size_t levels) const;
        void depth_into(Side side, std::size_t levels,
                        std::vector<std::pair<Price, Quantity>>& out) const;
        [[nodiscard]] std::expected<void, RejectReason> cancel(OrderId id) noexcept;
        [[nodiscard]] std::expected<void, RejectReason> modify(OrderId id, Quantity newQty, Sequence newSeq);
        [[nodiscard]] std::optional<Order> front_at(Side side, Price price) const noexcept;
        [[nodiscard]] std::optional<Fill> take_from_front(Side side, Price price, Quantity want) noexcept;
        [[nodiscard]] Quantity total_quantity(Side side) const noexcept;
        [[nodiscard]] std::expected<void, std::string> validate() const;

    private:
        static constexpr std::uint32_t kNull = 0xFFFFFFFFu;

        struct Slot {
            std::uint32_t id{};
            std::int32_t price{};
            std::int32_t qty{};
            std::uint32_t seq{};
            std::uint32_t next{kNull};
            std::uint32_t prev{kNull};
            std::uint8_t side{};
        };
        static_assert(sizeof(Slot) == 28, "Slot layout regressed");

        struct Level {
            std::uint32_t head{kNull};
            std::uint32_t tail{kNull};
        };

        std::vector<Slot> pool_;
        std::uint32_t freeHead_{kNull};
        PriceLadder<Side::Buy, Level> bids_;
        PriceLadder<Side::Sell, Level> asks_;
        ankerl::unordered_dense::map<OrderId, std::uint32_t> index_;

        [[nodiscard]] std::uint32_t alloc_slot();
        void free_slot(std::uint32_t idx) noexcept;
        void link_back(Level& level, std::uint32_t idx) noexcept;
        void unlink(Level& level, std::uint32_t idx) noexcept;
    };

}
