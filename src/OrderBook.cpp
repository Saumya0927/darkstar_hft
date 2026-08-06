#include <dhft/Check.h>
#include <dhft/OrderBook.h>


namespace dhft {

    std::expected<void, RejectReason> OrderBook::add(const Order& o) {
        if (!o.qty.positive()) {
            return std::unexpected(RejectReason::BadQuantity);
        }

        auto [slot, inserted] = index_.try_emplace(o.id);
        if (!inserted) {
            return std::unexpected(RejectReason::DuplicateOrderId);
        }

        auto place = [&](auto& m) {
            auto& lvl = m[o.price];
            try {
                lvl.push_back(o);
            } catch (...) {
                if (lvl.empty()) {
                    m.erase(o.price);
                }
                index_.erase(slot);
                throw;
            }
            slot->second = Location{o.side, o.price, std::prev(lvl.end())};
        };

        if (o.side == Side::Buy) {
            place(bids_);
        } else {
            place(asks_);
        }
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

    void OrderBook::depth_into(Side side, std::size_t levels,
                               std::vector<std::pair<Price, Quantity>>& out) const {
        out.clear();

        auto walk = [&](const auto& m) {
            std::size_t count = 0;
            for (const auto& [price, level] : m) {
                if (count == levels)
                    break;
                Quantity total{};
                for (const auto& ord : level)
                    total = total + ord.qty;
                out.emplace_back(price, total);
                ++count;
            }
        };

        if (side == Side::Buy) {
            walk(bids_);
        } else {
            walk(asks_);
        }
    }

    std::vector<std::pair<Price, Quantity>> OrderBook::depth(Side side, std::size_t levels) const {
        std::vector<std::pair<Price, Quantity>> result;
        depth_into(side, levels, result);
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
        Quantity total{};

        auto sum = [&](const auto& m) {
            for (const auto& entry : m) {
                for (const auto& order : entry.second) {
                    total = total + order.qty;
                }
            }
        };

        if (side == Side::Buy) {
            sum(bids_);
        } else {
            sum(asks_);
        }

        return total;
    }

    std::expected<void, std::string> OrderBook::validate() const {
        std::size_t counted = 0;

        auto check_order = [&](const Order& order, Price levelPrice,
                               Side side) -> std::expected<void, std::string> {
            auto who = [&order] { return "order " + std::to_string(order.id.v); };

            if (!order.qty.positive()) {
                return std::unexpected(who() + " has non-positive quantity");
            }
            if (order.price != levelPrice) {
                return std::unexpected(who() + " price " + std::to_string(order.price.ticks) +
                                       " does not match its level " +
                                       std::to_string(levelPrice.ticks));
            }
            if (order.side != side) {
                return std::unexpected(who() + " sits on the wrong side of the book");
            }

            const auto entry = index_.find(order.id);
            if (entry == index_.end()) {
                return std::unexpected(who() + " is missing from the index");
            }

            const Location& loc = entry->second;
            if (loc.side != order.side || loc.price != order.price || &(*loc.node) != &order) {
                return std::unexpected("index entry for " + who() + " is stale");
            }
            return {};
        };

        auto check_level = [&](Price price, const Level& level,
                               Side side) -> std::expected<void, std::string> {
            if (level.empty()) {
                return std::unexpected("empty level at price " + std::to_string(price.ticks));
            }

            bool first = true;
            Sequence prev{};
            for (const auto& order : level) {
                ++counted;
                if (auto r = check_order(order, price, side); !r.has_value()) {
                    return r;
                }
                if (!first && !(prev < order.seq)) {
                    return std::unexpected("sequence is not ascending at price " +
                                           std::to_string(price.ticks) + ", order " +
                                           std::to_string(order.id.v) + " has sequence " +
                                           std::to_string(order.seq.v));
                }
                prev = order.seq;
                first = false;
            }
            return {};
        };

        auto check_side = [&](const auto& m, Side side) -> std::expected<void, std::string> {
            for (const auto& [price, level] : m) {
                if (auto r = check_level(price, level, side); !r.has_value()) {
                    return r;
                }
            }
            return {};
        };

        if (auto r = check_side(bids_, Side::Buy); !r.has_value()) {
            return r;
        }
        if (auto r = check_side(asks_, Side::Sell); !r.has_value()) {
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
