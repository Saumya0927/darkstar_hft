#pragma once

#include <dhft/Events.h>
#include <dhft/Sink.h>
#include <dhft/OrderBook.h>

namespace dhft {

    class MatchingEngine {
    public:
        explicit MatchingEngine(Sink& sink) noexcept;
        void process(const InEvent& e) noexcept;
        [[nodiscard]] const OrderBook& book() const noexcept;

    private:
        OrderBook book_;
        Sink& sink_;
        Sequence next_{0};
        void match_and_rest(Order o) noexcept;
    };

}
