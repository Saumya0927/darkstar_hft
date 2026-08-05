#pragma once

#include <dhft/Events.h>

#include <iosfwd>
#include <string>
#include <vector>

namespace dhft::io {

// Text order-script format, one event per line. Blank lines and '#' comments ignored.
//   N <id> <BUY|SELL> <price> <qty>   new order
//   C <id>                            cancel
//   M <id> <qty>                      modify quantity
[[nodiscard]] std::vector<InEvent> parse_script(std::istream& in);
[[nodiscard]] std::vector<InEvent> parse_script_file(const std::string& path);

} // namespace dhft::io
