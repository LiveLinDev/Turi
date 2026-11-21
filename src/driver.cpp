#include "commands.hpp"
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace veloz {
fs::path pathFrom(std::string_view text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string pathText(const fs::path& path) {
    auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string readFile(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input || !fs::is_regular_file(file))
        throw Diagnostic("E_IO", "No se pudo leer " + pathText(file), {});
    std::string contents{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) throw Diagnostic("E_IO", "Error de lectura", {});
    return contents;
}
namespace {
void writeFile(const fs::path& file, std::string_view text) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.close();
    if (!stream) throw Diagnostic("E_IO", "Error de escritura en " + pathText(file), {});
}
struct Stage {
    fs::path dir;
    explicit Stage(const fs::path& destination) {
        std::random_device random;
        auto parent = fs::absolute(destination).parent_path();
        for (unsigned i = 0; i < 16; ++i) {
            dir = parent / (".veloz-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(dir)) return;
        }
        throw Diagnostic("E_IO", "No se pudo crear el directorio temporal", {});
    }
    ~Stage() { std::error_code ignored; fs::remove_all(dir, ignored); }
};
void publish(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Diagnostic("E_IO", "No se pudo guardar " + pathText(to), {});
#else
    fs::rename(from, to);
#endif
}
#ifdef _WIN32
std::wstring quoteArgument(std::wstring_view arg) {
    std::wstring result = L"\"";
    std::size_t slash = 0;
    for (auto c : arg) {
        if (c == L'\\') { ++slash; continue; }
        result.append(c == L'"' ? 2 * slash + 1 : slash, L'\\');
        slash = 0;
        result += c;
    }
    result.append(2 * slash, L'\\');
    return result + L'"';
}
#endif
int clangProcess(const std::vector<fs::path>& args) {
#ifdef _WIN32
    std::wstring line;
    for (const auto& arg : args) {
        if (!line.empty()) line += L' ';
        line += quoteArgument(arg.wstring());
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &info))
        throw Diagnostic("E_CLANG", "No se pudo iniciar Clang. Configura VELOZ_CLANG.", {});
    CloseHandle(info.hThread);
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t child;
    if (posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ))
        throw Diagnostic("E_CLANG", "No se pudo iniciar Clang. Configura VELOZ_CLANG.", {});
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
}
void replaceFile(const fs::path& file, std::string_view text) {
    Stage stage(file);
    auto temp = stage.dir / "contenido";
    writeFile(temp, text);
    if (fs::exists(file)) fs::permissions(temp, fs::status(file).permissions());
    publish(temp, file);
}
void buildBinary(const Program& program, const fs::path& destination, const BuildOptions& options) {
    auto ir = emitLLVM(program);
    Stage stage(destination);
    auto source = stage.dir / "program.ll";
    writeFile(source, ir);
#ifdef _WIN32
    auto exe = stage.dir / "program.exe";
#else
    auto exe = stage.dir / "program";
#endif
#ifndef VELOZ_CLANG_PATH
#define VELOZ_CLANG_PATH "clang"
#endif
    fs::path clang = pathFrom(VELOZ_CLANG_PATH);
#ifdef _WIN32
    wchar_t* overridePath = nullptr;
    std::size_t length = 0;
    if (!_wdupenv_s(&overridePath, &length, L"VELOZ_CLANG") && length > 1)
        clang = fs::path(overridePath);
    std::free(overridePath);
#else
    if (const char* env = std::getenv("VELOZ_CLANG"); env && *env) clang = pathFrom(env);
#endif
    std::vector<fs::path> args{clang, options.release ? "-O3" : "-O2"};
    if (!options.cpu.empty()) args.push_back(pathFrom("-march=" + options.cpu));
    if (options.release) args.insert(args.end(), {"-flto", "-fuse-ld=lld"});
    args.insert(args.end(), {"-Wno-override-module", "-x", "ir", source, "-o", exe});
#ifdef _WIN32
    args.insert(args.end(), {"-Xlinker", "/STACK:33554432"});
#endif
    if (clangProcess(args))
        throw Diagnostic("E_CLANG", "Clang no pudo producir el ejecutable", {});
    publish(exe, destination);
}
}
