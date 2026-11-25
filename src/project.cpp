#include "project.hpp"
#include "prelude.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <map>
#include <set>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <vector>
#endif
#endif

namespace veloz {
namespace {

std::optional<fs::path> executableDir() {
#ifdef _WIN32
    wchar_t buf[32768];
    DWORD len = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (len > 0 && len < std::size(buf)) return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    uint32_t sz = 0;
    _NSGetExecutablePath(nullptr, &sz);
    if (sz > 0) {
        std::vector<char> buf(sz + 1, '\0');
        if (_NSGetExecutablePath(buf.data(), &sz) == 0) {
            std::error_code ec;
            auto can = fs::weakly_canonical(pathFrom(buf.data()), ec);
            if (!ec) return can.parent_path();
            return pathFrom(buf.data()).parent_path();
        }
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return pathFrom(buf).parent_path();
    }
#endif
    return std::nullopt;
}

std::optional<fs::path> probeStdAncestors(const fs::path& start, std::string_view stdName) {
    std::error_code ec;
    fs::path cur = fs::weakly_canonical(start, ec);
    if (ec) cur = start;
    while (!cur.empty()) {
        const auto cand = cur / "lib" / pathFrom(stdName);
        if (fs::exists(cand / "veloz.pkg", ec)) return cand;
        const auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return std::nullopt;
}

std::optional<fs::path> resolveStdPackage(std::string_view stdName, const fs::path& pkgDir) {
    std::error_code ec;
#ifdef _WIN32
    wchar_t* homeBuf = nullptr;
    size_t homeLen = 0;
    if (!_wdupenv_s(&homeBuf, &homeLen, L"TURI_HOME") && homeBuf && homeLen > 1) {
        const auto cand = fs::path(homeBuf) / "lib" / pathFrom(stdName);
        free(homeBuf);
        if (fs::exists(cand / "veloz.pkg", ec)) return cand;
    } else if (homeBuf) {
        free(homeBuf);
    }
#else
    if (const char* home = std::getenv("TURI_HOME"); home && *home) {
        const auto cand = pathFrom(home) / "lib" / pathFrom(stdName);
        if (fs::exists(cand / "veloz.pkg", ec)) return cand;
    }
#endif
    if (auto found = probeStdAncestors(pkgDir, stdName)) return found;
    if (const auto cwd = fs::current_path(ec); !ec) {
        if (auto found = probeStdAncestors(cwd, stdName)) return found;
    }
    if (auto exe = executableDir()) {
        if (auto found = probeStdAncestors(*exe, stdName)) return found;
    }
    return std::nullopt;
}

bool packageName(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    });
}
bool versionNumber(std::string_view version) {
    std::size_t start = 0;
    for (unsigned part = 0; part < 3; ++part) {
        const auto end = version.find('.', start);
        const auto stop = part == 2 ? version.size() : end;
        if (stop == std::string_view::npos || stop == start ||
            (stop - start > 1 && version[start] == '0')) return false;
        for (std::size_t i = start; i < stop; ++i)
            if (version[i] < '0' || version[i] > '9') return false;
        if (part < 2 && end == std::string_view::npos) return false;
        start = end == std::string_view::npos ? version.size() : end + 1;
    }
    return true;
}

struct Requirement {
    std::string name, version, declaredPath;
    fs::path directory;
    Span span;
};
struct Manifest {
    std::string name, version;
    std::vector<Requirement> dependencies;
};
struct Package {
    Manifest manifest;
    fs::path directory;
    unsigned state = 1;
};

[[noreturn]] void manifestError(std::string id, std::string message, Span span,
                                const fs::path& file, std::string_view source) {
    Diagnostic error(std::move(id), std::move(message), span);
    error.file = pathText(file);
    error.source = source;
    throw error;
}

Manifest parseManifest(const fs::path& file, std::string_view source) {
    Manifest result;
    std::size_t cursor = 0, line = 0;
    std::set<std::string> requirements;
    while (cursor < source.size()) {
        const auto start = cursor;
        auto end = source.find('\n', cursor);
        if (end == std::string_view::npos) end = source.size();
        cursor = end == source.size() ? end : end + 1;
        auto lineEnd = end;
        if (lineEnd > start && source[lineEnd - 1] == '\r') --lineEnd;
        const auto text = source.substr(start, lineEnd - start);
        const Span span{start, lineEnd};
        if (line == 0) {
            constexpr std::string_view prefix = "package ";
            if (!text.starts_with(prefix) || !packageName(text.substr(prefix.size())))
                manifestError("E_MANIFEST", "La primera línea debe ser package NOMBRE.", span, file, source);
            result.name = text.substr(prefix.size());
        } else if (line == 1) {
            constexpr std::string_view prefix = "version ";
            if (!text.starts_with(prefix) || !versionNumber(text.substr(prefix.size())))
                manifestError("E_MANIFEST", "La segunda línea debe ser version MAJOR.MINOR.PATCH.", span, file, source);
            result.version = text.substr(prefix.size());
        } else {
            constexpr std::string_view prefix = "require ";
            if (!text.starts_with(prefix))
                manifestError("E_MANIFEST", "Solo se admiten líneas require NOMBRE VERSION \"RUTA\".", span, file, source);
            const auto rest = text.substr(prefix.size());
            const auto first = rest.find(' ');
            const auto second = first == std::string_view::npos ? first : rest.find(' ', first + 1);
            if (first == std::string_view::npos || second == std::string_view::npos)
                manifestError("E_MANIFEST", "Require incompleto.", span, file, source);
            Requirement requirement;
            requirement.name = rest.substr(0, first);
            requirement.version = rest.substr(first + 1, second - first - 1);
            const auto quoted = rest.substr(second + 1);
            if (!packageName(requirement.name) || !versionNumber(requirement.version) || quoted.size() < 2 ||
                quoted.front() != '"' || quoted.back() != '"' ||
                quoted.substr(1, quoted.size() - 2).find('"') != std::string_view::npos)
                manifestError("E_MANIFEST", "Require debe tener nombre, versión exacta y una ruta entre comillas.", span, file, source);
            requirement.declaredPath = quoted.substr(1, quoted.size() - 2);
            requirement.span = span;
            if (!requirements.insert(requirement.name).second)
                manifestError("E_PACKAGE_DUP", "Dependencia duplicada: " + requirement.name, span, file, source);
            result.dependencies.push_back(std::move(requirement));
        }
        ++line;
    }
    if (line < 2) manifestError("E_MANIFEST", "El manifiesto requiere package y version.", {0, source.size()}, file, source);
    return result;
}

class Loader {
    fs::path entry;
    const SourceOverrides& overrides;
    Program result;
    std::vector<Package> packages;
    std::map<std::string, std::size_t> packageIndices;
    std::map<std::string, std::size_t> moduleIndices;

    std::string read(const fs::path& path) {
        std::error_code error;
        auto canonical = fs::canonical(path, error);
        const auto key = pathText(error ? fs::absolute(path).lexically_normal() : canonical);
        if (auto found = overrides.find(key); found != overrides.end()) return found->second;
        return readFile(path);
    }
    std::size_t loadPackage(const fs::path& directory, const std::string& expectedName = {},
                            const std::string& expectedVersion = {}, const Requirement* from = nullptr,
                            const fs::path& fromManifest = {}) {
        std::error_code error;
        auto canonical = fs::canonical(directory, error);
        if (error || !fs::is_directory(canonical)) {
            if (from) {
                Diagnostic d("E_PACKAGE_PATH", "La ruta local de la dependencia no existe: " + from->declaredPath, from->span);
                d.file = pathText(fromManifest);
                d.source = read(fromManifest);
                throw d;
            }
            throw Diagnostic("E_PACKAGE_PATH", "El paquete no existe: " + pathText(directory), {});
        }
        const auto manifestPath = canonical / "veloz.pkg";
        if (!fs::is_regular_file(manifestPath)) {
            if (from) {
                Diagnostic d("E_PACKAGE_PATH", "La dependencia no contiene veloz.pkg: " + from->declaredPath, from->span);
                d.file = pathText(fromManifest);
                d.source = read(fromManifest);
                throw d;
            }
            throw Diagnostic("E_PACKAGE_PATH", "Falta veloz.pkg en " + pathText(canonical), {});
        }
        const auto manifestSource = read(manifestPath);
        auto manifest = parseManifest(manifestPath, manifestSource);
        if ((!expectedName.empty() && manifest.name != expectedName) ||
            (!expectedVersion.empty() && manifest.version != expectedVersion)) {
            Diagnostic d("E_PACKAGE_VERSION", "El paquete destino no coincide con require.", from ? from->span : Span{});
            d.expected = expectedName + " " + expectedVersion;
            d.actual = manifest.name + " " + manifest.version;
            d.file = from ? pathText(fromManifest) : pathText(manifestPath);
            d.source = from ? read(fromManifest) : manifestSource;
            throw d;
        }
        if (auto found = packageIndices.find(manifest.name); found != packageIndices.end()) {
            auto& prior = packages[found->second];
            if (prior.manifest.version != manifest.version || prior.directory != canonical) {
                Diagnostic d("E_PACKAGE_CONFLICT", "Versiones o rutas en conflicto para el paquete " + manifest.name,
                             from ? from->span : Span{});
                d.expected = prior.manifest.version;
                d.actual = manifest.version;
                d.file = from ? pathText(fromManifest) : pathText(manifestPath);
                d.source = from ? read(fromManifest) : manifestSource;
                throw d;
            }
            if (prior.state == 1)
                manifestError("E_PACKAGE_CYCLE", "Ciclo de dependencias de paquetes: " + manifest.name,
                              from ? from->span : Span{}, from ? fromManifest : manifestPath,
                              from ? read(fromManifest) : manifestSource);
            return found->second;
        }
        const auto index = packages.size();
        packageIndices.emplace(manifest.name, index);
        packages.push_back(Package{std::move(manifest), canonical});
        for (std::size_t required = 0; required < packages[index].manifest.dependencies.size(); ++required) {
            const auto requirement = packages[index].manifest.dependencies[required];
            fs::path destination;
            if (requirement.declaredPath == "std" || requirement.declaredPath.starts_with("std/")) {
                const std::string stdName = requirement.declaredPath == "std"
                    ? requirement.name
                    : requirement.declaredPath.substr(4);
                if (stdName.empty() || stdName.find("..") != std::string::npos ||
                    stdName.find('/') != std::string::npos || stdName.find('\\') != std::string::npos) {
                    manifestError("E_PACKAGE_PATH", "Nombre de paquete std inválido: " + requirement.declaredPath,
                                  requirement.span, manifestPath, manifestSource);
                }
                const auto resolved = resolveStdPackage(stdName, canonical);
                if (!resolved) {
                    manifestError("E_PACKAGE_PATH", "Paquete estándar no encontrado: " + stdName,
                                  requirement.span, manifestPath, manifestSource);
                }
                destination = *resolved;
            } else {
                auto relative = pathFrom(requirement.declaredPath);
                if (requirement.declaredPath.empty() || relative.is_absolute() || relative.has_root_name() ||
                    requirement.declaredPath.find("://") != std::string::npos)
                    manifestError("E_PACKAGE_PATH", "Las dependencias usan rutas locales relativas.", requirement.span,
                                  manifestPath, manifestSource);
                destination = canonical / relative;
                if (!fs::exists(destination))
                    manifestError("E_PACKAGE_PATH", "La ruta local de la dependencia no existe: " + requirement.declaredPath,
                                  requirement.span, manifestPath, manifestSource);
            }
            loadPackage(destination, requirement.name, requirement.version, &requirement, manifestPath);
        }
        packages[index].state = 2;
        return index;
    }
    Span importSpan(std::size_t file, const std::string& name) const {
        for (const auto& import : result.imports)
            if (import.span.file == file && import.name == name) return import.span;
        return {0, 0, file};
    }
    void loadModules() {
        for (std::size_t package = 0; package < packages.size(); ++package) {
            const auto sourceDirectory = packages[package].directory / "src";
            if (!fs::is_directory(sourceDirectory)) {
                const auto manifest = packages[package].directory / "veloz.pkg";
                Diagnostic error("E_PACKAGE_PATH", "Falta el directorio src en " + packages[package].manifest.name, {});
                error.file = pathText(manifest);
                error.source = read(manifest);
                throw error;
            }
            std::vector<fs::path> files;
            for (const auto& item : fs::directory_iterator(sourceDirectory))
                if (item.is_regular_file() && item.path().extension() == ".turi") files.push_back(item.path());
            std::sort(files.begin(), files.end());
            for (const auto& file : files) {
                const auto canonical = fs::canonical(file);
                const auto module = pathText(file.stem());
                const auto index = result.files.size();
                result.files.push_back({pathText(canonical), read(canonical)});
                if (!packageName(module))
                    throw Diagnostic("E_MODULE_NAME", "El nombre del archivo debe ser un identificador camelCase.", {0, 0, index});
                if (auto found = moduleIndices.find(module); found != moduleIndices.end()) {
                    Diagnostic d("E_DUP_MODULE", "Módulo duplicado: " + module, {0, 0, index});
                    d.related.push_back({0, 0, result.modules[found->second].file});
                    throw d;
                }
                moduleIndices.emplace(module, result.modules.size());
                parseInto(result, result.files.back().text, module, index);
                result.modules.back().package = packages[package].manifest.name;
            }
        }
        for (auto& module : result.modules) {
            std::set<std::string> seen;
            const auto& package = packages[packageIndices.at(module.package)].manifest;
            for (const auto& imported : module.imports) {
                if (!seen.insert(imported).second)
                    throw Diagnostic("E_DUP_IMPORT", "Import duplicado: " + imported, importSpan(module.file, imported));
                auto target = moduleIndices.find(imported);
                if (target == moduleIndices.end())
                    throw Diagnostic("E_IMPORT", "Módulo desconocido: " + imported, importSpan(module.file, imported));
                const auto& targetPackage = result.modules[target->second].package;
                const bool direct = std::any_of(package.dependencies.begin(), package.dependencies.end(),
                    [&](const Requirement& requirement) { return requirement.name == targetPackage; });
                if (targetPackage != module.package && !direct)
                    throw Diagnostic("E_IMPORT_TRANSITIVE", "El módulo pertenece a una dependencia que no es directa: " + imported,
                                     importSpan(module.file, imported));
            }
        }
        std::vector<unsigned> state(result.modules.size());
        auto visit = [&](auto&& self, std::size_t index) -> void {
            if (state[index] == 2) return;
            if (state[index] == 1) throw Diagnostic("E_IMPORT_CYCLE", "Ciclo de imports.", {0, 0, result.modules[index].file});
            state[index] = 1;
            for (const auto& imported : result.modules[index].imports) {
                const auto target = moduleIndices.at(imported);
                if (state[target] == 1)
                    throw Diagnostic("E_IMPORT_CYCLE", "Ciclo de imports con " + imported + ".", importSpan(result.modules[index].file, imported));
                self(self, target);
            }
            state[index] = 2;
        };
        for (std::size_t i = 0; i < result.modules.size(); ++i) visit(visit, i);
        for (const auto& module : result.modules)
            if (module.package == packages.front().manifest.name) result.rootModules.push_back(module.name);
        result.entryMain = pathText(entry.stem()) + ".main";
    }
public:
    Loader(fs::path input, const SourceOverrides& replacements) : entry(fs::canonical(input)), overrides(replacements) {}
    Program load() {
        const auto sourceDirectory = entry.parent_path();
        const auto manifest = sourceDirectory.parent_path() / "veloz.pkg";
        if (sourceDirectory.filename() != "src" || !fs::is_regular_file(manifest)) {
            result.source = read(entry);
            auto single = parse(result.source);
            single.files.push_back({pathText(entry), result.source});
            if (!single.imports.empty())
                throw Diagnostic("E_PROJECT", "Los imports requieren un paquete con veloz.pkg y fuentes bajo src.", single.imports.front().span);
            return single;
        }
        auto error = captureDiagnostic([&] {
            loadPackage(sourceDirectory.parent_path());
            loadModules();
        });
        if (error) {
            if (error->file.empty() && error->span.file < result.files.size()) {
                error->file = result.files[error->span.file].path;
                error->source = result.files[error->span.file].text;
            }
            throw *error;
        }
        return std::move(result);
    }
};

}
namespace {
// Nombres de función que aparecen en llamadas; los de método se anotan aparte para los alias.
struct CallNames {
    std::set<std::string> calls, methods;
    void expr(const Expr& e) {
        if (e.kind == ExprKind::Call || e.kind == ExprKind::Name) calls.insert(e.text);
        if (e.kind == ExprKind::MethodCall) { calls.insert(e.text); methods.insert(e.text); }
        for (const auto& a : e.args) expr(a);
        if (e.closure) stmts(e.closure->body);
        if (e.matchExpr) {
            expr(e.matchExpr->scrutinee);
            for (const auto& arm : e.matchExpr->arms) {
                stmts(arm.body);
                if (arm.resultExpr) expr(*arm.resultExpr);
            }
        }
        if (e.ifExpr) {
            expr(e.ifExpr->condition);
            stmts(e.ifExpr->thenBody);
            stmts(e.ifExpr->elseBody);
            if (e.ifExpr->thenResult) expr(*e.ifExpr->thenResult);
            if (e.ifExpr->elseResult) expr(*e.ifExpr->elseResult);
        }
    }
    void stmts(const std::vector<Stmt>& body) {
        for (const auto& s : body) {
            for (const auto& e : s.exprs) expr(e);
            stmts(s.body);
            stmts(s.otherwise);
            for (const auto& arm : s.arms) {
                stmts(arm.body);
                if (arm.resultExpr) expr(*arm.resultExpr);
            }
        }
    }
    void function(const Function& fn) {
        stmts(fn.body);
        for (const auto& e : fn.requiresClauses) expr(e);
        for (const auto& e : fn.ensuresClauses) expr(e);
    }
};

// Método escrito por costumbre de otros lenguajes -> funciones de la biblioteca que lo implementan.
const std::vector<std::pair<std::string, std::vector<std::string>>>& preludeMethodAliases() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> aliases = {
        {"repeat", {"strRepeat"}}, {"reverse", {"strReverse", "reversed"}},
        {"max", {"arrayMax"}}, {"min", {"arrayMin"}}, {"sum", {"sum", "sumFloat"}},
        {"indexOf", {"indexOfInt", "indexOfStr"}}, {"contains", {"indexOfInt", "indexOfStr"}},
        {"includes", {"indexOfInt", "indexOfStr"}},
        {"toUpperCase", {"upper"}}, {"toUpper", {"upper"}}, {"to_uppercase", {"upper"}}, {"toUppercase", {"upper"}},
        {"toLowerCase", {"lower"}}, {"toLower", {"lower"}}, {"to_lowercase", {"lower"}}, {"toLowercase", {"lower"}},
        {"strip", {"trim"}}, {"count", {"countOf", "countIf"}},
    };
    return aliases;
}

// Añade al programa las funciones de la biblioteca base que nombra (y las que estas usan).
// Si no nombra ninguna, el programa queda exactamente como estaba.
void injectPrelude(Program& program) {
    if (strictSyntax()) return;
    CallNames used;
    for (const auto& fn : program.functions) used.function(fn);
    for (const auto& impl : program.impls)
        for (const auto& fn : impl.methods) used.function(fn);
    for (const auto& iface : program.interfaces)
        for (const auto& fn : iface.methods) used.function(fn);
    for (const auto& c : program.consts) used.expr(c.init);
    for (const auto& [method, targets] : preludeMethodAliases())
        if (used.methods.contains(method)) used.calls.insert(targets.begin(), targets.end());

    // Las tuplas son estructuras de la biblioteca: se conservan solo si el programa las nombra.
    bool usesTuples = used.calls.contains("tuple2") || used.calls.contains("tuple3") || used.calls.contains("tuple4");
    for (const auto& app : program.typeApps)
        if (app.baseName == "Tuple2" || app.baseName == "Tuple3" || app.baseName == "Tuple4") usesTuples = true;

    Program scratch = program;
    const std::size_t before = scratch.functions.size();
    const std::size_t dataBefore = scratch.data.size();
    scratch.files.push_back({"<biblioteca base>", turiPreludeSource()});
    parseInto(scratch, scratch.files.back().text, "", scratch.files.size() - 1);

    // Tipos recursivos: por cada tipo guardado en celda, dos funciones que pasan de `T?` al campo
    // en celda y al revés. El comprobador las llama solo donde los dos tipos se encuentran
    // (`insert(n.left, k)` con un parámetro `Tree?`); el programador no las ve.
    std::vector<std::string> cellHelpers;
    {
        std::string text;
        for (const auto& name : recursiveTypeNames(program)) {
            const bool plain = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
                return std::isalnum(ch) != 0;
            });
            if (!plain) continue;
            text += "fn tzCellGet" + name + "(o: Shared<" + name + ">?) -> " + name + "? {\n"
                    "    match o {\n        Some(c) => { return Some(sharedGet(c)) }\n        None => { return None }\n    }\n}\n"
                    "fn tzCellNew" + name + "(o: " + name + "?) -> Shared<" + name + ">? {\n"
                    "    match o {\n        Some(v) => { return Some(sharedNew(v)) }\n        None => { return None }\n    }\n}\n";
            cellHelpers.push_back("tzCellGet" + name);
            cellHelpers.push_back("tzCellNew" + name);
        }
        if (!text.empty()) {
            scratch.files.push_back({"<celdas>", std::move(text)});
            parseInto(scratch, scratch.files.back().text, "", scratch.files.size() - 1);
        }
    }

    std::set<std::string> userNames;
    for (std::size_t i = 0; i < before; ++i) userNames.insert(scratch.functions[i].sourceName);
    std::map<std::string, std::size_t> library;
    for (std::size_t i = before; i < scratch.functions.size(); ++i)
        if (!userNames.contains(scratch.functions[i].sourceName)) library.emplace(scratch.functions[i].sourceName, i);

    std::set<std::string> needed;
    std::vector<std::string> pending;
    for (const auto& name : used.calls)
        if (library.contains(name) && needed.insert(name).second) pending.push_back(name);
    for (const auto& name : cellHelpers)
        if (library.contains(name)) needed.insert(name);
    if (pending.empty() && !usesTuples && cellHelpers.empty()) return;
    while (!pending.empty()) {
        const std::string name = std::move(pending.back());
        pending.pop_back();
        CallNames inner;
        inner.function(scratch.functions[library.at(name)]);
        for (const auto& [method, targets] : preludeMethodAliases())
            if (inner.methods.contains(method)) inner.calls.insert(targets.begin(), targets.end());
        for (const auto& callee : inner.calls)
            if (library.contains(callee) && needed.insert(callee).second) pending.push_back(callee);
    }
    std::vector<Function> kept;
    kept.reserve(before + needed.size());
    for (std::size_t i = 0; i < scratch.functions.size(); ++i) {
        if (i < before || needed.contains(scratch.functions[i].sourceName)) kept.push_back(std::move(scratch.functions[i]));
    }
    scratch.functions = std::move(kept);
    if (!usesTuples) scratch.data.resize(dataBefore);
    program = std::move(scratch);
}
} // namespace

Program loadProject(const fs::path& entry, const SourceOverrides& overrides) {
    Program program = Loader(entry, overrides).load();
    injectPrelude(program);
    return program;
}
}
