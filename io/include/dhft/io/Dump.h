#pragma once

#include <dhft/OrderBook.h>

#include <cstddef>
#include <iosfwd>

namespace dhft::io {

void dump_book(const OrderBook& book, std::ostream& out, std::size_t levels = 5);

} // namespace dhft::io
