#include <dhft/Script.h>

#include <fstream>
#include <istream>
#include <sstream>
#include <stdexcept>

namespace dhft {

namespace {

Side parse_side(const std::string& tok, int lineNo) {
    if (tok == "BUY" || tok == "B") {
        return Side::Buy;
    }
    if (tok == "SELL" || tok == "S") {
        return Side::Sell;
    }
    throw std::runtime_error("line " + std::to_string(lineNo) + ": bad side '" + tok + "'");
}

// Strip everything from the first '#' onward.
void strip_comment(std::string& line) {
    const auto hash = line.find('#');
    if (hash != std::string::npos) {
        line.erase(hash);
    }
}

} // namespace

std::vector<InEvent> parse_script(std::istream& in) {
    std::vector<InEvent> events;
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        strip_comment(line);

        std::istringstream ls{line};
        std::string op;
        if (!(ls >> op)) {
            continue; // blank or comment-only
        }

        if (op == "N") {
            std::uint64_t id{};
            std::string sideTok;
            std::int64_t px{};
            std::int64_t qty{};
            if (!(ls >> id >> sideTok >> px >> qty)) {
                throw std::runtime_error("line " + std::to_string(lineNo) +
                                         ": expected 'N <id> <BUY|SELL> <price> <qty>'");
            }
            events.push_back(InEvent{EventType::NewOrder, OrderId{id}, parse_side(sideTok, lineNo),
                                     Price{px}, Quantity{qty}});
        } else if (op == "C") {
            std::uint64_t id{};
            if (!(ls >> id)) {
                throw std::runtime_error("line " + std::to_string(lineNo) + ": expected 'C <id>'");
            }
            events.push_back(
                InEvent{EventType::Cancel, OrderId{id}, Side::Buy, Price{0}, Quantity{0}});
        } else if (op == "M") {
            std::uint64_t id{};
            std::int64_t qty{};
            if (!(ls >> id >> qty)) {
                throw std::runtime_error("line " + std::to_string(lineNo) +
                                         ": expected 'M <id> <qty>'");
            }
            events.push_back(
                InEvent{EventType::Modify, OrderId{id}, Side::Buy, Price{0}, Quantity{qty}});
        } else {
            throw std::runtime_error("line " + std::to_string(lineNo) + ": unknown op '" + op + "'");
        }
    }

    return events;
}

std::vector<InEvent> parse_script_file(const std::string& path) {
    std::ifstream in{path};
    if (!in) {
        throw std::runtime_error("cannot open script file: " + path);
    }
    return parse_script(in);
}

} // namespace dhft
