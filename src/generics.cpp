#include "generics.hpp"
#include <algorithm>

namespace veloz {
namespace {
using Substitutions = std::map<Type, Type>;
Type substitute(Type type, const Substitutions& mapping) {
    auto found = mapping.find(type);
    return found == mapping.end() ? type : found->second;
}
void substituteLocals(std::vector<Stmt>& statements, const Substitutions& mapping) {
    for (auto& s : statements) {
        s.annotation = substitute(s.annotation, mapping);
        substituteLocals(s.body, mapping);
        substituteLocals(s.otherwise, mapping);
        for (auto& arm : s.arms) substituteLocals(arm.body, mapping);
    }
}
[[noreturn]] void invalid(std::string message, Span span) {
    Diagnostic d("E_GENERIC", std::move(message), span);
    d.example = "fn identity<T>(value: T) -> T { return value }";
    throw d;
}
}
GenericInstances::GenericInstances(Program& input) : program(input) {
    std::map<std::string, Span> names;
    for (const auto& f : program.functions) {
        if (f.isTest) continue;
        auto [previous, unique] = names.emplace(f.name, f.span);
        if (!unique) {
            Diagnostic d("E_DUP_FUNCTION", "Función duplicada: " + f.name, f.span);
            d.related.push_back(previous->second);
            throw d;
        }
        if (f.generics.empty()) continue;
        if (f.name == "len" || f.name == "main")
            invalid("Esta función no admite una plantilla", f.span);
        std::set<Type> seen;
        for (Type t : f.generics) {
            if (!seen.insert(t).second || program.declaration(t))
                invalid("El parámetro de tipo debe tener un nombre propio en su firma", f.span);
            if (std::none_of(f.params.begin(), f.params.end(), [t](const Param& p){ return p.type == t; }))
                invalid("El argumento de tipo debe poder inferirse desde un parámetro", f.span);
        }
    }
    auto it = std::remove_if(program.functions.begin(), program.functions.end(), [&](Function& f) {
        if (f.generics.empty()) return false;
        std::string name = f.name;
        templates.emplace(std::move(name), std::move(f));
        return true;
    });
    program.functions.erase(it, program.functions.end());
    for (const auto& [name, f] : templates) {
        std::vector<Type> types;
        for (Type t : f.generics) {
            auto display = program.nameOf(t) + "@" + name;
            auto unique = program.intern(display);
            opaque.insert(unique);
            program.data.push_back({unique, false, f.span, {{display, f.span, {}}}});
            types.push_back(unique);
        }
        instance(f, types);
    }
}
Function& GenericInstances::instance(const Function& f, const std::vector<Type>& concrete) {
    const auto key = std::make_pair(f.name, concrete);
    if (auto it = cache.find(key); it != cache.end()) return pending[it->second];
    if (pending.size() >= 4096) invalid("Se superó el límite de 4096 especializaciones", f.span);
    auto index = pending.size();
    cache.emplace(key, index);
    Substitutions mapping;
    for (std::size_t i = 0; i < concrete.size(); ++i) {
        mapping.emplace(f.generics[i], concrete[i]);
        if (opaque.contains(concrete[i])) abstractInstances.insert(index);
    }
    auto& copy = pending.emplace_back(f);
    copy.name += ".g" + std::to_string(index); // '.' is unavailable inside a source identifier
    copy.generics.clear();
    for (auto& p : copy.params) p.type = substitute(p.type, mapping);
    copy.result = substitute(copy.result, mapping);
    substituteLocals(copy.body, mapping);
    return copy;
}
const Function& GenericInstances::resolve(Expr& call, const std::vector<Type>& arguments) {
    const auto& f = templates.at(call.text);
    if (arguments.size() != f.params.size()) {
        Diagnostic d("E_ARITY", "Cantidad incorrecta de argumentos de " + f.name, call.span);
        d.expected = std::to_string(f.params.size());
        d.actual = std::to_string(arguments.size());
        d.related.push_back(f.span);
        throw d;
    }
    std::vector<Type> concrete;
    for (Type t : f.generics) {
        Type selected = Type::Unknown;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (f.params[i].type != t) continue;
            if (selected != Type::Unknown && selected != arguments[i]) {
                Diagnostic d("E_TYPE", "Los argumentos del mismo parámetro de tipo deben coincidir", call.args[i].span);
                d.expected = program.nameOf(selected);
                d.actual = program.nameOf(arguments[i]);
                d.related.push_back(f.params[i].span);
                throw d;
            }
            selected = arguments[i];
        }
        concrete.push_back(selected);
    }
    const auto& specialized = instance(f, concrete);
    call.text = specialized.name;
    return specialized;
}
void GenericInstances::finish() {
    for (std::size_t i = 0; i < pending.size(); ++i)
        if (!abstractInstances.contains(i)) program.functions.push_back(std::move(pending[i]));
    std::erase_if(program.data, [&](const DataDecl& d){ return opaque.contains(d.type); });
}
}
