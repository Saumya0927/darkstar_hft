#include <dhft/Check.h>
#include <dhft/MatchingEngine.h>


namespace dhft {

    MatchingEngine::MatchingEngine(Sink& sink) noexcept : sink_{sink} {}

    const OrderBook& MatchingEngine::book() const noexcept { return book_; }

    void MatchingEngine::process(const InEvent& e) noexcept {
        switch (e.type) {
            case EventType::NewOrder: {
                if (!e.qty.positive()) {
                    sink_.on_event(OutEvent{OutKind::Reject, e.id, OrderId{0}, Price{0}, Quantity{0},
                                            RejectReason::BadQuantity});
                    break;
                }
                if (book_.contains(e.id)) {
                    sink_.on_event(OutEvent{OutKind::Reject, e.id, OrderId{0}, Price{0}, Quantity{0},
                                            RejectReason::DuplicateOrderId});
                    break;
                }
                Order o{e.id, e.side, e.price, e.qty, next_};
                next_ = next_.next();
                match_and_rest(o);
                break;
            }
            case EventType::Cancel: {
                auto r = book_.cancel(e.id);
                if (r.has_value()) {
                    sink_.on_event(OutEvent{OutKind::Ack, e.id});
                } else {
                    sink_.on_event(OutEvent{OutKind::Reject, e.id, OrderId{0}, Price{0}, Quantity{0}, r.error()});
                }
                break;
            }
            case EventType::Modify: {
                const Sequence s = next_;
                next_ = next_.next();
                auto r = book_.modify(e.id, e.qty, s);
                if (r.has_value()) {
                    sink_.on_event(OutEvent{OutKind::Ack, e.id});
                } else {
                    sink_.on_event(OutEvent{OutKind::Reject, e.id, OrderId{0}, Price{0}, Quantity{0}, r.error()});
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

            const Order* resting = book_.front_at(opposite, *best);
            if (!resting) break;

            Quantity fill = (o.qty <= resting->qty) ? o.qty : resting->qty;

            const OrderId restingId = resting->id;
            const Quantity restingQty = resting->qty;
            const Sequence restingSeq = resting->seq;

            sink_.on_event(OutEvent{OutKind::Trade, o.id, restingId, *best, fill});

            o.qty = o.qty - fill;

            if (fill == restingQty) {
                (void)book_.cancel(restingId);
            } else {
                (void)book_.modify(restingId, restingQty - fill, restingSeq);
            }
        }

        if (o.qty.positive()) {
            const auto rested = book_.add(o);
            DHFT_CHECK_MSG(rested.has_value(), "residual of a validated order must rest");
        }
        sink_.on_event(OutEvent{OutKind::Ack, o.id});
    }
}
