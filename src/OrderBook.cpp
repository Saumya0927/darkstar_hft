#include <dhft/OrderBook.h>


namespace dhft {

    void OrderBook::add(const Order& o) {
        auto& lvl = (o.side == Side::Buy) ? bids_[o.price] : asks_[o.price];
        lvl.push_back(o);
        index_[o.id] = Location{o.side, o.price, std::prev(lvl.end())};
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

    std::expected<void, RejectReason> OrderBook::cancel(OrderId id) {
        auto it = index_.find(id);
        if (it == index_.end())
            return std::unexpected(RejectReason::UnknownOrder);

        const Location loc = it->second;

        auto removeFrom = [&](auto& m) {
            auto lvlIt = m.find(loc.price);
            lvlIt->second.erase(loc.node);
            if (lvlIt->second.empty())
                m.erase(lvlIt);
        };

        if (loc.side == Side::Buy) {
            removeFrom(bids_);
        } else {
            removeFrom(asks_);
        }

        index_.erase(it);
        return {};
    }

    std::expected<void, RejectReason> OrderBook::modify(OrderId id, Quantity newQty) {
        if (!newQty.positive())
            return std::unexpected(RejectReason::BadQuantity);

        auto it = index_.find(id);
        if (it == index_.end())
            return std::unexpected(RejectReason::UnknownOrder);

        const Location loc = it->second;

        if (newQty <= loc.node->qty) {
            loc.node->qty = newQty;
            return {};
        }

        Order updated = *loc.node;
        updated.qty = newQty;
        (void)cancel(id);
        add(updated);
        return {};
    }

    const Order* OrderBook::front_at(Side side, Price price) const noexcept {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it == bids_.end())
                return nullptr;
            return &(it->second.front());
        } else {
            auto it = asks_.find(price);
            if (it == asks_.end())
                return nullptr;
            return &(it->second.front());
        }
    }
}
