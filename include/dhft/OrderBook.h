#pragma once

#include <dhft/Events.h>
#include <dhft/Types.h>

#include <expected>
#include <functional>
#include <list>
#include <map>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <utility>


namespace dhft {

    struct Fill {
        OrderId restingId;
        Quantity filled;
        bool levelEmptied;
    };

    class OrderBook {
    public:
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
        [[nodiscard]] const Order* front_at(Side side, Price price) const noexcept;
        [[nodiscard]] std::optional<Fill> take_from_front(Side side, Price price, Quantity want);
        [[nodiscard]] Quantity total_quantity(Side side) const noexcept;
        [[nodiscard]] std::expected<void, std::string> validate() const;

    private:
        using Level = std::list<Order>;
        struct Location { Side side{}; Price price{}; Level::iterator node{}; };
        std::map<Price, Level, std::greater<>> bids_;
        std::map<Price, Level> asks_;
        std::unordered_map<OrderId, Location> index_;
    };

}
