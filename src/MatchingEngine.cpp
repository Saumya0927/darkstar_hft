#include <dhft/Events.h>
#include <dhft/Check.h>
#include <dhft/MatchingEngine.h>

namespace dhft {

    MatchingEngine::MatchingEngine(Sink& sink) noexcept : sink_{sink} {}

    const OrderBook& MatchingEngine::book() const noexcept { return book_; }

    void MatchingEngine::process(const InEvent& e) noexcept {
        switch (e.type) {
            case EventType::NewOrder: {
                if (!e.qty.positive()) {
                    sink_.on_event(OutEvent::reject(e.id, RejectReason::BadQuantity));
                    break;
                }
                if (book_.contains(e.id)) {
                    sink_.on_event(OutEvent::reject(e.id, RejectReason::DuplicateOrderId));
                    break;
                }
                Order o{e.id, e.side, e.price, e.qty, next_};
                next_ = next_.next();
                match_and_rest(o);
                break;
            }
            case EventType::Cancel: {
                const auto r = book_.cancel(e.id);
                if (r.has_value()) {
                    sink_.on_event(OutEvent::ack(e.id));
                } else {
                    sink_.on_event(OutEvent::reject(e.id, r.error()));
                }
                break;
            }
            case EventType::Modify: {
                const auto r = book_.modify(e.id, e.qty, next_);
                if (r.has_value()) {
                    next_ = next_.next();
                    sink_.on_event(OutEvent::ack(e.id));
                } else {
                    sink_.on_event(OutEvent::reject(e.id, r.error()));
                }
                break;
            }
        }
    }

    void MatchingEngine::match_and_rest(Order o) noexcept {
        const Side opposite = (o.side == Side::Buy) ? Side::Sell : Side::Buy;

        while (o.qty.positive()) {
            auto best = (o.side == Side::Buy ) ? book_.best_ask() : book_.best_bid();
            if (!best) break;

            bool crosses = (o.side == Side::Buy) ? (*best <= o.price) : (*best >= o.price);
            if (!crosses) break;

            const auto fill = book_.take_from_front(opposite, *best, o.qty);

            if (!fill) break;

            sink_.on_event(OutEvent::trade(o.id, fill->restingId, *best, fill->filled));

            o.qty = o.qty - fill->filled;
        }

        if (o.qty.positive()) {
            const auto rested = book_.add(o);
            DHFT_CHECK_MSG(rested.has_value(), "residual of a validated order must rest");
        }
        sink_.on_event(OutEvent::ack(o.id));
    }
}
