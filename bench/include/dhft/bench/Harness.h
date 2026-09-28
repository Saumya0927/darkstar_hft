#pragma once

#include <dhft/Events.h>
#include <dhft/Sink.h>
#include <dhft/bench/Counters.h>
#include <dhft/bench/Samples.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dhft::bench {

class ChecksumSink final : public Sink {
public:
    void on_event(const OutEvent& e) noexcept override;
    [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_{0xcbf29ce484222325ULL};
};

struct BookDepth {
    std::size_t quantity{};
    std::size_t levels{};
};

struct BatchResult {
    std::size_t events{};
    double totalNs{};
    double nsPerEvent{};
    std::uint64_t checksum{};
    BookDepth atStart{};
    BookDepth atEnd{};
    CounterSample counters{};
};

struct TailResult {
    Samples all;
    Samples newOrder;
    Samples cancel;
    Samples modify;
    std::uint64_t checksum{};
    BookDepth atStart{};
    BookDepth atEnd{};

    TailResult(std::size_t total, std::size_t nNew, std::size_t nCancel, std::size_t nModify)
        : all{total}, newOrder{nNew}, cancel{nCancel}, modify{nModify} {}
};

[[nodiscard]] BatchResult run_batch(const std::vector<InEvent>& script, std::size_t warmup,
                                   Counters* counters = nullptr);

[[nodiscard]] TailResult run_tail(const std::vector<InEvent>& script, std::size_t warmup);

}
