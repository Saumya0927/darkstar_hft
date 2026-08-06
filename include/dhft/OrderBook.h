#pragma once

#include <dhft/Events.h>
#include <dhft/Types.h>

#include <expected>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <vector>
#include <unordered_map>
#include <utility>


namespace dhft {

    class OrderBook {
    public:
        void add(const Order& o);
        [[nodiscard]] std::optional<Price> best_bid() const noexcept;
        [[nodiscard]] std::optional<Price> best_ask() const noexcept;
        [[nodiscard]] std::vector<std::pair<Price, Quantity>> depth(Side side, int levels) const;
        [[nodiscard]] std::expected<void, RejectReason> cancel(OrderId id);
        [[nodiscard]] std::expected<void, RejectReason> modify(OrderId id, Quantity newQty, Sequence newSeq);
        [[nodiscard]] const Order* front_at(Side side, Price price) const noexcept;

    private:
        using Level = std::list<Order>;
        struct Location { Side side; Price price; Level::iterator node; };
        std::map<Price, Level, std::greater<>> bids_;
        std::map<Price, Level> asks_;
        std::unordered_map<OrderId, Location> index_;
    };

}
