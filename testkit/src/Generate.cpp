#include <dhft/testkit/Generate.h>

#include <random>

namespace dhft::testkit {

std::vector<InEvent> generate(const GenConfig& cfg) {
    std::vector<InEvent> events;
    events.reserve(cfg.events);

    std::mt19937_64 rng{cfg.seed};
    std::discrete_distribution<int> kind{
        static_cast<double>(cfg.weightNew),
        static_cast<double>(cfg.weightCancel),
        static_cast<double>(cfg.weightModify),
    };
    std::uniform_int_distribution<std::int64_t> priceDist{cfg.minPrice, cfg.maxPrice};
    std::uniform_int_distribution<std::int64_t> qtyDist{1, cfg.maxQty};
    std::bernoulli_distribution buyDist{0.5};

    // Ids we have issued and not yet cancelled. Some of these will already have been
    // filled by the engine, so cancels and modifies against them reject naturally --
    // which is wanted: it exercises the reject paths without generating pure garbage.
    std::vector<OrderId> live;
    std::uint64_t nextId = 1;

    const auto emitNew = [&] {
        const OrderId id{nextId++};
        live.push_back(id);
        events.push_back(InEvent::new_order(id, buyDist(rng) ? Side::Buy : Side::Sell,
                                            Price{priceDist(rng)}, Quantity{qtyDist(rng)}));
    };

    while (events.size() < cfg.events) {
        const int choice = live.empty() ? 0 : kind(rng);

        if (choice == 0) {
            emitNew();
            continue;
        }

        std::uniform_int_distribution<std::size_t> pick{0, live.size() - 1};
        const std::size_t at = pick(rng);

        if (choice == 1) {
            events.push_back(InEvent::cancel(live[at]));
            live[at] = live.back();
            live.pop_back();
        } else {
            events.push_back(InEvent::modify(live[at], Quantity{qtyDist(rng)}));
        }
    }

    return events;
}

} // namespace dhft::testkit
