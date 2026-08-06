#include <dhft/reference/NaiveEngine.h>

namespace dhft::reference {

NaiveEngine::NaiveEngine(Sink& sink) noexcept : sink_{sink} {}

std::size_t NaiveEngine::resting_count() const noexcept { return resting_.size(); }

std::optional<Price> NaiveEngine::best(Side side) const noexcept {
    std::optional<Price> found;
    for (const auto& o : resting_) {
        if (o.side != side) {
            continue;
        }
        if (!found) {
            found = o.price;
        } else if (side == Side::Buy ? (o.price > *found) : (o.price < *found)) {
            found = o.price;
        }
    }
    return found;
}

std::optional<Price> NaiveEngine::best_bid() const noexcept { return best(Side::Buy); }
std::optional<Price> NaiveEngine::best_ask() const noexcept { return best(Side::Sell); }

Quantity NaiveEngine::total_quantity(Side side) const noexcept {
    Quantity total{};
    for (const auto& o : resting_) {
        if (o.side == side) {
            total = total + o.qty;
        }
    }
    return total;
}

std::size_t NaiveEngine::index_of(OrderId id) const noexcept {
    for (std::size_t i = 0; i < resting_.size(); ++i) {
        if (resting_[i].id == id) {
            return i;
        }
    }
    return npos;
}

bool NaiveEngine::contains(OrderId id) const noexcept { return index_of(id) != npos; }

// Oldest order at this price on this side: lowest sequence wins.
std::size_t NaiveEngine::front_at(Side side, Price price) const noexcept {
    std::size_t found = npos;
    for (std::size_t i = 0; i < resting_.size(); ++i) {
        const auto& o = resting_[i];
        if (o.side != side || o.price != price) {
            continue;
        }
        if (found == npos || o.seq < resting_[found].seq) {
            found = i;
        }
    }
    return found;
}

void NaiveEngine::process(const InEvent& e) noexcept {
    switch (e.type) {
    case EventType::NewOrder: {
        if (!e.qty.positive()) {
            sink_.on_event(OutEvent::reject(e.id, RejectReason::BadQuantity));
            break;
        }
        if (contains(e.id)) {
            sink_.on_event(OutEvent::reject(e.id, RejectReason::DuplicateOrderId));
            break;
        }
        Order o{e.id, e.side, e.price, e.qty, next_};
        next_ = next_.next();
        match_and_rest(o);
        break;
    }
    case EventType::Cancel: {
        const auto at = index_of(e.id);
        if (at == npos) {
            sink_.on_event(OutEvent::reject(e.id, RejectReason::UnknownOrder));
            break;
        }
        resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(at));
        sink_.on_event(OutEvent::ack(e.id));
        break;
    }
    case EventType::Modify: {
        if (!e.qty.positive()) {
            sink_.on_event(OutEvent::reject(e.id, RejectReason::BadQuantity));
            break;
        }
        const auto at = index_of(e.id);
        if (at == npos) {
            sink_.on_event(OutEvent::reject(e.id, RejectReason::UnknownOrder));
            break;
        }
        // A decrease keeps its place; an increase loses it, which here simply means
        // taking a fresh (larger) sequence.
        if (e.qty > resting_[at].qty) {
            resting_[at].seq = next_;
        }
        resting_[at].qty = e.qty;
        next_ = next_.next();
        sink_.on_event(OutEvent::ack(e.id));
        break;
    }
    }
}

void NaiveEngine::match_and_rest(Order o) noexcept {
    const Side opposite = (o.side == Side::Buy) ? Side::Sell : Side::Buy;

    while (o.qty.positive()) {
        const auto bestPrice = best(opposite);
        if (!bestPrice) {
            break;
        }

        const bool crosses =
            (o.side == Side::Buy) ? (*bestPrice <= o.price) : (*bestPrice >= o.price);
        if (!crosses) {
            break;
        }

        const auto at = front_at(opposite, *bestPrice);
        if (at == npos) {
            break;
        }

        const OrderId restingId = resting_[at].id;
        const Quantity restingQty = resting_[at].qty;
        const Quantity fill = (o.qty <= restingQty) ? o.qty : restingQty;

        sink_.on_event(OutEvent::trade(o.id, restingId, *bestPrice, fill));
        o.qty = o.qty - fill;

        if (fill == restingQty) {
            resting_.erase(resting_.begin() + static_cast<std::ptrdiff_t>(at));
        } else {
            resting_[at].qty = restingQty - fill;
        }
    }

    if (o.qty.positive()) {
        resting_.push_back(o);
    }
    sink_.on_event(OutEvent::ack(o.id));
}

} // namespace dhft::reference
