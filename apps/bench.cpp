#include <dhft/bench/Counters.h>
#include <dhft/bench/Harness.h>
#include <dhft/testkit/Generate.h>

#include <pthread/qos.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Options {
    bool json{false};
    int reps{9};
    std::size_t events{400000};
    std::size_t warmup{50000};
    std::uint64_t seed{1};
    std::int64_t minPrice{9900};
    std::int64_t maxPrice{10100};
};

Options parse(int argc, char** argv) {
    Options o;
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto next = [&](std::size_t at) -> long long {
            return at + 1 < args.size() ? std::stoll(args[at + 1]) : 0;
        };
        if (args[i] == "--json") {
            o.json = true;
        } else if (args[i] == "--reps") {
            o.reps = static_cast<int>(next(i));
            ++i;
        } else if (args[i] == "--events") {
            o.events = static_cast<std::size_t>(next(i));
            ++i;
        } else if (args[i] == "--warmup") {
            o.warmup = static_cast<std::size_t>(next(i));
            ++i;
        } else if (args[i] == "--seed") {
            o.seed = static_cast<std::uint64_t>(next(i));
            ++i;
        } else if (args[i] == "--min-price") {
            o.minPrice = static_cast<std::int64_t>(next(i));
            ++i;
        } else if (args[i] == "--max-price") {
            o.maxPrice = static_cast<std::int64_t>(next(i));
            ++i;
        }
    }
    return o;
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    dhft::testkit::GenConfig cfg;
    cfg.seed = opt.seed;
    cfg.events = opt.events;
    cfg.minPrice = opt.minPrice;
    cfg.maxPrice = opt.maxPrice;
    const auto script = dhft::testkit::generate(cfg);

    std::vector<double> nsPerEvent;
    std::vector<double> p99;
    std::vector<double> p999;
    nsPerEvent.reserve(static_cast<std::size_t>(opt.reps));
    p99.reserve(static_cast<std::size_t>(opt.reps));
    p999.reserve(static_cast<std::size_t>(opt.reps));

    std::uint64_t checksum = 0;
    bool checksumStable = true;
    dhft::bench::BatchResult lastBatch{};

    lastBatch = dhft::bench::run_batch(script, opt.warmup);
    checksum = lastBatch.checksum;

    for (int r = 0; r < opt.reps; ++r) {
        const auto batch = dhft::bench::run_batch(script, opt.warmup);
        if (batch.checksum != checksum) {
            checksumStable = false;
        }
        nsPerEvent.push_back(batch.nsPerEvent);
        lastBatch = batch;
    }

    for (int r = 0; r < opt.reps; ++r) {
        const auto t = dhft::bench::run_tail(script, opt.warmup);
        if (t.checksum != checksum) {
            checksumStable = false;
        }
        p99.push_back(t.all.percentile_ns(0.99));
        p999.push_back(t.all.percentile_ns(0.999));
    }

    const auto tail = dhft::bench::run_tail(script, opt.warmup);

    dhft::bench::Counters counters;
    dhft::bench::CounterSample counted;
    double countedNs = 0.0;
    if (counters.available()) {
        const auto counting = dhft::bench::run_batch(script, opt.warmup, &counters);
        counted = counting.counters;
        countedNs = counting.totalNs;
        if (counting.checksum != checksum) {
            checksumStable = false;
        }
    }

    if (opt.json) {
        std::printf("{\n");
        std::printf("  \"events\": %zu,\n", lastBatch.events);
        std::printf("  \"warmup\": %zu,\n", opt.warmup);
        std::printf("  \"seed\": %llu,\n", static_cast<unsigned long long>(opt.seed));
        std::printf("  \"reps\": %d,\n", opt.reps);
        std::printf("  \"quantity_at_start\": %zu,\n", lastBatch.atStart.quantity);
        std::printf("  \"levels_at_start\": %zu,\n", lastBatch.atStart.levels);
        std::printf("  \"quantity_at_end\": %zu,\n", lastBatch.atEnd.quantity);
        std::printf("  \"levels_at_end\": %zu,\n", lastBatch.atEnd.levels);
        std::printf("  \"ns_per_event_median\": %.2f,\n", median(nsPerEvent));
        std::printf("  \"ns_per_event_min\": %.2f,\n",
                    *std::min_element(nsPerEvent.begin(), nsPerEvent.end()));
        std::printf("  \"ns_per_event_max\": %.2f,\n",
                    *std::max_element(nsPerEvent.begin(), nsPerEvent.end()));
        std::printf("  \"p99_median\": %.1f,\n", median(p99));
        std::printf("  \"p999_median\": %.1f,\n", median(p999));
        std::printf("  \"p99_new\": %.1f,\n", tail.newOrder.percentile_ns(0.99));
        std::printf("  \"p99_cancel\": %.1f,\n", tail.cancel.percentile_ns(0.99));
        std::printf("  \"p99_modify\": %.1f,\n", tail.modify.percentile_ns(0.99));
        std::printf("  \"max_ns\": %.0f,\n", tail.all.max_ns());
        std::printf("  \"over_1us\": %zu,\n", tail.all.count_over_ns(1000.0));
        std::printf("  \"over_10us\": %zu,\n", tail.all.count_over_ns(10000.0));
        std::printf("  \"checksum\": \"%016llx\",\n", static_cast<unsigned long long>(checksum));
        std::printf("  \"checksum_stable\": %s,\n", checksumStable ? "true" : "false");
        std::printf("  \"counters_available\": %s,\n", counters.available() ? "true" : "false");
        std::printf("  \"counters_status\": \"%s\"", counters.status().c_str());
        if (counted.valid) {
            const auto ev = static_cast<double>(lastBatch.events);
            std::printf(",\n  \"cycles_per_event\": %.2f,\n", static_cast<double>(counted.cycles) / ev);
            std::printf("  \"instructions_per_event\": %.2f,\n",
                        static_cast<double>(counted.instructions) / ev);
            std::printf("  \"branch_misses_per_event\": %.4f,\n",
                        static_cast<double>(counted.branchMisses) / ev);
            std::printf("  \"implied_clock_ghz\": %.3f",
                        countedNs == 0.0 ? 0.0 : static_cast<double>(counted.cycles) / countedNs);
        }
        std::printf("\n}\n");
        return checksumStable ? 0 : 1;
    }

    std::printf("script: %zu events (seed %llu), warmup %zu, %d repetitions\n", opt.events,
                static_cast<unsigned long long>(opt.seed), opt.warmup, opt.reps);
    std::printf("book depth: entering %zu qty / %zu levels   leaving %zu qty / %zu levels\n\n",
                lastBatch.atStart.quantity, lastBatch.atStart.levels, lastBatch.atEnd.quantity,
                lastBatch.atEnd.levels);

    std::printf("throughput (batch timing -- the only valid median)\n");
    std::printf("  ns/event   median %7.2f   min %7.2f   max %7.2f   spread %.1f%%\n",
                median(nsPerEvent), *std::min_element(nsPerEvent.begin(), nsPerEvent.end()),
                *std::max_element(nsPerEvent.begin(), nsPerEvent.end()),
                100.0 * (*std::max_element(nsPerEvent.begin(), nsPerEvent.end()) -
                         *std::min_element(nsPerEvent.begin(), nsPerEvent.end())) /
                    median(nsPerEvent));

    std::printf("\ntail (per-event, includes ~41 ns timer overhead)\n");
    std::printf("  p99        median %7.1f ns   min %7.1f   max %7.1f\n", median(p99),
                *std::min_element(p99.begin(), p99.end()),
                *std::max_element(p99.begin(), p99.end()));
    std::printf("  p99.9      median %7.1f ns   min %7.1f   max %7.1f\n", median(p999),
                *std::min_element(p999.begin(), p999.end()),
                *std::max_element(p999.begin(), p999.end()));
    std::printf("  worst      %.0f ns    over 1us: %zu    over 10us: %zu\n", tail.all.max_ns(),
                tail.all.count_over_ns(1000.0), tail.all.count_over_ns(10000.0));

    std::printf("\nper event type (p99 / p99.9 ns)\n");
    std::printf("  new     %8.1f / %8.1f   (%zu samples)\n", tail.newOrder.percentile_ns(0.99),
                tail.newOrder.percentile_ns(0.999), tail.newOrder.count());
    std::printf("  cancel  %8.1f / %8.1f   (%zu samples)\n", tail.cancel.percentile_ns(0.99),
                tail.cancel.percentile_ns(0.999), tail.cancel.count());
    std::printf("  modify  %8.1f / %8.1f   (%zu samples)\n", tail.modify.percentile_ns(0.99),
                tail.modify.percentile_ns(0.999), tail.modify.count());

    std::printf("\nhardware counters: %s\n", counters.status().c_str());
    if (counted.valid) {
        const auto ev = static_cast<double>(lastBatch.events);
        std::printf("  cycles/event        %8.2f\n", static_cast<double>(counted.cycles) / ev);
        std::printf("  instructions/event  %8.2f\n",
                    static_cast<double>(counted.instructions) / ev);
        std::printf("  IPC                 %8.2f\n",
                    counted.cycles == 0 ? 0.0
                                        : static_cast<double>(counted.instructions) /
                                              static_cast<double>(counted.cycles));
        std::printf("  branch misses/event %8.4f\n",
                    static_cast<double>(counted.branchMisses) / ev);
        // Cycles accrue only while the thread runs; wall clock also counts time it did not.
        // An implied clock well under the P-core maximum means the thread was descheduled.
        std::printf("  implied clock       %8.2f GHz  (cycles / wall time; M1 P-core max 3.20)\n",
                    countedNs == 0.0 ? 0.0 : static_cast<double>(counted.cycles) / countedNs);
    }

    std::printf("\nchecksum %016llx  %s\n", static_cast<unsigned long long>(checksum),
                checksumStable ? "(stable)" : "(UNSTABLE -- behaviour changed between runs)");
    return checksumStable ? 0 : 1;
}
