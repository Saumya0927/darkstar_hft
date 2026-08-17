#pragma once

#include <dhft/Types.h>
#include <dhft/Check.h>

#include <bit>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>


namespace dhft {

    template <Side Sd, typename LevelT>
    class PriceLadder {
    public:
        explicit PriceLadder(Price min, Price max)
          : min_{min},
            max_{max},
            levels_(span(min, max)),
            occupied_((levels_.size() + 63) / 64, 0) {}

        template <typename Self>
        [[nodiscard]] auto* find(this Self&& self, Price p) noexcept {
            if (self.in_band(p)) {
                const std::size_t i = self.idx(p);
                return self.test(i) ? &self.levels_[i] : nullptr;
            }
            const auto it = self.tail_.find(p);
            return it == self.tail_.end() ? nullptr : &it->second;
        }

        void erase(Price p) noexcept {
            if (in_band(p)) {
                const std::size_t i = idx(p);
                clear(i);
                if (i == bestIdx_) {
                    bestIdx_ = rescan_best(i / 64);
                }
                return;
            }
            tail_.erase(p);
        }

        [[nodiscard]] LevelT& insert(Price p) {
            if (in_band(p)) {
                const std::size_t i = idx(p);
                if (!test(i)) {
                    levels_[i] = LevelT{};
                    set(i);
                    if (bestIdx_ == kNone || is_better(i, bestIdx_)) {
                        bestIdx_ = i;
                    }
                }
                return levels_[i];
            }
            return tail_[p];
        }

        [[nodiscard]] std::optional<Price> best() const noexcept {
            const bool haveArray = bestIdx_ != kNone;
            const bool haveTail = !tail_.empty();

            if (!haveArray && !haveTail) { return std::nullopt; }
            if (!haveTail) { return price_at(bestIdx_); }
            if (!haveArray) { return tail_.begin()->first; }

            const Price fromArray = price_at(bestIdx_);
            const Price fromTail = tail_.begin()->first;
            return is_better(fromArray, fromTail) ? fromArray : fromTail;
        }

        template <typename F>
        void for_each(F&& f) const {
            for (std::size_t w = 0; w < occupied_.size(); ++w) {
                std::uint64_t word = occupied_[w];
                while (word != 0) {
                    const std::size_t i = w * 64 + static_cast<std::size_t>(std::countr_zero(word));
                    f(price_at(i), levels_[i]);
                    word &= word - 1;
                }
            }
            for (const auto& [price, level] : tail_) {
                f(price, level);
            }
        }

        [[nodiscard]] std::expected<void, std::string> validate() const {
            // A set padding bit would make for_each and rescan_best index past levels_.
            // The used != 0 guard is load-bearing: ~0ULL << 0 is all ones, which would
            // flag every legitimate bit in a band whose span is a multiple of 64.
            const std::size_t used = levels_.size() % 64;
            if (used != 0) {
                const std::uint64_t mask = ~0ULL << used;
                if ((occupied_.back() & mask) != 0) {
                    return std::unexpected("occupancy bit set past the end of the ladder");
                }
            }

            // Recomputed the slow way on purpose: rescan_best is the thing under test.
            std::size_t truth = kNone;
            for (std::size_t w = 0; w < occupied_.size(); ++w) {
                std::uint64_t word = occupied_[w];
                while (word != 0) {
                    const std::size_t i = w * 64 + static_cast<std::size_t>(std::countr_zero(word));
                    if (truth == kNone || is_better(i, truth)) {
                        truth = i;
                    }
                    word &= word - 1;
                }
            }

            if (bestIdx_ != truth) {
                const auto name = [](std::size_t i) {
                    return i == kNone ? std::string{"none"} : std::to_string(i);
                };
                return std::unexpected("cached best index is " + name(bestIdx_) +
                                       " but the true best is " + name(truth));
            }

            for (const auto& [price, level] : tail_) {
                if (in_band(price)) {
                    return std::unexpected("in-band price " + std::to_string(price.ticks) +
                                           " is in the tail map");
                }
            }

            return {};
        }

    private:
        static constexpr std::size_t kMaxSpan = std::size_t{1} << 24;

        [[nodiscard]] static std::size_t span(Price min, Price max) {
            DHFT_CHECK_MSG(min.ticks <= max.ticks, "price band is inverted");
            const std::uint64_t width = static_cast<std::uint64_t>(max.ticks) -
                                        static_cast<std::uint64_t>(min.ticks);
            DHFT_CHECK_MSG(width < kMaxSpan, "price band is implausibly wide");
            return static_cast<std::size_t>(width) + 1;
        }

        [[nodiscard]] Price price_at(std::size_t i) const noexcept {
            return Price{min_.ticks + static_cast<std::int64_t>(i)};
        }

        [[nodiscard]] static bool is_better(std::size_t a, std::size_t b) noexcept {
            if constexpr (Sd == Side::Buy) {
                return a > b;
            } else {
                return a < b;
            }
        }

        [[nodiscard]] static bool is_better(Price a, Price b) noexcept {
            if constexpr (Sd == Side::Buy) {
                return a.ticks > b.ticks;
            } else {
                return a.ticks < b.ticks;
            }
        }

        [[nodiscard]] std::size_t rescan_best(std::size_t startWord) const noexcept {
            if constexpr (Sd == Side::Buy) {
                for (std::size_t w = startWord + 1; w-- > 0; ) {
                    if (occupied_[w] != 0) {
                        return w * 64 + (63 - static_cast<std::size_t>(std::countl_zero(occupied_[w])));
                    }
                }
            } else {
                for (std::size_t w = startWord; w < occupied_.size(); ++w) {
                    if (occupied_[w] != 0) {
                        return w * 64 + static_cast<std::size_t>(std::countr_zero(occupied_[w]));
                    }
                }
            }
            return kNone;
        }

        [[nodiscard]] bool in_band(Price p) const noexcept {
            return p.ticks >= min_.ticks && p.ticks <= max_.ticks;
        }

        [[nodiscard]] std::size_t idx(Price p) const noexcept {
            DHFT_DCHECK(in_band(p));
            return static_cast<std::size_t>(p.ticks - min_.ticks);
        }

        using TailCmp = std::conditional_t<Sd == Side::Buy, std::greater<>, std::less<>>;

        Price min_;
        Price max_;
        std::vector<LevelT> levels_;
        std::vector<std::uint64_t> occupied_;
        static constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
        std::size_t bestIdx_{kNone};
        std::map<Price, LevelT, TailCmp> tail_;

        void set(std::size_t i) noexcept { occupied_[i / 64] |= 1ULL << (i % 64); }
        void clear(std::size_t i) noexcept { occupied_[i / 64] &= ~(1ULL << (i % 64)); }
        [[nodiscard]] bool test(std::size_t i) const noexcept {
            return ((occupied_[i / 64] >> (i % 64)) & 1ULL) != 0;
        }
    };
}
