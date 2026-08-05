#include <dhft/Sink.h>

#include <ostream>

namespace dhft {

namespace {

const char* reason_text(RejectReason r) {
    switch (r) {
    case RejectReason::None:
        return "NONE";
    case RejectReason::UnknownOrder:
        return "UNKNOWN_ORDER";
    case RejectReason::BadQuantity:
        return "BAD_QUANTITY";
    }
    return "?";
}

} // namespace

void PrintingSink::on_event(const OutEvent& e) {
    switch (e.kind) {
    case OutKind::Trade:
        out_ << "TRADE  aggressor=" << e.id.v << " resting=" << e.resting.v
             << " price=" << e.price.ticks << " qty=" << e.qty.v << '\n';
        break;
    case OutKind::Ack:
        out_ << "ACK    id=" << e.id.v << '\n';
        break;
    case OutKind::Reject:
        out_ << "REJECT id=" << e.id.v << " reason=" << reason_text(e.reason) << '\n';
        break;
    }
}

} // namespace dhft
