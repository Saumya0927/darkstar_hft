#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dhft::bench {

struct CounterSample {
    std::uint64_t cycles{};
    std::uint64_t instructions{};
    std::uint64_t branches{};
    std::uint64_t branchMisses{};
    std::uint64_t l1dMissLd{};
    std::uint64_t l1dMissSt{};
    bool valid{false};
};

class Counters {
public:
    Counters();
    ~Counters();
    Counters(const Counters&) = delete;
    Counters& operator=(const Counters&) = delete;
    Counters(Counters&&) = delete;
    Counters& operator=(Counters&&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] const std::string& status() const noexcept;
    [[nodiscard]] const std::vector<std::string>& resolved_events() const noexcept;

    bool begin() noexcept;
    [[nodiscard]] CounterSample end() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dhft::bench
