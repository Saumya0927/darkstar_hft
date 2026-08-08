#include <dhft/bench/Harness.h>

#include <dhft/MatchingEngine.h>

#include <mach/mach_time.h>

#include <limits>

namespace dhft::bench {

namespace {

inline std::uint64_t now_ticks() noexcept { return mach_absolute_time(); }

BookDepth measure_depth(const OrderBook& book) {
    constexpr std::size_t kAll = 1'000'000;
    BookDepth d;
    d.quantity = static_cast<std::size_t>(book.total_quantity(Side::Buy).v +
                                          book.total_quantity(Side::Sell).v);
    d.levels = book.depth(Side::Buy, kAll).size() + book.depth(Side::Sell, kAll).size();
    return d;
}

}

void ChecksumSink::on_event(const OutEvent& e) noexcept {
    const auto mix = [this](std::uint64_t v) noexcept {
        hash_ ^= v;
        hash_ *= 0x100000001b3ULL;
    };
    mix(static_cast<std::uint64_t>(e.kind));
    mix(e.id.v);
    mix(e.resting.v);
    mix(static_cast<std::uint64_t>(e.price.ticks));
    mix(static_cast<std::uint64_t>(e.qty.v));
    mix(static_cast<std::uint64_t>(e.reason));
}

BatchResult run_batch(const std::vector<InEvent>& script, std::size_t warmup,
                      Counters* counters) {
    ChecksumSink sink;
    MatchingEngine engine{sink};

    const std::size_t start = std::min(warmup, script.size());
    for (std::size_t i = 0; i < start; ++i) {
        engine.process(script[i]);
    }

    const BookDepth entering = measure_depth(engine.book());

    if (counters != nullptr) {
        counters->begin();
    }
    const std::uint64_t t0 = now_ticks();
    for (std::size_t i = start; i < script.size(); ++i) {
        engine.process(script[i]);
    }
    const std::uint64_t t1 = now_ticks();
    const CounterSample sample = (counters != nullptr) ? counters->end() : CounterSample{};

    BatchResult r;
    r.counters = sample;
    r.atStart = entering;
    r.events = script.size() - start;
    r.totalNs = Samples::ticks_to_ns(static_cast<double>(t1 - t0));
    r.nsPerEvent = r.events == 0 ? 0.0 : r.totalNs / static_cast<double>(r.events);
    r.checksum = sink.value();
    r.atEnd = measure_depth(engine.book());
    return r;
}

TailResult run_tail(const std::vector<InEvent>& script, std::size_t warmup) {
    ChecksumSink sink;
    MatchingEngine engine{sink};

    const std::size_t start = std::min(warmup, script.size());
    for (std::size_t i = 0; i < start; ++i) {
        engine.process(script[i]);
    }

    std::size_t nNew = 0;
    std::size_t nCancel = 0;
    std::size_t nModify = 0;
    for (std::size_t i = start; i < script.size(); ++i) {
        switch (script[i].type) {
        case EventType::NewOrder: ++nNew; break;
        case EventType::Cancel: ++nCancel; break;
        case EventType::Modify: ++nModify; break;
        }
    }

    TailResult r{script.size() - start, nNew, nCancel, nModify};
    r.atStart = measure_depth(engine.book());
    for (std::size_t i = start; i < script.size(); ++i) {
        const std::uint64_t a = now_ticks();
        engine.process(script[i]);
        const std::uint64_t b = now_ticks();

        const std::uint64_t d = b - a;
        const auto ticks = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(d, std::numeric_limits<std::uint32_t>::max()));

        r.all.add(ticks);
        switch (script[i].type) {
        case EventType::NewOrder: r.newOrder.add(ticks); break;
        case EventType::Cancel: r.cancel.add(ticks); break;
        case EventType::Modify: r.modify.add(ticks); break;
        }
    }

    r.all.finalise();
    r.newOrder.finalise();
    r.cancel.finalise();
    r.modify.finalise();
    r.checksum = sink.value();
    r.atEnd = measure_depth(engine.book());
    return r;
}

}
