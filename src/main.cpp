#include "commands.hpp"
#include "project.hpp"
#include "tooling.hpp"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <optional>
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
constexpr auto help = R"(veloz check ARCHIVO [--format] [--max-level N]
veloz run ARCHIVO [--steps N]
veloz build ARCHIVO -o EJECUTABLE [--release] [--web] [--gpu] [--cpu x86-64-v2|x86-64-v3|x86-64-v4|znver4|native] [--pgo-gen[=RUTA]|--pgo-use=RUTA] [--extreme] [--extreme-budget=N] [--extreme-cache=DIR] [--extreme-train=ARGS] [--extreme-proposer=CMD] [--max-level N]
veloz emit-ir ARCHIVO [--target x86_64-windows-msvc|x86_64-linux-gnu|aarch64-linux-gnu|arm64-apple-darwin] [--audit-copies]
veloz sig ARCHIVO [SÍMBOLO] [--offset N] [--max-tokens N]
veloz slice ARCHIVO SÍMBOLO [--offset N] [--max-tokens N]
veloz types ARCHIVO [FUNCIÓN] [--offset N] [--max-tokens N]
veloz costes|costs ARCHIVO [FUNCIÓN] [--max-level N] [--explain-checks] [--offset N] [--max-tokens N]
veloz fmt ARCHIVO [--check]
veloz patch ARCHIVO SÍMBOLO HASH REEMPLAZO
veloz patch-body ARCHIVO FUNCIÓN HASH CUERPO
veloz fix ARCHIVO
veloz explain ID
veloz new NOMBRE [--template cli|web|gpu|lib] [--dir RUTA]
veloz bench ARCHIVO [--rounds N] [--cpu ARCH] [--extreme]
veloz mcp
veloz lsp
)";
std::string spanJson(Span span, std::string_view source) {
    auto pos = std::min(span.start, source.size());
    auto line = 1 + std::count(source.begin(), source.begin() + pos, '\n');
    auto last = source.substr(0, pos).rfind('\n');
    auto column = pos - (last == source.npos ? 0 : last + 1) + 1;
    return "{\"start\":" + std::to_string(span.start) + ",\"end\":" + std::to_string(span.end)
        + ",\"line\":" + std::to_string(line) + ",\"column\":" + std::to_string(column) + "}";
}
void report(const Diagnostic& error, std::string_view file, std::string_view source, const Program* program,
            const fs::path* verifyCheckFile = nullptr) {
    std::string actualFile(error.file);
    std::string_view actualSource = error.source;
    if (actualFile.empty() && program && !program->fileOf(error.span).empty()) actualFile = program->fileOf(error.span);
    if (actualFile.empty()) actualFile = file;
    if (actualSource.empty()) actualSource = program ? program->sourceOf(error.span) : source;
    if (actualSource.empty()) actualSource = source;
    std::cerr << "{\"id\":" << jsonString(error.id) << ",\"message\":" << jsonString(error.message)
              << ",\"file\":" << jsonString(actualFile) << ",\"span\":" << spanJson(error.span, actualSource)
              << ",\"expected\":" << jsonString(error.expected) << ",\"actual\":" << jsonString(error.actual)
              << ",\"related\":[";
    for (size_t i = 0; i < error.related.size(); ++i) {
        if (i) std::cerr << ',';
        const auto relatedSource = program ? program->sourceOf(error.related[i]) : actualSource;
        std::cerr << spanJson(error.related[i], relatedSource);
    }
    std::cerr << "],\"fix\":";
    if (error.fix) {
        const auto fixSource = program ? program->sourceOf(error.fix->span) : actualSource;
        std::cerr << "{\"span\":" << spanJson(error.fix->span, fixSource)
                  << ",\"replacement\":" << jsonString(error.fix->replacement);
        if (verifyCheckFile && !source.empty()) {
            bool verified = false;
            std::error_code ec;
            const std::string fixFile = !error.file.empty() ? std::string(error.file)
                : (program ? program->fileOf(error.fix->span) : std::string{});
            const auto targetCanon = fixFile.empty()
                ? fs::weakly_canonical(*verifyCheckFile, ec)
                : fs::weakly_canonical(pathFrom(fixFile), ec);
            if (!ec && error.fix->span.start <= error.fix->span.end && error.fix->span.end <= fixSource.size()) {
                std::string candidate(fixSource);
                candidate.replace(error.fix->span.start, error.fix->span.end - error.fix->span.start,
                                  error.fix->replacement);
                SourceOverrides overrides;
                overrides[pathText(targetCanon)] = std::move(candidate);
                try {
                    auto candProg = loadProject(*verifyCheckFile, overrides);
                    check(candProg);
                    checkEffects(candProg);
                    verified = true;
                } catch (...) {
                    verified = false;
                }
            }
            std::cerr << ",\"verified\":" << (verified ? "true" : "false");
        }
        std::cerr << "}";
    } else std::cerr << "null";
    std::cerr << ",\"example\":" << jsonString(error.example) << "}\n";
}
void usage() { throw Diagnostic("E_USAGE", help, {}); }
// check executes tests silently, even when a test calls print many times.
struct Discard : std::streambuf {
    int_type overflow(int_type c) override { return traits_type::not_eof(c); }
    std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};
int parseMaxLevel(std::string_view text) {
    int parsed = -1;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc{} || end != text.data() + text.size() || parsed < 0 || parsed > 5) usage();
    return parsed;
}
int dispatch(const std::vector<std::string>& args) {
    std::string source, filename;
    Program program;
    bool loaded = false;
    bool isCheck = false;
    fs::path checkFile;
    try {
        if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) { std::cout << help; return 0; }
        if (args.size() == 1 && args[0] == "--version") { std::cout << "veloz " << VELOZ_VERSION << '\n'; return 0; }
        if (args.empty()) usage();
        const auto& cmd = args[0];
        if (cmd == "mcp") {
            if (args.size() != 1) usage();
            return runMcpServer();
        }
        if (cmd == "lsp") {
            if (args.size() != 1) usage();
            return runLspServer();
        }
        if (cmd == "new") return runNewCommand(args);
        if (cmd == "bench") return runBenchCommand(args);
        if (args.size() < 2) usage();
        if (cmd == "explain") {
            if (args.size() != 2) usage();
            std::cout << explain(args[1]) << '\n';
            return 0;
        }
        filename = args[1];
        auto file = pathFrom(filename);
        source = readFile(file);
        if (cmd == "check") {
            isCheck = true;
            checkFile = file;
        }
        if (cmd == "fix") {
            if (args.size() != 2) usage();
            SourceOverrides overrides;
            for (unsigned edits = 0; ; ++edits) {
                try {
                    program = loadProject(file, overrides);
                    loaded = true;
                    check(program);
                    checkEffects(program);
                    Discard discard;
                    std::ostream quiet(&discard);
                    runTests(program, quiet);
                    std::cout << source;
                    return 0;
                } catch (const Diagnostic& d) {
                    if (edits == 64 || !d.fix || (d.id != "E_SEMICOLON" && d.id != "E_LET_MUT" &&
                        d.id != "E_ELIF" && d.id != "E_PRINTLN" && d.id != "E_IMMUTABLE")) {
                        report(d, filename, source, loaded ? &program : nullptr);
                        return 1;
                    }
                    const auto origin = !d.file.empty() ? d.file
                        : loaded ? program.fileOf(d.fix->span) : std::string{};
                    if ((origin.empty() && loaded) ||
                        (!origin.empty() && fs::canonical(pathFrom(origin)) != fs::canonical(file))) {
                        report(d, filename, source, loaded ? &program : nullptr);
                        return 1;
                    }
                    const auto& edit = *d.fix;
                    source.replace(edit.span.start, edit.span.end - edit.span.start, edit.replacement);
                    overrides[pathText(fs::canonical(file))] = source;
                }
            }
        }
        if (cmd == "fmt") {
            program = parse(source);
            loaded = true;
            std::cout << agentCommand(args, file, source, program);
            return 0;
        }
        program = loadProject(file);
        loaded = true;
        if (cmd == "patch" || cmd == "patch-body" || cmd == "sig" || cmd == "types" || cmd == "costes" || cmd == "costs" || cmd == "slice") {
            std::cout << agentCommand(args, file, source, program);
            return 0;
        }
        check(program);
        checkEffects(program);
        if (cmd == "check") {
            bool doFormat = false;
            std::optional<int> maxLevel;
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--format" && !doFormat) doFormat = true;
                else if (args[i] == "--max-level" && !maxLevel && i + 1 < args.size())
                    maxLevel = parseMaxLevel(args[++i]);
                else usage();
            }
            if (doFormat && formatSource(source) != source)
                throw Diagnostic("E_FORMAT", "Ejecuta veloz fmt para normalizar el formato", {});
            if (maxLevel) checkMemoryLevels(program, *maxLevel);
            Discard discard;
            std::ostream quiet(&discard);
            runTests(program, quiet);
        } else if (cmd == "run") {
            Limits limits;
            program.programArgs.clear();
            program.programArgs.push_back(args[1]);
            std::size_t idx = 2;
            while (idx < args.size()) {
                if (args[idx] == "--") {
                    for (++idx; idx < args.size(); ++idx) program.programArgs.push_back(args[idx]);
                    break;
                }
                if (args[idx] == "--steps" && idx + 1 < args.size()) {
                    const auto& value = args[idx + 1];
                    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), limits.steps);
                    if (ec != std::errc{} || end != value.data() + value.size() || !limits.steps) usage();
                    idx += 2;
                } else usage();
            }
            runMain(program, std::cout, limits, &std::cin);
        } else if (cmd == "emit-ir") {
            auto abi = hostABI;
            bool auditCopies = false;
            std::string target;
            std::size_t arg = 2;
            while (arg < args.size()) {
                if (args[arg] == "--audit-copies" && !auditCopies) {
                    auditCopies = true;
                    ++arg;
                } else if (args[arg] == "--target" && target.empty() && arg + 1 < args.size()) {
                    target = args[arg + 1];
                    if (target == "x86_64-windows-msvc") abi = RuntimeABI::Windows;
                    else if (target == "x86_64-linux-gnu" || target == "aarch64-linux-gnu") abi = RuntimeABI::Linux;
                    else if (target == "arm64-apple-darwin") abi = RuntimeABI::Darwin;
                    else usage();
                    arg += 2;
                } else usage();
            }
            auto module = emitLLVM(program, abi, auditCopies);
            if (!target.empty()) std::cout << "target triple = \"" << target << "\"\n";
            std::cout << module;
        } else if (cmd == "build" && args.size() >= 4 && args[2] == "-o") {
            BuildOptions options;
            std::optional<int> maxLevel;
            bool extremeBudgetSet = false;
            for (std::size_t i = 4; i < args.size(); ++i) {
                if (args[i] == "--release" && !options.release) options.release = true;
                else if (args[i] == "--web" && !options.web) options.web = true;
                else if (args[i] == "--gpu" && !options.gpu) options.gpu = true;
                else if (args[i] == "--extreme" && !options.extreme) options.extreme = true;
                else if (args[i].starts_with("--extreme-budget=") && !extremeBudgetSet) {
                    const std::string val = args[i].substr(17);
                    int parsed = 0;
                    auto [end, ec] = std::from_chars(val.data(), val.data() + val.size(), parsed);
                    if (ec != std::errc{} || end != val.data() + val.size() || parsed < 1 || parsed > 64) usage();
                    options.extremeBudget = parsed;
                    extremeBudgetSet = true;
                } else if (args[i].starts_with("--extreme-cache=") && options.extremeCache.empty()) {
                    options.extremeCache = args[i].substr(16);
                    if (options.extremeCache.empty()) usage();
                } else if (args[i].starts_with("--extreme-train=") && options.extremeTrain.empty()) {
                    options.extremeTrain = args[i].substr(16);
                } else if (args[i].starts_with("--extreme-proposer=") && options.extremeProposer.empty()) {
                    options.extremeProposer = args[i].substr(19);
                    if (options.extremeProposer.empty()) usage();
                } else if (args[i] == "--cpu" && options.cpu.empty() && i + 1 < args.size()) {
                    options.cpu = args[++i];
                    if (options.cpu != "x86-64-v2" && options.cpu != "x86-64-v3" &&
                        options.cpu != "x86-64-v4" && options.cpu != "znver4" &&
                        options.cpu != "native") usage();
                } else if (args[i] == "--pgo-gen" && !options.hasPgoGen && options.pgoUse.empty()) {
                    options.hasPgoGen = true;
                } else if (args[i].starts_with("--pgo-gen=") && !options.hasPgoGen && options.pgoUse.empty()) {
                    options.pgoGen = args[i].substr(10);
                    if (options.pgoGen.empty()) usage();
                    options.hasPgoGen = true;
                } else if (args[i].starts_with("--pgo-use=") && options.pgoUse.empty() && !options.hasPgoGen) {
                    options.pgoUse = args[i].substr(10);
                    if (options.pgoUse.empty()) usage();
                } else if (args[i] == "--max-level" && !maxLevel && i + 1 < args.size()) {
                    maxLevel = parseMaxLevel(args[++i]);
                } else usage();
            }
            if ((extremeBudgetSet || !options.extremeCache.empty() || !options.extremeTrain.empty() ||
                 !options.extremeProposer.empty()) && !options.extreme) {
                usage();
            }
            if (options.extreme && (options.hasPgoGen || !options.pgoUse.empty())) usage();
            if (maxLevel) checkMemoryLevels(program, *maxLevel);
            options.sourcePath = pathText(fs::weakly_canonical(file));
            auto out = pathFrom(args[3]);
            if ((fs::exists(out) && fs::equivalent(file, out)) || fs::weakly_canonical(out) == fs::weakly_canonical(file))
                throw Diagnostic("E_OUTPUT", "La salida sobrescribiría el archivo fuente", {});
            buildBinary(program, out, options);
        } else usage();
        return 0;
    } catch (const Diagnostic& d) {
        report(d, filename, source, loaded ? &program : nullptr, (isCheck && !source.empty()) ? &checkFile : nullptr);
    } catch (const std::exception& e) {
        report(Diagnostic("E_HOST", e.what(), {}), filename, source, loaded ? &program : nullptr);
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
