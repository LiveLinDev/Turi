#include "commands.hpp"
#include "native_collections_rt.hpp"
#include "native_conc_rt.hpp"
#include "native_crypto_rt.hpp"
#include "native_gpu_rt.hpp"
#include "native_pg_rt.hpp"
#include "native_sys_rt.hpp"
#include "native_web_rt.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
fs::path envPath(const char* asciiName);
// VELOZ_KEEP_STAGE=DIR conserva program.ll y los runtimes C para inspeccionar o reproducir a mano.
void keepStage(const Stage& stage) {
    const auto keep = envPath("VELOZ_KEEP_STAGE");
    if (keep.empty()) return;
    std::error_code ignored;
    fs::create_directories(keep, ignored);
    for (const auto& entry : fs::directory_iterator(stage.dir, ignored)) {
        if (!entry.is_regular_file(ignored)) continue;
        const auto ext = entry.path().extension();
        if (ext == ".ll" || ext == ".c" || ext == ".json")
            fs::copy_file(entry.path(), keep / entry.path().filename(), fs::copy_options::overwrite_existing, ignored);
    }
}
// VELOZ_CLANG_FLAGS="-a -b" añade opciones a la compilación del IR (experimentos de ajuste).
std::vector<std::string> extraClangFlags() {
    std::vector<std::string> out;
    const std::string text = envPath("VELOZ_CLANG_FLAGS").string();
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        std::size_t j = i;
        while (j < text.size() && text[j] != ' ') ++j;
        if (j > i) out.push_back(text.substr(i, j - i));
        i = j;
    }
    return out;
}
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
int spawnProcess(const std::vector<fs::path>& args, const char* errId, const char* errMsg) {
#ifdef _WIN32
    std::wstring line;
    for (const auto& arg : args) {
        if (!line.empty()) line += L' ';
        line += quoteArgument(arg.wstring());
    }
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &info))
        throw Diagnostic(errId, errMsg, {});
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
        throw Diagnostic(errId, errMsg, {});
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
int clangProcess(const std::vector<fs::path>& args) {
    return spawnProcess(args, "E_CLANG", "No se pudo iniciar Clang. Configura VELOZ_CLANG.");
}
fs::path envPath(const char* asciiName) {
#ifdef _WIN32
    std::wstring wname(asciiName, asciiName + std::strlen(asciiName));
    wchar_t* val = nullptr;
    std::size_t len = 0;
    fs::path out;
    if (!_wdupenv_s(&val, &len, wname.c_str()) && len > 1 && val) out = fs::path(val);
    std::free(val);
    return out;
#else
    if (const char* env = std::getenv(asciiName); env && *env) return pathFrom(env);
    return {};
#endif
}
fs::path selfExeDir() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size()) return fs::path(buf.substr(0, n)).parent_path();
#else
    std::array<char, 4096> buf{};
    ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n > 0) return fs::path(std::string(buf.data(), static_cast<std::size_t>(n))).parent_path();
#endif
    return fs::current_path();
}
fs::path findSuperoptScript() {
    if (auto env = envPath("VELOZ_SUPEROPT"); !env.empty() && fs::exists(env)) return env;
    auto checkAncestors = [](fs::path dir) -> fs::path {
        for (int i = 0; i < 6 && !dir.empty(); ++i) {
            auto cand = dir / "tools" / "superopt" / "superopt.py";
            if (fs::exists(cand)) return cand;
            auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return {};
    };
    if (auto p = checkAncestors(fs::current_path()); !p.empty()) return p;
    return checkAncestors(selfExeDir());
}
std::string jsonEscape(std::string_view text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
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
    // `sanitize_address` solo sirve cuando se instrumenta el IR de `emit-ir` con -fsanitize=address.
    // En un ejecutable normal impide a LLVM especular cargas (LICM, if-conversion, vectorización).
    // El reservador de celdas pequeñas solo se activa en ejecutables: en el IR de `emit-ir` queda
    // en malloc/free para que ASan y las auditorías vean cada celda.
    constexpr std::string_view cellsOff = "@tz.cellSlab = internal constant i1 false";
    if (auto at = ir.find(cellsOff); at != std::string::npos)
        ir.replace(at, cellsOff.size(), "@tz.cellSlab = internal constant i1 true");
    constexpr std::string_view sanitizeAttr = " sanitize_address";
    for (auto at = ir.find(sanitizeAttr); at != std::string::npos; at = ir.find(sanitizeAttr, at))
        ir.erase(at, sanitizeAttr.size());
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
    const auto appendCpuFlags = [](std::vector<fs::path>& out, const std::string& cpu) {
        if (cpu.empty()) return;
        if (cpu == "avx10") {
#if defined(VELOZ_CLANG_MAJOR) && VELOZ_CLANG_MAJOR < 22
            out.push_back(pathFrom("-march=graniterapids"));
#else
            out.push_back(pathFrom("-march=x86-64-v4"));
            out.push_back(pathFrom("-mavx10.1"));
#endif
        } else {
            out.push_back(pathFrom("-march=" + cpu));
        }
    };
    appendCpuFlags(args, options.cpu);
    const auto tuningFlags = extraClangFlags();
    for (const auto& flag : tuningFlags) args.push_back(pathFrom(flag));
    if (options.release) args.insert(args.end(), {"-flto", "-fuse-ld=lld"});
    if (options.hasPgoGen) {
        args.push_back(options.pgoGen.empty() ? fs::path("-fprofile-generate")
                                              : pathFrom("-fprofile-generate=" + options.pgoGen));
    } else if (!options.pgoUse.empty()) {
        args.push_back(pathFrom("-fprofile-use=" + options.pgoUse));
    }
    args.insert(args.end(), {"-Wno-override-module"});
    std::vector<fs::path> extraC;
    std::vector<std::string> cFlags;
    const bool usesNet = (ir.find("@tz_sys_tcp_") != std::string::npos ||
                          ir.find("@tz_sys_sock_") != std::string::npos);
    [[maybe_unused]] const bool usesMutex = (ir.find("@tz_sys_mutex_") != std::string::npos ||
                                             ir.find("@tz_sys_lock_") != std::string::npos);
    if (ir.find("@tz_sys_") != std::string::npos) {
        auto rtSource = stage.dir / "turi_rt.c";
        writeFile(rtSource, native::turiSysRuntimeC());
        if (usesNet) {
            args.emplace_back("-DTZ_USE_NET");
            cFlags.emplace_back("-DTZ_USE_NET");
        }
        args.insert(args.end(), {"-x", "c", rtSource});
        extraC.push_back(rtSource);
    }
    const bool usesConc = (ir.find("@tz_conc_") != std::string::npos);
    if (ir.find("@tz_col_") != std::string::npos) {
        auto colRtSource = stage.dir / "turi_col_rt.c";
        writeFile(colRtSource, native::turiCollectionsRuntimeC());
        args.insert(args.end(), {"-x", "c", colRtSource});
        extraC.push_back(colRtSource);
    }
    if (usesConc) {
        auto concRtSource = stage.dir / "turi_conc_rt.c";
        writeFile(concRtSource, native::turiConcurrencyRuntimeC());
        args.insert(args.end(), {"-x", "c", concRtSource});
        extraC.push_back(concRtSource);
    }
    auto hasIrCallOrRef = [&](std::string_view prefix) {
        std::size_t pos = 0;
        while ((pos = ir.find(prefix, pos)) != std::string::npos) {
            const std::size_t lineStart = ir.rfind('\n', pos);
            const std::size_t start = (lineStart == std::string::npos) ? 0 : lineStart + 1;
            const std::size_t lineEnd = ir.find('\n', pos);
            const std::string_view line = std::string_view(ir).substr(
                start, (lineEnd == std::string::npos ? ir.size() : lineEnd) - start);
            if (!line.starts_with("declare ")) return true;
            pos += prefix.size();
        }
        return false;
    };
    const bool needWs = hasIrCallOrRef("@tz_web_ws_");
    const bool needSrv = options.web || needWs || hasIrCallOrRef("@tz_web_srv_");
    const bool needClient = hasIrCallOrRef("@tz_web_cli_");
    const bool needSql = hasIrCallOrRef("@tz_web_sql_");
    const bool needTls = hasIrCallOrRef("@tz_web_tls_");
    const bool usesWeb = needSrv || needClient || needSql || needTls;
    if (usesWeb) {
        auto webRtSource = stage.dir / "turi_web_rt.c";
        writeFile(webRtSource, native::turiWebRuntimeC(needSrv, needWs, needClient, needSql, needTls));
        args.insert(args.end(), {"-x", "c", webRtSource});
        extraC.push_back(webRtSource);
    }
    const bool usesGpu = options.gpu || hasIrCallOrRef("@tz_gpu_");
    if (usesGpu) {
        auto gpuRtSource = stage.dir / "turi_gpu_rt.c";
        writeFile(gpuRtSource, native::turiGpuRuntimeC());
        args.insert(args.end(), {"-x", "c", gpuRtSource});
        extraC.push_back(gpuRtSource);
    }
    const bool usesCrypto = hasIrCallOrRef("@tz_crypto_");
    if (usesCrypto) {
        auto cryptoRtSource = stage.dir / "turi_crypto_rt.c";
        writeFile(cryptoRtSource, native::turiCryptoRuntimeC());
        args.insert(args.end(), {"-x", "c", cryptoRtSource});
        extraC.push_back(cryptoRtSource);
    }
    const bool usesPg = hasIrCallOrRef("@tz_pg_");
    if (usesPg) {
        auto pgRtSource = stage.dir / "turi_pg_rt.c";
        writeFile(pgRtSource, native::turiPgRuntimeC());
        args.insert(args.end(), {"-x", "c", pgRtSource});
        extraC.push_back(pgRtSource);
    }
    std::vector<fs::path> linkFlags;
    std::vector<std::string> rawLinkFlags;
    auto pushLink = [&](std::string s) {
        linkFlags.push_back(pathFrom(s));
        rawLinkFlags.push_back(std::move(s));
    };
    std::vector<std::string> seenLibs;
    std::vector<std::string> libFileInputs;
    for (const auto& lib : program.linkLibs) {
        if (lib.empty() || std::find(seenLibs.begin(), seenLibs.end(), lib) != seenLibs.end()) continue;
        seenLibs.push_back(lib);
        const bool isPath = lib.find('/') != std::string::npos || lib.find('\\') != std::string::npos ||
                            lib.ends_with(".o") || lib.ends_with(".obj") || lib.ends_with(".a") ||
                            lib.ends_with(".lib") || lib.ends_with(".so") || lib.ends_with(".dylib") ||
                            lib.ends_with(".dll");
        if (isPath) libFileInputs.push_back(lib);
        else pushLink("-l" + lib);
    }
#ifdef _WIN32
    if (usesNet || usesWeb) pushLink("-lws2_32");
    if (needSrv) pushLink("-lmswsock");
    if (usesCrypto) pushLink("-lbcrypt");
    if (usesWeb && options.release) {
        pushLink("-Xlinker"); pushLink("/OPT:REF");
        pushLink("-Xlinker"); pushLink("/OPT:ICF");
    }
    pushLink("-Xlinker"); pushLink("/STACK:33554432");
#else
    if (ir.find("@tz_sys_") != std::string::npos ||
        ir.find("@tz_col_") != std::string::npos ||
        ir.find("@llvm.sin.") != std::string::npos ||
        ir.find("@llvm.cos.") != std::string::npos ||
        ir.find("@llvm.pow.") != std::string::npos ||
        ir.find("@llvm.round.") != std::string::npos ||
        ir.find("@llvm.fma.") != std::string::npos ||
        ir.find("@llvm.sqrt.") != std::string::npos) {
        pushLink("-lm");
    }
    if (usesMutex || usesConc || usesWeb) pushLink("-lpthread");
    if (needSql || needTls || needClient || usesGpu || usesPg) pushLink("-ldl");
    if (usesWeb && options.release) {
        pushLink("-ffunction-sections");
        pushLink("-fdata-sections");
        pushLink("-Wl,--gc-sections");
    }
#endif
    args.insert(args.end(), {"-x", "ir", source});
    if (!libFileInputs.empty()) {
        args.push_back("-x");
        args.push_back("none");
        for (const auto& lib : libFileInputs) {
            args.push_back(pathFrom(lib));
            rawLinkFlags.push_back(lib);
        }
    }
    args.insert(args.end(), {"-o", exe});
    args.insert(args.end(), linkFlags.begin(), linkFlags.end());
    if (options.extreme && !options.hasPgoGen && options.pgoUse.empty()) {
        fs::path script = findSuperoptScript();
        if (script.empty()) {
            throw Diagnostic("E_EXTREME", "No se encontró tools/superopt/superopt.py (configura VELOZ_SUPEROPT).", {});
        }
        fs::path py = envPath("VELOZ_PYTHON");
        if (py.empty()) {
#ifdef _WIN32
            py = pathFrom("python");
#else
            py = pathFrom("python3");
#endif
        }
        fs::path cacheDir = options.extremeCache.empty()
                                ? (fs::absolute(destination).parent_path() / ".turi-extreme-cache")
                                : pathFrom(options.extremeCache);
        std::string jobJson = "{\n";
        jobJson += "  \"ir\": " + jsonEscape(pathText(source)) + ",\n";
        jobJson += "  \"source_path\": " + jsonEscape(options.sourcePath) + ",\n";
        jobJson += "  \"output\": " + jsonEscape(pathText(exe)) + ",\n";
        jobJson += "  \"clang\": " + jsonEscape(pathText(clang)) + ",\n";
        jobJson += "  \"cpu\": " + jsonEscape(options.cpu) + ",\n";
        jobJson += "  \"release\": " + std::string(options.release ? "true" : "false") + ",\n";
        jobJson += "  \"budget\": " + std::to_string(options.extremeBudget) + ",\n";
        jobJson += "  \"cache\": " + jsonEscape(pathText(cacheDir)) + ",\n";
        jobJson += "  \"train\": " + jsonEscape(options.extremeTrain) + ",\n";
        jobJson += "  \"proposer_cmd\": " + jsonEscape(options.extremeProposer) + ",\n";
        jobJson += "  \"extra_c\": [";
        for (std::size_t i = 0; i < extraC.size(); ++i) {
            if (i) jobJson += ", ";
            jobJson += jsonEscape(pathText(extraC[i]));
        }
        jobJson += "],\n  \"c_flags\": [";
        for (std::size_t i = 0; i < cFlags.size(); ++i) {
            if (i) jobJson += ", ";
            jobJson += jsonEscape(cFlags[i]);
        }
        jobJson += "],\n  \"link_flags\": [";
        for (std::size_t i = 0; i < rawLinkFlags.size(); ++i) {
            if (i) jobJson += ", ";
            jobJson += jsonEscape(rawLinkFlags[i]);
        }
        jobJson += "]\n}\n";
        auto jobPath = stage.dir / "superopt_job.json";
        writeFile(jobPath, jobJson);
        if (spawnProcess({py, script, "--job", jobPath, "--quiet"},
                         "E_EXTREME", "No se pudo iniciar Python para --extreme (configura VELOZ_PYTHON).") ||
            !fs::exists(exe)) {
            throw Diagnostic("E_EXTREME", "La optimización --extreme falló sin producir el ejecutable", {});
        }
    } else if (options.release && options.cpu == "avx10") {
        // avx10 no es un nombre de CPU para el enlazador: se compila el IR a objeto con sus opciones.
        // Compile LLVM IR to native object first so lld LTO does not drop -march=<cpu> target features.
        auto irObj = stage.dir / "program.o";
        std::vector<fs::path> irArgs{clang, "-O3"};
        appendCpuFlags(irArgs, options.cpu);
        for (const auto& flag : tuningFlags) irArgs.push_back(pathFrom(flag));
        if (options.hasPgoGen) {
            irArgs.push_back(options.pgoGen.empty() ? fs::path("-fprofile-generate")
                                                    : pathFrom("-fprofile-generate=" + options.pgoGen));
        } else if (!options.pgoUse.empty()) {
            irArgs.push_back(pathFrom("-fprofile-use=" + options.pgoUse));
        }
        irArgs.insert(irArgs.end(), {"-Wno-override-module", "-c", "-x", "ir", source, "-o", irObj});
        if (clangProcess(irArgs))
            throw Diagnostic("E_CLANG", "Clang no pudo compilar el IR nativo", {});

        std::vector<fs::path> linkArgs{clang, "-O3", "-fuse-ld=lld"};
        appendCpuFlags(linkArgs, options.cpu);
        if (options.hasPgoGen) {
            linkArgs.push_back(options.pgoGen.empty() ? fs::path("-fprofile-generate")
                                                      : pathFrom("-fprofile-generate=" + options.pgoGen));
        } else if (!options.pgoUse.empty()) {
            linkArgs.push_back(pathFrom("-fprofile-use=" + options.pgoUse));
        }
        linkArgs.push_back("-Wno-override-module");
        for (const auto& cf : cFlags) linkArgs.push_back(pathFrom(cf));
        for (const auto& ec : extraC) linkArgs.insert(linkArgs.end(), {"-x", "c", ec});
        if (!extraC.empty()) linkArgs.push_back("-x"), linkArgs.push_back("none");
        linkArgs.push_back(irObj);
        for (const auto& lib : libFileInputs) linkArgs.push_back(pathFrom(lib));
        linkArgs.insert(linkArgs.end(), {"-o", exe});
        linkArgs.insert(linkArgs.end(), linkFlags.begin(), linkFlags.end());
        if (clangProcess(linkArgs))
            throw Diagnostic("E_CLANG", "Clang no pudo enlazar el ejecutable", {});
    } else {
        if (options.release && !options.cpu.empty()) {
            // Programa y runtime se optimizan juntos (LTO): las llamadas a mapas, cadenas y E/S se
            // integran y se especializan. El enlazador recibe la CPU para generar código de -march.
#ifdef _WIN32
            args.push_back(pathFrom("-Wl,/mllvm:-mcpu=" + options.cpu));
#else
            args.push_back(pathFrom("-Wl,-mllvm,-mcpu=" + options.cpu));
#endif
        }
        if (clangProcess(args))
            throw Diagnostic("E_CLANG", "Clang no pudo producir el ejecutable", {});
    }
    keepStage(stage);
    publish(exe, destination);
}
}
