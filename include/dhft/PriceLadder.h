#pragma once

#include <dhft/Types.h>
#include <dhft/Check.h>

#include <bit>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <type_traits>
#include <vector>


namespace dhft {

    template <Side Sd, typename LevelT>
    class PriceLadder {
    public:
        explicit PriceLadder(Price min, Price max)
          : min_{min},
            max_{max},
            levels_(static_cast<std::size_t>(max.ticks - min.ticks + 1)),
            occupied_((levels_.size() + 63) / 64, 0) {}

        [[nodiscard]] LevelT* find(Price p) noexcept {
            if (in_band(p)) {
                const std::size_t i = idx(p);
                return test(i) ? &levels_[i] : nullptr;
            }
            const auto it = tail_.find(p);
            return it == tail_.end() ? nullptr : &it->second;
        }

        void erase(Price p) noexcept {
            if (in_band(p)) {
                const std::size_t i = idx(p);
                clear(i);
                if (i == bestIdx_) {
                    bestIdx_ = rescan_best();
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

    private:
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

        [[nodiscard]] std::size_t rescan_best() const noexcept {
            if constexpr (Sd == Side::Buy) {
                for (std::size_t w = occupied_.size(); w-- > 0; ) {
                    if (occupied_[w] != 0) {
                        return w * 64 + (63 - static_cast<std::size_t>(std::countl_zero(occupied_[w])));
                    }
                }
            } else {
                for (std::size_t w = 0; w < occupied_.size(); ++w) {
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
