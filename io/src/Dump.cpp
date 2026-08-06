#include <dhft/io/Dump.h>

#include <ostream>

namespace dhft::io {

void dump_book(const OrderBook& book, std::ostream& out, int levels) {
    const auto asks = book.depth(Side::Sell, levels);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        out << "  ask " << it->first.ticks << " x" << it->second.v << '\n';
    }

    if (book.best_bid() && book.best_ask()) {
        out << "  --- spread " << (book.best_ask()->ticks - book.best_bid()->ticks) << " ---\n";
    } else {
        out << "  ---\n";
    }

    for (const auto& [price, qty] : book.depth(Side::Buy, levels)) {
        out << "  bid " << price.ticks << " x" << qty.v << '\n';
    }
}

} // namespace dhft::io
