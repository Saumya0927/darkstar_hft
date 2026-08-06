#pragma once

#include <dhft/OrderBook.h>

#include <iosfwd>

namespace dhft::io {

void dump_book(const OrderBook& book, std::ostream& out, int levels = 5);

} // namespace dhft::io
