#include "commands.hpp"
#include <algorithm>
#include <charconv>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace veloz {
namespace {
std::string fingerprint(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
std::string docLine(const std::string& source, Span span) {
    auto at = source.rfind('\n', span.start);
    if (at == source.npos || !at) return {};
    auto before = source.rfind('\n', at - 1);
    std::string previous = source.substr(before == source.npos ? 0 : before+1,
                                         before == source.npos ? at : at-before-1);
    auto i = previous.find_first_not_of(" \t");
    if (i == previous.npos || previous.compare(i, 2, "//")) return {};
    i = previous.find_first_not_of("/ \t", i);
    if (i == previous.npos) return {};
    return previous.substr(i, previous.find_last_not_of(" \r\t") - i + 1);
}
template<class Statement, class Expression>
void walk(const std::vector<Stmt>& body, Statement& visit, Expression& seeExpr) {
    auto expression = [&](auto& self, const Expr& e) -> void {
        seeExpr(e);
        for (const auto& arg : e.args) self(self, arg);
    };
    for (const auto& stmt : body) {
        visit(stmt);
        for (const auto& e : stmt.exprs) expression(expression, e);
        walk(stmt.body, visit, seeExpr);
        walk(stmt.otherwise, visit, seeExpr);
        for (const auto& arm : stmt.arms) walk(arm.body, visit, seeExpr);
    }
}
struct Effects { bool io = false, trap = false; std::set<std::string> calls; };
std::map<std::string, Effects> effects(const Program& p) {
    std::map<std::string, Effects> result;
    for (const auto& f : p.functions) {
        if (f.isTest) continue;
        auto& fx = result[f.name];
        auto visit = [&](const Stmt& s) {
            fx.io |= s.kind == StmtKind::Print;
            fx.trap |= s.kind == StmtKind::Assert;
        };
        auto expr = [&](const Expr& e) {
            if (e.kind == ExprKind::Call && e.text != "len") {
                fx.calls.insert(e.text);
                fx.trap = true; // Llamar puede agotar la profundidad.
            }
            fx.trap |= (e.kind == ExprKind::Binary && std::string_view("+-*/%").find(e.text) != std::string_view::npos)
                         || (e.kind == ExprKind::Unary && e.text == "-");
        };
        walk(f.body, visit, expr);
    }
    bool changed;
    do {
        changed = false;
        for (auto& [_, fx] : result) {
            for (const auto& callee : fx.calls) {
                const auto& other = result.at(callee);
                if (!fx.io && other.io) { fx.io = true; changed = true; }
                if (!fx.trap && other.trap) { fx.trap = true; changed = true; }
            }
        }
    } while (changed);
    return result;
}
std::string signature(const Program& p, const Function& f) {
    std::string result = "fn " + f.name;
    if (!f.generics.empty()) {
        result += "<";
        for (size_t i = 0; i < f.generics.size(); ++i) {
            if (i) result += ", ";
            result += p.nameOf(f.generics[i]);
        }
        result += ">";
    }
    result += "(";
    for (size_t i = 0; i < f.params.size(); ++i) {
        if (i) result += ", ";
        result += f.params[i].name + ": " + p.nameOf(f.params[i].type);
    }
    result += ")";
    if (f.result != Type::Void) result += " -> " + p.nameOf(f.result);
    return result;
}
// Serialize concurrent veloz edits. A failed precondition leaves the original intact.
struct EditLock {
    fs::path path;
    explicit EditLock(const fs::path& file) : path(fs::canonical(file)) {
        path += ".veloz-lock";
        if (!fs::create_directory(path))
            throw Diagnostic("E_BUSY", "Ya hay una edición de este archivo en curso", {});
    }
    ~EditLock() { std::error_code ignored; fs::remove(path, ignored); }
};
void publishSource(const fs::path& file, const std::string& original, std::string_view replacement) {
    EditLock lock(file);
    if (readFile(file) != original)
        throw Diagnostic("E_STALE", "La fuente cambió durante la edición. Vuelve a leer su firma.", {});
    replaceFile(fs::canonical(file), replacement);
}
const Function& symbol(const Program& p, const std::string& name) {
    for (const auto& f : p.functions) if (!f.isTest && f.name == name) return f;
    throw Diagnostic("E_SYMBOL", "No existe la función " + name, {});
}
std::string applyPatch(const std::vector<std::string>& args, const fs::path& file,
                       const std::string& source, const Program& original) {
    if (args.size() != 5) throw Diagnostic("E_USAGE", "veloz patch ARCHIVO FUNCIÓN HASH REEMPLAZO", {});
    const auto& f = symbol(original, args[2]);
    auto bytes = std::string_view(source).substr(f.span.start, f.span.end - f.span.start);
    if (fingerprint(bytes) != args[3])
        throw Diagnostic("E_STALE", "El hash cambió. Consulta veloz sig.", f.span);
    auto replacement = readFile(pathFrom(args[4]));
    auto parsed = parse(replacement);
    if (!parsed.data.empty() || parsed.functions.size() != 1 || parsed.functions[0].isTest ||
        parsed.functions[0].name != f.name)
        throw Diagnostic("E_PATCH", "El reemplazo debe contener una función con el mismo nombre", f.span);
    auto span = parsed.functions[0].span;
    auto updated = source.substr(0, f.span.start) + replacement.substr(span.start, span.end - span.start)
                 + source.substr(f.span.end);
    auto candidate = parse(updated);
    check(candidate);
    // A failing test must not publish a partially repaired edit.
    struct Sink : std::streambuf {
        int_type overflow(int_type c) override { return traits_type::not_eof(c); }
        std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
    } sink;
    std::ostream quiet(&sink);
    runTests(candidate, quiet);
    publishSource(file, source, updated);
    return {};
}
struct Page {
    std::size_t offset, seen = 0;
    std::string text;
    std::optional<std::size_t> next;
    void add(std::string line) {
        auto i = seen++;
        if (i < offset || next) return;
        if (!text.empty() && (text.size() + line.size() > 7168 || i - offset >= 60)) { next = i; return; }
        text += line + "\n";
    }
    std::string finish() {
        if (next) text += "{\"nextOffset\":" + std::to_string(*next) + "}\n";
        return text;
    }
};
}
std::string agentCommand(const std::vector<std::string>& args, const fs::path& file,
                         const std::string& source, Program& program) {
    auto& command = args[0];
    if (command == "fmt") {
        bool verify = args.size() == 3 && args[2] == "--check";
        if (!verify && args.size() != 2) throw Diagnostic("E_USAGE", "veloz fmt ARCHIVO [--check]", {});
        auto formatted = formatSource(source);
        if (formatted != source) {
            if (verify) throw Diagnostic("E_FORMAT", "Ejecuta veloz fmt para normalizar el formato", {});
            parse(formatted);
            publishSource(file, source, formatted);
        }
        return {};
    }
    if (command == "patch") return applyPatch(args, file, source, program);
    auto checked = program;
    check(checked);
    std::string filter;
    std::size_t arg = 2, offset = 0;
    if (arg < args.size() && args[arg] != "--offset") filter = args[arg++];
    if (arg < args.size()) {
        if (arg+2 != args.size() || args[arg] != "--offset") throw Diagnostic("E_USAGE", "Usa [FUNCIÓN] [--offset N]", {});
        auto& value = args[arg+1];
        auto [end, ec] = std::from_chars(value.data(), value.data()+value.size(), offset);
        if (ec != std::errc{} || end != value.data()+value.size()) throw Diagnostic("E_USAGE", "Offset inválido", {});
    }
    auto namesType = [&](const DataDecl& t){ return program.nameOf(t.type) == filter; };
    if (!filter.empty() && (command != "sig" || std::none_of(program.data.begin(), program.data.end(), namesType)))
        symbol(program, filter);
    Page page{offset, 0, {}, {}};
    if (command == "sig") {
        auto summary = effects(program);
        for (const auto& d : program.data) {
            if (!filter.empty() && !namesType(d)) continue;
            // All fields and payloads are part of the module's public signature.
            auto view = std::string_view(source).substr(d.span.start, d.span.end - d.span.start);
            page.add("{\"name\":" + jsonString(program.nameOf(d.type)) + ",\"signature\":" + jsonString(view) +
                     ",\"doc\":" + jsonString(docLine(source, d.span)) + "}");
        }
        for (const auto& f : program.functions) {
            if (f.isTest || (!filter.empty() && f.name != filter)) continue;
            const auto& fx = summary.at(f.name);
            auto hash = fingerprint(std::string_view(source).substr(f.span.start, f.span.end-f.span.start));
            page.add("{\"name\":" + jsonString(f.name) + ",\"signature\":" + jsonString(signature(program, f)) +
                ",\"doc\":" + jsonString(docLine(source, f.span)) + ",\"io\":" + (fx.io ? "true" : "false") +
                ",\"mayTrap\":" + (fx.trap ? "true" : "false") + ",\"hash\":" + jsonString(hash) + "}");
        }
    } else {
        for (const auto& f : checked.functions) {
            if (!filter.empty() && (f.isTest || (f.name != filter && !f.name.starts_with(filter+".g")))) continue;
            auto emit = [&](Span s, Type t) {
                page.add("{\"function\":" + jsonString(f.name) + ",\"start\":" + std::to_string(s.start) +
                    ",\"end\":" + std::to_string(s.end) + ",\"type\":" + jsonString(checked.nameOf(t)) + "}");
            };
            for (const auto& p : f.params) emit(p.span, p.type);
            auto visit = [](const Stmt&) {};
            auto expr = [&](const Expr& e){ emit(e.span, e.type); };
            walk(f.body, visit, expr);
        }
    }
    return page.finish();
}
std::string explain(std::string_view topic) {
    static const std::map<std::string_view, std::string_view> index{
        {"E_TYPE","Comprueba el tipo esperado, el obtenido y el tramo relacionado. No hay conversiones implícitas."},
        {"E_NAME","Declara el nombre en el ámbito actual o en uno exterior. Los valores usan camelCase ASCII."},
        {"E_PARSE","Revisa expected, actual y span. Separa las sentencias con un salto de línea."},
        {"E_IMMUTABLE","Declara con var si necesitas reasignar. Los parámetros y el índice de for son de solo lectura."},
        {"E_LET_MUT","Sustituye let mut por var. La edición exacta va en fix."},
        {"E_LENGTH","len(texto) devuelve el número de bytes UTF-8."},
        {"E_SEMICOLON","Separa dos sentencias con un salto de línea. Elimina el punto y coma final."},
        {"E_PRINTLN","print(valor) escribe el valor y un salto de línea."},
        {"E_ELIF","Escribe } else if condición { en la misma línea."},
        {"E_RETURN","Cada camino de una función con resultado debe devolver un valor del tipo declarado."},
        {"E_GENERIC","Declara fn id<T>(x: T) -> T. Cada tipo genérico se deduce de los argumentos y debe funcionar como tipo opaco."},
        {"E_EXHAUSTIVE","Incluye un brazo por variante del enum. El compilador comprueba la cobertura incluso si el valor parece constante."},
        {"E_LAYOUT","Reduce la profundidad o el tamaño de los tipos contenidos por valor."},
        {"E_TYPE_CYCLE","Un ciclo de campos por valor necesitaría tamaño infinito. Usa una representación sin el ciclo."},
        {"R_STEPS","El intérprete agotó su presupuesto. veloz run archivo --steps N permite cambiarlo."},
        {"R_DEPTH","La llamada superó 256 marcos activos. Reescribe la recursión como bucle."},
        {"R_OVERFLOW","El resultado matemático queda fuera de int64. Comprueba el rango antes de la operación."},
        {"R_DIV_ZERO","El segundo operando de / o % debe ser distinto de cero."},
        {"R_ASSERT","La expresión de assert dio false. Reproduce el caso usando veloz check o veloz run."},
        {"E_STALE","Obtén el hash actual con veloz sig y revisa la nueva versión antes de editar."}
    };
    if (auto found = index.find(topic); found != index.end()) return std::string(found->second);
    throw Diagnostic("E_TOPIC", "No hay una explicación registrada para " + std::string(topic), {});
}
}
