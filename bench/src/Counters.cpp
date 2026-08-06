#include <dhft/bench/Counters.h>

#include <dlfcn.h>
#include <unistd.h>

#include <array>
#include <cstring>

namespace dhft::bench {

namespace {

constexpr std::uint32_t kClassFixed = 1U << 0;
constexpr std::uint32_t kClassConfigurable = 1U << 1;
constexpr std::size_t kMaxCounters = 32;

using kpc_config_t = std::uint64_t;

struct KperfApi {
    int (*kpc_get_counter_count)(std::uint32_t);
    int (*kpc_set_config)(std::uint32_t, kpc_config_t*);
    int (*kpc_set_counting)(std::uint32_t);
    int (*kpc_set_thread_counting)(std::uint32_t);
    int (*kpc_get_thread_counters)(std::uint32_t, std::uint32_t, std::uint64_t*);
    int (*kpc_force_all_ctrs_set)(int);
};

struct KperfDataApi {
    int (*kpep_db_create)(const char*, void**);
    void (*kpep_db_free)(void*);
    int (*kpep_db_event)(void*, const char*, void**);
    int (*kpep_config_create)(void*, void**);
    void (*kpep_config_free)(void*);
    int (*kpep_config_add_event)(void*, void**, std::uint32_t, std::uint32_t*);
    int (*kpep_config_force_counters)(void*);
    int (*kpep_config_kpc)(void*, kpc_config_t*, std::size_t);
    int (*kpep_config_kpc_count)(void*, std::size_t*);
    int (*kpep_config_kpc_classes)(void*, std::uint32_t*);
    int (*kpep_config_kpc_map)(void*, std::size_t*, std::size_t);
};

// Event names differ per chip generation, so each metric carries fallbacks.
struct Alias {
    const char* label;
    std::array<const char*, 4> names;
};

constexpr std::array<Alias, 4> kAliases{{
    {"cycles", {"FIXED_CYCLES", "CPU_CLK_UNHALTED.THREAD", "CPU_CLK_UNHALTED.CORE", nullptr}},
    {"instructions", {"FIXED_INSTRUCTIONS", "INST_ALL", "INST_RETIRED.ANY", nullptr}},
    {"branches", {"INST_BRANCH", "BR_INST_RETIRED.ALL_BRANCHES", nullptr, nullptr}},
    {"branch-misses",
     {"BRANCH_MISPRED_NONSPEC", "BRANCH_MISPREDICT", "BR_MISP_RETIRED.ALL_BRANCHES", nullptr}},
}};

// kpc_force_all_ctrs_set(1) claims the PMU process-wide; two live readers would fight.
bool& pmu_claimed() {
    static bool claimed = false;
    return claimed;
}

template <typename Fn> bool load(void* handle, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(handle, name));
    return out != nullptr;
}

} // namespace

struct Counters::Impl {
    void* kperf{nullptr};
    void* kperfdata{nullptr};
    KperfApi k{};
    KperfDataApi d{};
    void* db{nullptr};
    void* cfg{nullptr};

    bool ready{false};
    std::string status{"not initialised"};
    std::vector<std::string> events;

    std::uint32_t classes{0};
    bool claimedPmu{false};
    std::array<std::size_t, kMaxCounters> map{};
    std::array<std::uint64_t, kMaxCounters> before{};

    ~Impl() {
        if (claimedPmu && k.kpc_force_all_ctrs_set != nullptr) {
            k.kpc_force_all_ctrs_set(0);
            pmu_claimed() = false;
        }
        if (cfg != nullptr && d.kpep_config_free != nullptr) {
            d.kpep_config_free(cfg);
        }
        if (db != nullptr && d.kpep_db_free != nullptr) {
            d.kpep_db_free(db);
        }
        if (kperfdata != nullptr) {
            dlclose(kperfdata);
        }
        if (kperf != nullptr) {
            dlclose(kperf);
        }
    }

    bool load_frameworks() {
        kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
        if (kperf == nullptr) {
            status = "kperf.framework not present";
            return false;
        }
        kperfdata =
            dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_LAZY);
        if (kperfdata == nullptr) {
            status = "kperfdata.framework not present";
            return false;
        }

        const bool ok =
            load(kperf, "kpc_get_counter_count", k.kpc_get_counter_count) &&
            load(kperf, "kpc_set_config", k.kpc_set_config) &&
            load(kperf, "kpc_set_counting", k.kpc_set_counting) &&
            load(kperf, "kpc_set_thread_counting", k.kpc_set_thread_counting) &&
            load(kperf, "kpc_get_thread_counters", k.kpc_get_thread_counters) &&
            load(kperf, "kpc_force_all_ctrs_set", k.kpc_force_all_ctrs_set) &&
            load(kperfdata, "kpep_db_create", d.kpep_db_create) &&
            load(kperfdata, "kpep_db_free", d.kpep_db_free) &&
            load(kperfdata, "kpep_db_event", d.kpep_db_event) &&
            load(kperfdata, "kpep_config_create", d.kpep_config_create) &&
            load(kperfdata, "kpep_config_free", d.kpep_config_free) &&
            load(kperfdata, "kpep_config_add_event", d.kpep_config_add_event) &&
            load(kperfdata, "kpep_config_force_counters", d.kpep_config_force_counters) &&
            load(kperfdata, "kpep_config_kpc", d.kpep_config_kpc) &&
            load(kperfdata, "kpep_config_kpc_count", d.kpep_config_kpc_count) &&
            load(kperfdata, "kpep_config_kpc_classes", d.kpep_config_kpc_classes) &&
            load(kperfdata, "kpep_config_kpc_map", d.kpep_config_kpc_map);
        if (!ok) {
            status = "kperf symbols missing (framework layout changed?)";
        }
        return ok;
    }

    bool build_config() {
        if (d.kpep_db_create(nullptr, &db) != 0 || db == nullptr) {
            status = "no PMU event database for this CPU";
            return false;
        }
        if (d.kpep_config_create(db, &cfg) != 0 || cfg == nullptr) {
            status = "kpep_config_create failed";
            return false;
        }
        d.kpep_config_force_counters(cfg);

        for (const auto& alias : kAliases) {
            for (const char* name : alias.names) {
                if (name == nullptr) {
                    break;
                }
                void* ev = nullptr;
                if (d.kpep_db_event(db, name, &ev) == 0 && ev != nullptr &&
                    d.kpep_config_add_event(cfg, &ev, 0, nullptr) == 0) {
                    events.emplace_back(alias.label);
                    break;
                }
            }
        }
        if (events.empty()) {
            status = "no known PMU events resolved for this CPU";
            return false;
        }

        std::size_t configured = 0;
        if (d.kpep_config_kpc_classes(cfg, &classes) != 0 ||
            d.kpep_config_kpc_count(cfg, &configured) != 0 || configured > kMaxCounters ||
            d.kpep_config_kpc_map(cfg, map.data(), sizeof(std::size_t) * kMaxCounters) != 0) {
            status = "kpep counter mapping failed";
            return false;
        }
        return true;
    }

    bool arm() {
        std::array<kpc_config_t, kMaxCounters> regs{};
        if (d.kpep_config_kpc(cfg, regs.data(), sizeof(kpc_config_t) * kMaxCounters) != 0) {
            status = "kpep_config_kpc failed";
            return false;
        }
        // Every step below needs root; this is where a normal user is rejected.
        if (pmu_claimed()) {
            status = "PMU already claimed by another Counters instance";
            return false;
        }
        if (k.kpc_force_all_ctrs_set(1) != 0) {
            status = "PMU access denied (run as root)";
            return false;
        }
        claimedPmu = true;
        pmu_claimed() = true;
        if ((classes & kClassConfigurable) != 0 && k.kpc_set_config(classes, regs.data()) != 0) {
            status = "kpc_set_config failed";
            return false;
        }
        if (k.kpc_set_counting(classes) != 0 || k.kpc_set_thread_counting(classes) != 0) {
            status = "kpc_set_counting failed";
            return false;
        }
        return true;
    }

    bool read(std::array<std::uint64_t, kMaxCounters>& out) const {
        const int total = k.kpc_get_counter_count(kClassFixed | kClassConfigurable);
        if (total <= 0 || static_cast<std::size_t>(total) > kMaxCounters) {
            return false;
        }
        return k.kpc_get_thread_counters(0, static_cast<std::uint32_t>(total), out.data()) == 0;
    }
};

Counters::Counters() : impl_{std::make_unique<Impl>()} {
    // Framework loading and event resolution work unprivileged; only arming needs root.
    // Doing them first turns "needs root" into a much more specific diagnostic.
    if (!impl_->load_frameworks() || !impl_->build_config()) {
        return;
    }
    if (geteuid() != 0) {
        impl_->status = "resolved " + std::to_string(impl_->events.size()) +
                        " PMU events but arming needs root (re-run under sudo); timings only";
        return;
    }
    if (!impl_->arm()) {
        return;
    }
    impl_->ready = true;
    impl_->status = "ok";
}

Counters::~Counters() = default;

bool Counters::available() const noexcept { return impl_->ready; }
const std::string& Counters::status() const noexcept { return impl_->status; }
const std::vector<std::string>& Counters::resolved_events() const noexcept {
    return impl_->events;
}

bool Counters::begin() noexcept {
    if (!impl_->ready) {
        return false;
    }
    return impl_->read(impl_->before);
}

CounterSample Counters::end() noexcept {
    CounterSample s;
    if (!impl_->ready) {
        return s;
    }
    std::array<std::uint64_t, kMaxCounters> after{};
    if (!impl_->read(after)) {
        return s;
    }

    for (std::size_t i = 0; i < impl_->events.size(); ++i) {
        const std::size_t slot = impl_->map[i];
        if (slot >= kMaxCounters) {
            continue;
        }
        const std::uint64_t delta = after[slot] - impl_->before[slot];
        const std::string& label = impl_->events[i];
        if (label == "cycles") {
            s.cycles = delta;
        } else if (label == "instructions") {
            s.instructions = delta;
        } else if (label == "branches") {
            s.branches = delta;
        } else if (label == "branch-misses") {
            s.branchMisses = delta;
        }
    }
    s.valid = true;
    return s;
}

} // namespace dhft::bench
