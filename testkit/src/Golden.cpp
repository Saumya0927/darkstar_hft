#include <dhft/testkit/Golden.h>

#include <dhft/MatchingEngine.h>
#include <dhft/io/Dump.h>
#include <dhft/io/Script.h>
#include <dhft/io/TextSink.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dhft::testkit {

namespace {

bool updating() { return std::getenv("DHFT_UPDATE_GOLDEN") != nullptr; }

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream in{s};
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string first_difference(const std::string& expected, const std::string& actual) {
    const auto e = split_lines(expected);
    const auto a = split_lines(actual);
    const std::size_t n = std::min(e.size(), a.size());

    for (std::size_t i = 0; i < n; ++i) {
        if (e[i] != a[i]) {
            return "line " + std::to_string(i + 1) + "\n  expected: " + e[i] +
                   "\n  actual:   " + a[i];
        }
    }
    if (e.size() != a.size()) {
        return "line count differs: expected " + std::to_string(e.size()) + ", actual " +
               std::to_string(a.size());
    }
    return "trailing whitespace differs";
}

} // namespace

std::string run_to_text(const std::vector<InEvent>& events) {
    std::ostringstream out;
    io::TextSink sink{out};
    MatchingEngine engine{sink};

    out << "--- events ---\n";
    for (const auto& e : events) {
        engine.process(e);
    }

    out << "--- book ---\n";
    io::dump_book(engine.book(), out);
    return out.str();
}

GoldenOutcome check_golden(const std::string& dir, const std::string& name) {
    const std::filesystem::path base{dir};
    const auto scriptPath = base / (name + ".script");
    const auto expectedPath = base / (name + ".expected");

    if (!std::filesystem::exists(scriptPath)) {
        return {false, "missing script: " + scriptPath.string()};
    }

    std::string actual;
    try {
        actual = run_to_text(io::parse_script_file(scriptPath.string()));
    } catch (const std::exception& ex) {
        return {false, std::string{"script failed to run: "} + ex.what()};
    }

    if (updating()) {
        std::ofstream out{expectedPath, std::ios::binary};
        out << actual;
        return {true, "updated " + expectedPath.string()};
    }

    if (!std::filesystem::exists(expectedPath)) {
        return {false, "missing golden: " + expectedPath.string() +
                           " (run with DHFT_UPDATE_GOLDEN=1 to create it)"};
    }

    const std::string expected = read_file(expectedPath);
    if (expected == actual) {
        return {true, {}};
    }
    return {false, "golden mismatch in " + name + " at " + first_difference(expected, actual)};
}

std::vector<std::string> golden_names(const std::string& dir) {
    std::vector<std::string> names;
    if (!std::filesystem::exists(dir)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.path().extension() == ".script") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace dhft::testkit
