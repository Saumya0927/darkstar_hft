#pragma once

#include <dhft/Events.h>
#include <vector>


namespace dhft {

    struct Sink {
         virtual ~Sink() = default;
         virtual void on_event(const OutEvent& e) = 0;
    };

    class CollectingSink : public Sink {
    public:
        void on_event(const OutEvent& e) override { events_.push_back(e); };
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
