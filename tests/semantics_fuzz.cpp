#include "veloz.hpp"

#include <ostream>
#include <streambuf>

namespace {
class DiscardOutput : public std::streambuf {
    int_type overflow(int_type c) override { return traits_type::not_eof(c); }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};
}

// Build with Clang's -fsanitize=fuzzer,address,undefined. Diagnostics are a
// defined outcome for random source. Unexpected C++ exceptions propagate.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* bytes, std::size_t size) {
    const auto source = std::string_view(reinterpret_cast<const char*>(bytes), size);
    try {
        auto program = veloz::parse(source);
        veloz::check(program);
        DiscardOutput buffer;
        std::ostream output(&buffer);
        constexpr veloz::Limits budget{2'000, 32};
        veloz::runTests(program, output, budget);
        veloz::runMain(program, output, budget);
    } catch (const veloz::Diagnostic&) {
        // The front end and VM reject invalid and over-budget inputs.
    }
    return 0;
}
