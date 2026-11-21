#include "commands.hpp"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <sstream>
#ifndef VELOZ_VERSION
#define VELOZ_VERSION "dev"
#endif
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace veloz {
std::string jsonString(std::string_view input) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : input) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
namespace {
constexpr auto help = R"(veloz check ARCHIVO [--format]
veloz run ARCHIVO [--steps N]
veloz build ARCHIVO -o EJECUTABLE [--release] [--cpu x86-64-v3|native]
veloz emit-ir ARCHIVO
veloz sig ARCHIVO [SÍMBOLO] [--offset N]
veloz types ARCHIVO [FUNCIÓN] [--offset N]
veloz costes ARCHIVO [FUNCIÓN] [--offset N]
veloz fmt ARCHIVO [--check]
veloz patch ARCHIVO FUNCIÓN HASH REEMPLAZO
veloz explain ID
)";
std::string spanJson(Span span, std::string_view source) {
    auto pos = std::min(span.start, source.size());
    auto line = 1 + std::count(source.begin(), source.begin() + pos, '\n');
    auto last = source.substr(0, pos).rfind('\n');
    auto column = pos - (last == source.npos ? 0 : last + 1) + 1;
    return "{\"start\":" + std::to_string(span.start) + ",\"end\":" + std::to_string(span.end)
        + ",\"line\":" + std::to_string(line) + ",\"column\":" + std::to_string(column) + "}";
}
void report(const Diagnostic& error, std::string_view file, std::string_view source) {
    std::cerr << "{\"id\":" << jsonString(error.id) << ",\"message\":" << jsonString(error.message)
              << ",\"file\":" << jsonString(file) << ",\"span\":" << spanJson(error.span, source)
              << ",\"expected\":" << jsonString(error.expected) << ",\"actual\":" << jsonString(error.actual)
              << ",\"related\":[";
    for (size_t i = 0; i < error.related.size(); ++i) {
        if (i) std::cerr << ',';
        std::cerr << spanJson(error.related[i], source);
    }
    std::cerr << "],\"fix\":";
    if (error.fix) std::cerr << "{\"span\":" << spanJson(error.fix->span, source)
                             << ",\"replacement\":" << jsonString(error.fix->replacement) << "}";
    else std::cerr << "null";
    std::cerr << ",\"example\":" << jsonString(error.example) << "}\n";
}
void usage() { throw Diagnostic("E_USAGE", help, {}); }
// check executes tests silently, even when a test calls print many times.
struct Discard : std::streambuf {
    int_type overflow(int_type c) override { return traits_type::not_eof(c); }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};
int dispatch(const std::vector<std::string>& args) {
    std::string source, filename;
    try {
        if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) { std::cout << help; return 0; }
        if (args.size() == 1 && args[0] == "--version") { std::cout << "veloz " << VELOZ_VERSION << '\n'; return 0; }
        if (args.size() < 2) usage();
        const auto& cmd = args[0];
        if (cmd == "explain") {
            if (args.size() != 2) usage();
            std::cout << explain(args[1]) << '\n';
            return 0;
        }
        filename = args[1];
        auto file = pathFrom(filename);
        source = readFile(file);
        auto program = parse(source);
        if (cmd == "fmt" || cmd == "patch" || cmd == "sig" || cmd == "types" || cmd == "costes") {
            std::cout << agentCommand(args, file, source, program);
            return 0;
        }
        check(program);
        if (cmd == "check" && (args.size() == 2 || (args.size() == 3 && args[2] == "--format"))) {
            if (args.size() == 3 && formatSource(source) != source)
                throw Diagnostic("E_FORMAT", "Ejecuta veloz fmt para normalizar el formato", {});
            Discard discard;
            std::ostream quiet(&discard);
            runTests(program, quiet);
        } else if (cmd == "run") {
            Limits limits;
            if (args.size() != 2) {
                if (args.size() != 4 || args[2] != "--steps") usage();
                const auto& value = args[3];
                auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), limits.steps);
                if (ec != std::errc{} || end != value.data()+value.size() || !limits.steps) usage();
            }
            runMain(program, std::cout, limits, &std::cin);
        } else if (cmd == "emit-ir" && args.size() == 2) {
            std::cout << emitLLVM(program);
        } else if (cmd == "build" && args.size() >= 4 && args[2] == "-o") {
            BuildOptions options;
            for (std::size_t i = 4; i < args.size(); ++i) {
                if (args[i] == "--release" && !options.release) options.release = true;
                else if (args[i] == "--cpu" && options.cpu.empty() && i + 1 < args.size()) {
                    options.cpu = args[++i];
                    if (options.cpu != "x86-64-v3" && options.cpu != "native") usage();
                } else usage();
            }
            auto out = pathFrom(args[3]);
            if ((fs::exists(out) && fs::equivalent(file, out)) || fs::weakly_canonical(out) == fs::weakly_canonical(file))
                throw Diagnostic("E_OUTPUT", "La salida sobrescribiría el archivo fuente", {});
            buildBinary(program, out, options);
        } else usage();
        return 0;
    } catch (const Diagnostic& d) {
        report(d, filename, source);
    } catch (const std::exception& e) {
        report(Diagnostic("E_HOST", e.what(), {}), filename, source);
    }
    return 1;
}
}
}
int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    (void)argv;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    auto wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) return 1;
    for (int i = 1; i < argc; ++i) args.push_back(veloz::pathText(std::filesystem::path(wide[i])));
    LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return veloz::dispatch(args);
}
