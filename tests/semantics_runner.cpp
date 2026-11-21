#include "veloz.hpp"

#include <iostream>
#include <sstream>

// Test-only framing lets the Python oracle compare many independent programs
// without paying the process startup cost for each. 0x1e separates records.
int main() {
    std::string source;
    while (std::getline(std::cin, source, '\x1e')) {
        try {
            auto program = veloz::parse(source);
            veloz::check(program);
            std::ostringstream output;
            constexpr veloz::Limits limits{200'000, 2'000};
            veloz::runTests(program, output, limits);
            veloz::runMain(program, output, limits);
            std::cout << "OK " << output.str() << '\x1e';
        } catch (const veloz::Diagnostic& error) {
            std::cout << error.id << '\x1e';
        }
    }
}
