#pragma once

#include <dhft/Types.h>

#include <cstdint>


namespace dhft {

    enum class EventType    : std::uint8_t { NewOrder, Cancel, Modify };
    enum class OutKind      : std::uint8_t { Trade, Ack, Reject };
    enum class RejectReason : std::uint8_t { None, UnknownOrder, BadQuantity };

    struct InEvent { EventType type{}; OrderId id{}; Side side{}; Price price{}; Quantity qty{}; };
    struct OutEvent { OutKind kind{}; OrderId id{}; OrderId resting{}; Price price{};
                       Quantity qty{}; RejectReason reason{RejectReason::None}; };

}
