#pragma once

#include <dhft/Types.h>

#include <cstdint>


namespace dhft {

    enum class EventType    : std::uint8_t { NewOrder, Cancel, Modify };
    enum class OutKind      : std::uint8_t { Trade, Ack, Reject };
    enum class RejectReason : std::uint8_t { None, UnknownOrder, BadQuantity, DuplicateOrderId };

    struct InEvent {
        EventType type{}; OrderId id{}; Side side{}; Price price{}; Quantity qty{};

        [[nodiscard]] static constexpr InEvent new_order(OrderId id, Side side, Price price,
                                                         Quantity qty) noexcept {
            return InEvent{EventType::NewOrder, id, side, price, qty};
        }
        [[nodiscard]] static constexpr InEvent cancel(OrderId id) noexcept {
            return InEvent{EventType::Cancel, id, Side::Buy, Price{}, Quantity{}};
        }
        [[nodiscard]] static constexpr InEvent modify(OrderId id, Quantity qty) noexcept {
            return InEvent{EventType::Modify, id, Side::Buy, Price{}, qty};
        }
    };

    struct OutEvent {
        OutKind kind{}; OrderId id{}; OrderId resting{}; Price price{};
        Quantity qty{}; RejectReason reason{RejectReason::None};

        [[nodiscard]] constexpr bool operator==(const OutEvent&) const = default;

        [[nodiscard]] static constexpr OutEvent trade(OrderId aggressor, OrderId resting,
                                                      Price price, Quantity qty) noexcept {
            return OutEvent{OutKind::Trade, aggressor, resting, price, qty, RejectReason::None};
        }
        [[nodiscard]] static constexpr OutEvent ack(OrderId id) noexcept {
            return OutEvent{OutKind::Ack, id, OrderId{}, Price{}, Quantity{}, RejectReason::None};
        }
        [[nodiscard]] static constexpr OutEvent reject(OrderId id, RejectReason reason) noexcept {
            return OutEvent{OutKind::Reject, id, OrderId{}, Price{}, Quantity{}, reason};
        }
    };

}
