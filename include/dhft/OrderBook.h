#pragma once

#include <dhft/Types.h>

#include <functional>
#include <list>
#include <map>
#include <optional>
#include <vector>
#include <utility>


namespace dhft {

    class OrderBook {
    public:
        void add(const Order& o);
        [[nodiscard]] std::optional<Price> best_bid() const noexcept;
        [[nodiscard]] std::optional<Price> best_ask() const noexcept;
        [[nodiscard]] std::vector<std::pair<Price, Quantity>> depth(Side side, int levels) const;

    private:
        using Level = std::list<Order>;
        std::map<Price, Level, std::greater<>> bids_;
        std::map<Price, Level> asks_;
    };

}
