#include <dhft/Check.h>
#include <dhft/OrderBook.h>


namespace dhft {

    std::expected<void, RejectReason> OrderBook::add(const Order& o) {
        if (!o.qty.positive()) {
            return std::unexpected(RejectReason::BadQuantity);
        }
        if (index_.contains(o.id)) {
            return std::unexpected(RejectReason::DuplicateOrderId);
        }

        auto& lvl = (o.side == Side::Buy) ? bids_[o.price] : asks_[o.price];
        lvl.push_back(o);
        index_[o.id] = Location{o.side, o.price, std::prev(lvl.end())};
        return {};
    }

    bool OrderBook::contains(OrderId id) const noexcept {
        return index_.contains(id);
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
            DHFT_CHECK_MSG(lvlIt != m.end(), "index points at a price level that does not exist");
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

    std::expected<void, RejectReason> OrderBook::modify(OrderId id, Quantity newQty, Sequence newSeq) {
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
        updated.seq = newSeq;
        (void)cancel(id);
        const auto readded = add(updated);
        DHFT_CHECK_MSG(readded.has_value(), "re-adding a just-cancelled order must succeed");
        return {};
    }

    const Order* OrderBook::front_at(Side side, Price price) const noexcept {
        if (side == Side::Buy) {
            auto it = bids_.find(price);
            if (it == bids_.end())
                return nullptr;
            DHFT_CHECK_MSG(!it->second.empty(), "a price level must never be empty");
            return &(it->second.front());
        } else {
            auto it = asks_.find(price);
            if (it == asks_.end())
                return nullptr;
            DHFT_CHECK_MSG(!it->second.empty(), "a price level must never be empty");
            return &(it->second.front());
        }
    }

    Quantity OrderBook::total_quantity(Side side) const noexcept {
        std::int64_t total = 0;

        auto sum = [&](const auto& m) {
            for (const auto& entry : m) {
                for (const auto& order : entry.second) {
                    total += order.qty.v;
                }
            }
        };

        if (side == Side::Buy) {
            sum(bids_);
        } else {
            sum(asks_);
        }

        return Quantity{total};
    }

    std::expected<void, std::string> OrderBook::validate() const {
        std::size_t counted = 0;

        auto checkSide = [&](const auto& m, Side side) -> std::expected<void, std::string> {
            for (const auto& [price, level] : m) {
                if (level.empty()) {
                    return std::unexpected("empty level at price " + std::to_string(price.ticks));
                }

                bool first = true;
                Sequence prev{};

                for (const auto& order : level) {
                    ++counted;
                    auto who = [&order] { return "order " + std::to_string(order.id.v); };

                    if (!order.qty.positive()) {
                        return std::unexpected(who() + " has non-positive quantity");
                    }

                    if (order.price != price) {
                        return std::unexpected(who() + " price " +
                                               std::to_string(order.price.ticks) +
                                               " does not match its level " +
                                               std::to_string(price.ticks));
                    }

                    if (order.side != side) {
                        return std::unexpected(who() + " sits on the wrong side of the book");
                    }

                    auto it = index_.find(order.id);
                    if (it == index_.end()) {
                        return std::unexpected(who() + " is missing from the index");
                    }

                    const Location& loc = it->second;
                    if (loc.side != order.side || loc.price != order.price ||
                        &(*loc.node) != &order) {
                        return std::unexpected("index entry for " + who() + " is stale");
                    }

                    if (!first && !(prev < order.seq)) {
                        return std::unexpected("sequence is not ascending at price " +
                                               std::to_string(price.ticks) + ", " + who() +
                                               " has sequence " + std::to_string(order.seq.v));
                    }

                    prev = order.seq;
                    first = false;
                }
            }
            return {};
        };

        if (auto r = checkSide(bids_, Side::Buy); !r.has_value()) {
            return r;
        }
        if (auto r = checkSide(asks_, Side::Sell); !r.has_value()) {
            return r;
        }

        if (index_.size() != counted) {
            return std::unexpected("index holds " + std::to_string(index_.size()) +
                                   " entries but the book holds " + std::to_string(counted) +
                                   " orders");
        }

        return {};
    }
}
