// Runs an order script through the matching engine and prints the results.
//   usage: ./demo [script-file]      (default: data/scripts/simple.txt)
#include <dhft/Feed.h>
#include <dhft/MatchingEngine.h>
#include <dhft/Script.h>
#include <dhft/Sink.h>

#include <exception>
#include <iostream>
#include <string>

namespace {

void print_book(const dhft::OrderBook& book) {
    std::cout << "\n--- final book ---\n";

    const auto asks = book.depth(dhft::Side::Sell, 5);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) { // worst ask on top
        std::cout << "  ask " << it->first.ticks << "  x" << it->second.v << '\n';
    }

    if (book.best_bid() && book.best_ask()) {
        std::cout << "  --- spread " << (book.best_ask()->ticks - book.best_bid()->ticks)
                  << " ---\n";
    } else {
        std::cout << "  ---\n";
    }

    for (const auto& [price, qty] : book.depth(dhft::Side::Buy, 5)) { // best bid first
        std::cout << "  bid " << price.ticks << "  x" << qty.v << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string path = (argc > 1) ? argv[1] : "data/scripts/simple.txt";

        dhft::ScriptedFeed feed{dhft::parse_script_file(path)};
        dhft::PrintingSink sink{std::cout};
        dhft::MatchingEngine engine{sink};

        std::cout << "--- events ---\n";
        dhft::InEvent e{};
        while (feed.next(e)) {
            engine.process(e);
        }

        print_book(engine.book());
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
