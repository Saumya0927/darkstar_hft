#pragma once

#include <dhft/Events.h>
#include <dhft/Sink.h>
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
};

struct TailResult {
    Samples all;
    Samples newOrder;
    Samples cancel;
    Samples modify;
    std::uint64_t checksum{};
    BookDepth atStart{};
    BookDepth atEnd{};

    explicit TailResult(std::size_t capacity)
        : all{capacity}, newOrder{capacity}, cancel{capacity}, modify{capacity} {}
};

[[nodiscard]] BatchResult run_batch(const std::vector<InEvent>& script, std::size_t warmup);

[[nodiscard]] TailResult run_tail(const std::vector<InEvent>& script, std::size_t warmup);

}
