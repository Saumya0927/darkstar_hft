#pragma once

#include <cstdint>
#include <dhft/Events.h>
#include <dhft/Types.h>

#include <expected>
#include <functional>
#include <map>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>

namespace dhft {

    class OrderBook {
    public:

        struct Fill {
            OrderId restingId;
            Quantity filled;
            bool levelEmptied;
        };

        [[nodiscard]] std::expected<void, RejectReason> add(const Order& o);
        [[nodiscard]] bool contains(OrderId id) const noexcept;
        [[nodiscard]] std::optional<Price> best_bid() const noexcept;
        [[nodiscard]] std::optional<Price> best_ask() const noexcept;
        [[nodiscard]] std::vector<std::pair<Price, Quantity>> depth(Side side,
                                                                    std::size_t levels) const;
        void depth_into(Side side, std::size_t levels,
                        std::vector<std::pair<Price, Quantity>>& out) const;
        [[nodiscard]] std::expected<void, RejectReason> cancel(OrderId id);
        [[nodiscard]] std::expected<void, RejectReason> modify(OrderId id, Quantity newQty, Sequence newSeq);
        [[nodiscard]] std::optional<Order> front_at(Side side, Price price) const noexcept;
        [[nodiscard]] std::optional<Fill> take_from_front(Side side, Price price, Quantity want);
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
        std::map<Price, Level, std::greater<>> bids_;
        std::map<Price, Level> asks_;
        std::unordered_map<OrderId, std::uint32_t> index_;

        [[nodiscard]] std::uint32_t alloc_slot();
        void free_slot(std::uint32_t idx) noexcept;
        void link_back(Level& level, std::uint32_t idx) noexcept;
        void unlink(Level& level, std::uint32_t idx) noexcept;
    };

}
