// Runs an order script through the matching engine and prints the results.
//   usage: ./demo [script-file]      (default: data/scripts/simple.txt)
#include <dhft/Feed.h>
#include <dhft/MatchingEngine.h>
#include <dhft/io/Dump.h>
#include <dhft/io/Script.h>
#include <dhft/io/TextSink.h>

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    try {
        const std::string path = (argc > 1) ? argv[1] : "data/scripts/simple.txt";

        dhft::ScriptedFeed feed{dhft::io::parse_script_file(path)};
        dhft::io::TextSink sink{std::cout};
        dhft::MatchingEngine engine{sink};

        std::cout << "--- events ---\n";
        dhft::InEvent e{};
        while (feed.next(e)) {
            engine.process(e);
        }

        std::cout << "\n--- final book ---\n";
        dhft::io::dump_book(engine.book(), std::cout);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
