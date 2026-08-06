#include <dhft/io/Dump.h>

#include <ostream>
#include <utility>
#include <vector>

namespace dhft::io {

void dump_book(const OrderBook& book, std::ostream& out, std::size_t levels) {
    std::vector<std::pair<Price, Quantity>> rows;

    book.depth_into(Side::Sell, levels, rows);
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        out << "  ask " << it->first.ticks << " x" << it->second.v << '\n';
    }

    if (book.best_bid() && book.best_ask()) {
        out << "  --- spread " << (book.best_ask()->ticks - book.best_bid()->ticks) << " ---\n";
    } else {
        out << "  ---\n";
    }

    book.depth_into(Side::Buy, levels, rows);
    for (const auto& [price, qty] : rows) {
        out << "  bid " << price.ticks << " x" << qty.v << '\n';
    }
}

} // namespace dhft::io
