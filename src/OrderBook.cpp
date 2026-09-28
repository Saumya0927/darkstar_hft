#include <dhft/OrderBook.h>

#include <dhft/Check.h>
#include <dhft/Events.h>
#include <dhft/Types.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <optional>
#include <utility>

namespace dhft {

    OrderBook::OrderBook(std::size_t expectedOrders, Price minPrice, Price maxPrice)
        : bids_{minPrice, maxPrice}, asks_{minPrice, maxPrice} {
        index_.reserve(expectedOrders);
        pool_.reserve(expectedOrders);
    }

    std::expected<void, RejectReason> OrderBook::add(const Order& o) {
        if (!o.qty.positive()) {
            return std::unexpected(RejectReason::BadQuantity);
        }

        DHFT_CHECK_MSG(std::in_range<std::uint32_t>(o.id.v), "order id does not fit in 32 bits");
        DHFT_CHECK_MSG(std::in_range<std::int32_t>(o.price.ticks), "price does not fit in 32 bits");
        DHFT_CHECK_MSG(std::in_range<std::int32_t>(o.qty.v), "quantity does not fit in 32 bits");
        DHFT_CHECK_MSG(std::in_range<std::uint32_t>(o.seq.v), "sequence does not fit in 32 bits");

        const std::uint32_t idx = alloc_slot();

        auto place = [&](detail::PriceLevelBook auto& m) -> std::expected<void, RejectReason> {
            try {
                if (!index_.try_emplace(o.id, idx).second) {
                    free_slot(idx);
                    return std::unexpected(RejectReason::DuplicateOrderId);
                }

                Level& lvl = m.insert(o.price);

                Slot& s = pool_[idx];
                s.id = static_cast<std::uint32_t>(o.id.v);
                s.price = static_cast<std::int32_t>(o.price.ticks);
                s.qty = static_cast<std::int32_t>(o.qty.v);
                s.seq = static_cast<std::uint32_t>(o.seq.v);
                s.side = static_cast<std::uint8_t>(o.side);

                link_back(lvl, idx);
            } catch (...) {
                index_.erase(o.id);
                free_slot(idx);
                throw;
            }
            return {};
        };
        return (o.side == Side::Buy) ? place(bids_) : place(asks_);
    }

    bool OrderBook::contains(OrderId id) const noexcept {
        return index_.contains(id);
    }

    std::optional<Price> OrderBook::best_bid() const noexcept { return bids_.best(); }

    std::optional<Price> OrderBook::best_ask() const noexcept { return asks_.best(); }

    void OrderBook::depth_into(Side side, std::size_t levels,
                               std::vector<std::pair<Price, Quantity>>& out) const {
        out.clear();

        auto walk = [&](const detail::PriceLevelBook auto& m) {
            m.for_each([&](Price price, const Level& level) {
                Quantity total{};
                for (std::uint32_t i = level.head; i != kNull; i = pool_[i].next)
                    total = total + Quantity{pool_[i].qty};
                out.emplace_back(price, total);
            });
        };

        if (side == Side::Buy) {
            walk(bids_);
            std::ranges::sort(out, [](const auto& a, const auto& b) { return a.first > b.first; });
        } else {
            walk(asks_);
            std::ranges::sort(out, [](const auto& a, const auto& b) { return a.first < b.first; });
        }

        if (out.size() > levels) {
            out.resize(levels);
        }
    }

    std::vector<std::pair<Price, Quantity>> OrderBook::depth(Side side, std::size_t levels) const {
        std::vector<std::pair<Price, Quantity>> result;
        depth_into(side, levels, result);
        return result;
    }

    std::expected<void, RejectReason> OrderBook::cancel(OrderId id) noexcept {
        auto it = index_.find(id);
        if (it == index_.end())
            return std::unexpected(RejectReason::UnknownOrder);

        const std::uint32_t idx = it->second;
        DHFT_DCHECK(idx < pool_.size());

        const Price price{pool_[idx].price};
        const Side side = static_cast<Side>(pool_[idx].side);

        auto removeFrom = [&](detail::PriceLevelBook auto& m) {
            Level* lvl = m.find(price);
            DHFT_CHECK_MSG(lvl != nullptr, "index points at a price level that does not exist");

            unlink(*lvl, idx);
            if (lvl->head == kNull)
                m.erase(price);
        };

        if (side == Side::Buy) {
            removeFrom(bids_);
        } else {
            removeFrom(asks_);
        }

        free_slot(idx);
        index_.erase(it);
        return {};

    }

    std::expected<void, RejectReason> OrderBook::modify(OrderId id, Quantity newQty, Sequence newSeq) {
        if (!newQty.positive())
            return std::unexpected(RejectReason::BadQuantity);

        auto it = index_.find(id);
        if (it == index_.end())
            return std::unexpected(RejectReason::UnknownOrder);

        const std::uint32_t idx = it->second;
        DHFT_DCHECK(idx < pool_.size());

        if (newQty <= Quantity{pool_[idx].qty}) {
            pool_[idx].qty = static_cast<std::int32_t>(newQty.v);
            return {};
        }

        const Order updated{id,
                            static_cast<Side>(pool_[idx].side),
                            Price{pool_[idx].price},
                            newQty,
                            newSeq};
        (void)cancel(id);
        const auto readded = add(updated);
        DHFT_CHECK_MSG(readded.has_value(), "re-adding a just-cancelled order must succeed");
        return {};
    }

    std::optional<Order> OrderBook::front_at(Side side, Price price) const noexcept {
        auto lookup = [&](const detail::PriceLevelBook auto& m) -> std::optional<Order> {
            const Level* level = m.find(price);
            if (level == nullptr)
                return std::nullopt;

            DHFT_CHECK_MSG(level->head != kNull, "a price level must never be empty");

            const Slot& s = pool_[level->head];
            return Order{OrderId{s.id},
                         static_cast<Side>(s.side),
                         Price{s.price},
                         Quantity{s.qty},
                         Sequence{s.seq}};
        };
        return (side == Side::Buy) ? lookup(bids_) : lookup(asks_);
    }

    Quantity OrderBook::total_quantity(Side side) const noexcept {
        Quantity total{};

        auto sum = [&](const detail::PriceLevelBook auto& m) {
            m.for_each([&](Price, const Level& level) {
                for (std::uint32_t i = level.head; i != kNull; i = pool_[i].next) {
                    total = total + Quantity{pool_[i].qty};
                }
            });
        };

        if (side == Side::Buy) {
            sum(bids_);
        } else {
            sum(asks_);
        }

        return total;
    }

    std::expected<void, std::string> OrderBook::validate() const {
        if (auto r = bids_.validate(); !r.has_value()) {
            return std::unexpected("bid ladder: " + r.error());
        }
        if (auto r = asks_.validate(); !r.has_value()) {
            return std::unexpected("ask ladder: " + r.error());
        }

        std::vector<bool> seen(pool_.size(), false);
        std::size_t counted = 0;

        auto check_level = [&](Price price, const Level& level,
                               Side side) -> std::expected<void, std::string> {
            auto at = [price] { return " at price " + std::to_string(price.ticks); };

            if (level.head == kNull || level.tail == kNull) {
                return std::unexpected("empty level" + at());
            }

            std::uint32_t prevIdx = kNull;
            Sequence prevSeq{};
            bool first = true;

            for (std::uint32_t i = level.head; i != kNull; i = pool_[i].next) {
                if (i >= pool_.size()) {
                    return std::unexpected("link out of range" + at());
                }
                if (seen[i]) {
                    return std::unexpected("slot " + std::to_string(i) +
                                           " appears in more than one chain");
                }
                seen[i] = true;
                ++counted;

                const Slot& s = pool_[i];
                auto who = [&s] { return "order " + std::to_string(s.id); };

                if (s.qty <= 0) {
                    return std::unexpected(who() + " has non-positive quantity");
                }
                if (Price{s.price} != price) {
                    return std::unexpected(who() + " price " + std::to_string(s.price) +
                                           " does not match its level" + at());
                }
                if (s.side > static_cast<std::uint8_t>(Side::Sell)) {
                    return std::unexpected(who() + " has an invalid side byte " +
                                           std::to_string(s.side));
                }
                if (static_cast<Side>(s.side) != side) {
                    return std::unexpected(who() + " sits on the wrong side of the book");
                }
                if (s.prev != prevIdx) {
                    return std::unexpected("backward link is broken at " + who());
                }

                const auto entry = index_.find(OrderId{s.id});
                if (entry == index_.end()) {
                    return std::unexpected(who() + " is missing from the index");
                }
                if (entry->second != i) {
                    return std::unexpected("index entry for " + who() + " is stale");
                }

                if (!first && !(prevSeq < Sequence{s.seq})) {
                    return std::unexpected("sequence is not ascending" + at() + ", " + who() +
                                           " has sequence " + std::to_string(s.seq));
                }

                prevSeq = Sequence{s.seq};
                prevIdx = i;
                first = false;
            }

            if (prevIdx != level.tail) {
                return std::unexpected("tail does not end the chain" + at());
            }
            return {};
        };

        auto check_side = [&](const detail::PriceLevelBook auto& m, Side side) -> std::expected<void, std::string> {
            std::expected<void, std::string> result{};
            m.for_each([&](Price price, const Level& level) {
                if (result.has_value()) {
                    result = check_level(price, level, side);
                }
            });
            return result;
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

        std::size_t freeCount = 0;
        for (std::uint32_t i = freeHead_; i != kNull; i = pool_[i].next) {
            if (i >= pool_.size()) {
                return std::unexpected("free-list link out of range");
            }
            if (seen[i]) {
                return std::unexpected("slot " + std::to_string(i) + " is both live and free");
            }
            seen[i] = true;
            if (pool_[i].prev != kNull) {
                return std::unexpected("free slot " + std::to_string(i) +
                                       " still carries a prev link");
            }
            ++freeCount;
        }

        if (counted + freeCount != pool_.size()) {
            return std::unexpected("slot accounting: " + std::to_string(counted) + " live + " +
                                   std::to_string(freeCount) + " free != " +
                                   std::to_string(pool_.size()) + " in the pool");
        }

        return {};
    }

    std::optional<OrderBook::Fill> OrderBook::take_from_front(Side side, Price price, Quantity want) noexcept {
        auto takeFrom = [&](detail::PriceLevelBook auto& m) -> std::optional<Fill> {
            Level* lvl = m.find(price);
            if (lvl == nullptr)
                return std::nullopt;

            Level& level = *lvl;
            DHFT_CHECK_MSG(level.head != kNull, "a price level must never be empty");

            const std::uint32_t idx = level.head;
            DHFT_DCHECK(idx < pool_.size());
            Slot& slot = pool_[idx];

            const Quantity restingQty{slot.qty};
            const OrderId id{slot.id};
            const Quantity fill = (want <= restingQty) ? want : restingQty;

            bool emptied = false;
            if (fill == restingQty) {
                unlink(level, idx);
                free_slot(idx);
                index_.erase(id);
                if (level.head == kNull) {
                    m.erase(price);
                    emptied = true;
                }
            } else {
                slot.qty = static_cast<std::int32_t>((restingQty - fill).v);
            }
            return Fill{id, fill, emptied};
        };
        return (side == Side::Buy) ? takeFrom(bids_) : takeFrom(asks_);
    }

    void OrderBook::free_slot(std::uint32_t idx) noexcept {
        pool_[idx].next = freeHead_;
        freeHead_ = idx;
    }

    std::uint32_t OrderBook::alloc_slot() {
        if (freeHead_ != kNull) {
            const std::uint32_t idx = freeHead_;
            freeHead_ = pool_[idx].next;
            pool_[idx] = Slot{};
            return idx;
        }

        DHFT_CHECK_MSG(pool_.size() < kNull, "order pool exhausted");
        pool_.push_back(Slot{});
        return static_cast<std::uint32_t>(pool_.size() - 1);
    }

    void OrderBook::link_back(Level& level, std::uint32_t idx) noexcept {
        DHFT_DCHECK(idx < pool_.size());

        pool_[idx].next = kNull;
        pool_[idx].prev = level.tail;

        if (level.tail == kNull) {
            level.head = idx;
        } else {
            pool_[level.tail].next = idx;
        }

        level.tail = idx;
    }

    void OrderBook::unlink(Level& level, std::uint32_t idx) noexcept {
        DHFT_DCHECK(idx < pool_.size());

        const std::uint32_t p = pool_[idx].prev;
        const std::uint32_t n = pool_[idx].next;

        if (p != kNull) {
            pool_[p].next = n;
        } else {
            level.head = n;
        }

        if (n != kNull) {
            pool_[n].prev = p;
        } else {
            level.tail = p;
        }

        pool_[idx].next = kNull;
        pool_[idx].prev = kNull;
    }

}
