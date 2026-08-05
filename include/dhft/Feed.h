#pragma once

#include <dhft/Events.h>

#include <cstddef>
#include <utility>
#include <vector>


namespace dhft {

    struct Feed {
         virtual ~Feed() = default;
         virtual bool next(InEvent& out) = 0;
    };

    class ScriptedFeed : public Feed {
    public:
        explicit ScriptedFeed(std::vector<InEvent> events) : events_{std::move(events)} {}

        bool next(InEvent& out) override {
            if (pos_ >= events_.size())
                return false;
            out = events_[pos_];
            ++pos_;
            return true;
        }

    private:
        std::vector<InEvent> events_;
        std::size_t pos_{0};

    };

}
