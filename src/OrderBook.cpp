#include <dhft/OrderBook.h>


namespace dhft {

    void OrderBook::add(const Order& o) {
        auto& lvl = (o.side == Side::Buy) ? bids_[o.price] : asks_[o.price];
        lvl.push_back(o);
    }

    std::optional<Price> OrderBook::best_bid() const noexcept {
        if (bids_.empty()) {
            return std::nullopt;
        }
        return bids_.begin()->first;
    }

    std::optional<Price> OrderBook::best_ask() const noexcept {
        if (asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->first;
    }

    std::vector<std::pair<Price, Quantity>> OrderBook::depth(Side side, int levels) const {
        std::vector<std::pair<Price, Quantity>> result;

        auto walk = [&](const auto& m) {
            int count = 0;
            for (const auto& [price, level] : m) {
                if (count == levels)
                    break;
                std::int64_t total = 0;
                for (const auto& ord : level)
                    total += ord.qty.v;
                result.push_back({price, Quantity{total}});
                ++count;
            }
        };

        if (side == Side::Buy) {
            walk(bids_);
        } else {
            walk(asks_);
        }

        return result;
    }

}
