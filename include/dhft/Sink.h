#pragma once

#include <dhft/Events.h>
#include <vector>


namespace dhft {

    struct Sink {
        Sink() = default;
        Sink(const Sink&) = delete;
        Sink& operator=(const Sink&) = delete;
        Sink(Sink&&) = delete;
        Sink& operator=(Sink&&) = delete;
        virtual ~Sink() = default;

        virtual void on_event(const OutEvent& e) noexcept = 0;
    };

    class CollectingSink : public Sink {
    public:
        void on_event(const OutEvent& e) noexcept override { events_.push_back(e); }
        [[nodiscard]] const std::vector<OutEvent>& all() const noexcept { return events_; }
        [[nodiscard]] std::vector<Trade> trades() const {
            std::vector<Trade> result;
            for (const auto& e : events_) {
                if (e.kind == OutKind::Trade)
                    result.push_back(Trade{e.id, e.resting, e.price, e.qty});
            }
            return result;
        }

    private:
        std::vector<OutEvent> events_;

    };

}
