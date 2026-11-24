#include "generics.hpp"
#include <algorithm>

namespace veloz {
namespace {
using Substitutions = std::map<Type, Type>;
Type substitute(Type type, const Substitutions& mapping, Program& program) {
    auto found = mapping.find(type);
    if (found != mapping.end()) return found->second;
    if (program.isArray(type)) return program.arrayOf(substitute(program.elemOf(type), mapping, program));
    if (const auto* app = program.typeAppOf(type)) {
        const std::string base = app->baseName;
        std::vector<Type> args = app->args;
        for (Type& a : args) a = substitute(a, mapping, program);
        return program.typeApp(base, args);
    }
    return type;
}
void substituteLocals(std::vector<Stmt>& statements, const Substitutions& mapping, Program& program);
void substituteExpr(Expr& e, const Substitutions& mapping, Program& program) {
    e.assocType = substitute(e.assocType, mapping, program);
    for (Type& t : e.typeArgs) t = substitute(t, mapping, program);
    for (auto& arg : e.args) substituteExpr(arg, mapping, program);
    if (e.closure) {
        for (auto& p : e.closure->params) p.type = substitute(p.type, mapping, program);
        e.closure->result = substitute(e.closure->result, mapping, program);
        substituteLocals(e.closure->body, mapping, program);
    }
    if (e.matchExpr) {
        substituteExpr(e.matchExpr->scrutinee, mapping, program);
        for (auto& arm : e.matchExpr->arms) {
            for (Type& t : arm.typeArgs) t = substitute(t, mapping, program);
            substituteLocals(arm.body, mapping, program);
            if (arm.resultExpr) substituteExpr(*arm.resultExpr, mapping, program);
        }
    }
    if (e.ifExpr) {
        substituteExpr(e.ifExpr->condition, mapping, program);
        substituteLocals(e.ifExpr->thenBody, mapping, program);
        if (e.ifExpr->thenResult) substituteExpr(*e.ifExpr->thenResult, mapping, program);
        substituteLocals(e.ifExpr->elseBody, mapping, program);
        if (e.ifExpr->elseResult) substituteExpr(*e.ifExpr->elseResult, mapping, program);
    }
}
void substituteLocals(std::vector<Stmt>& statements, const Substitutions& mapping, Program& program) {
    for (auto& s : statements) {
        s.annotation = substitute(s.annotation, mapping, program);
        for (auto& e : s.exprs) substituteExpr(e, mapping, program);
        substituteLocals(s.body, mapping, program);
        substituteLocals(s.otherwise, mapping, program);
        for (auto& arm : s.arms) {
            for (Type& t : arm.typeArgs) t = substitute(t, mapping, program);
            substituteLocals(arm.body, mapping, program);
            if (arm.resultExpr) substituteExpr(*arm.resultExpr, mapping, program);
        }
    }
}
bool mentionsType(Type container, Type target, const Program& program) {
    if (container == target) return true;
    if (program.isArray(container)) return mentionsType(program.elemOf(container), target, program);
    if (const auto* app = program.typeAppOf(container)) {
        for (Type a : app->args)
            if (mentionsType(a, target, program)) return true;
    }
    return false;
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
        if (f.name == program.entryMain) {
            Diagnostic error("E_MAIN", "main no admite parámetros genéricos.", f.span);
            error.example = "fn main() {}";
            throw error;
        }
        if (f.name == "len") invalid("Esta función no admite una plantilla", f.span);
        std::set<Type> seen;
        for (Type t : f.generics) {
            if (!seen.insert(t).second || program.declaration(t))
                invalid("El parámetro de tipo debe tener un nombre propio en su firma", f.span);
            if (std::none_of(f.params.begin(), f.params.end(), [t, &input](const Param& p){ return mentionsType(p.type, t, input); }))
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
            program.data.push_back({unique, false, f.span, {{display, f.span, {}}}, f.module});
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
    copy.isSpecialization = true;
    copy.substitutedTypes = concrete;
    copy.methodConstraints.clear();
    for (std::size_t i = 0; i < f.constraints.size(); ++i)
        if (f.constraints[i] != Type::Unknown) copy.methodConstraints.emplace_back(concrete[i], f.constraints[i]);
    for (auto& p : copy.params) p.type = substitute(p.type, mapping, program);
    copy.result = substitute(copy.result, mapping, program);
    for (auto& req : copy.requiresClauses) substituteExpr(req, mapping, program);
    for (auto& ens : copy.ensuresClauses) substituteExpr(ens, mapping, program);
    substituteLocals(copy.body, mapping, program);
    return copy;
}
const Function& GenericInstances::resolve(Expr& call, const std::vector<Type>& arguments,
                                        const std::function<bool(Type, Type, Span)>& ensureImpl) {
    const auto& f = templates.at(call.text);
    if (arguments.size() != f.params.size()) {
        Diagnostic d("E_ARITY", "Cantidad incorrecta de argumentos de " + f.name, call.span);
        d.expected = std::to_string(f.params.size());
        d.actual = std::to_string(arguments.size());
        d.related.push_back(f.span);
        throw d;
    }
    std::map<Type, Type> deduced;
    for (Type t : f.generics) deduced.emplace(t, Type::Unknown);
    if (!call.typeArgs.empty()) {
        if (call.typeArgs.size() != f.generics.size()) {
            Diagnostic d("E_GENERIC", "Cantidad incorrecta de argumentos de tipo para " + f.name, call.span);
            d.expected = std::to_string(f.generics.size());
            d.actual = std::to_string(call.typeArgs.size());
            d.related.push_back(f.span);
            throw d;
        }
        for (std::size_t k = 0; k < f.generics.size(); ++k) deduced[f.generics[k]] = call.typeArgs[k];
    }
    auto unify = [&](auto& self, Type paramT, Type actualT, Span argSpan, Span paramSpan) -> void {
        if (auto it = deduced.find(paramT); it != deduced.end()) {
            if (it->second != Type::Unknown && it->second != actualT) {
                Diagnostic d("E_TYPE", "Los argumentos del mismo parámetro de tipo deben coincidir", argSpan);
                d.expected = program.nameOf(it->second);
                d.actual = program.nameOf(actualT);
                d.related.push_back(paramSpan);
                throw d;
            }
            it->second = actualT;
            return;
        }
        if (program.isArray(paramT)) {
            if (!program.isArray(actualT)) {
                Diagnostic d("E_TYPE", "Se esperaba un array para deducir el parámetro genérico", argSpan);
                d.expected = program.nameOf(paramT);
                d.actual = program.nameOf(actualT);
                d.related.push_back(paramSpan);
                throw d;
            }
            self(self, program.elemOf(paramT), program.elemOf(actualT), argSpan, paramSpan);
            return;
        }
        if (const auto* pApp = program.typeAppOf(paramT)) {
            const auto* aApp = program.typeAppOf(actualT);
            if (!aApp || aApp->baseName != pApp->baseName || aApp->args.size() != pApp->args.size()) {
                Diagnostic d("E_TYPE", "No coincide el tipo parametrizado para deducir el parámetro genérico", argSpan);
                d.expected = program.nameOf(paramT);
                d.actual = program.nameOf(actualT);
                d.related.push_back(paramSpan);
                throw d;
            }
            for (std::size_t k = 0; k < pApp->args.size(); ++k)
                self(self, pApp->args[k], aApp->args[k], argSpan, paramSpan);
        }
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        unify(unify, f.params[i].type, arguments[i], call.args[i].span, f.params[i].span);
    }
    std::vector<Type> concrete;
    concrete.reserve(f.generics.size());
    for (Type t : f.generics) concrete.push_back(deduced.at(t));
    for (std::size_t i = 0; i < f.constraints.size(); ++i) {
        if (f.constraints[i] == Type::Unknown || opaque.contains(concrete[i])) continue;
        const bool implemented = ensureImpl
            ? ensureImpl(f.constraints[i], concrete[i], call.span)
            : std::any_of(program.impls.begin(), program.impls.end(), [&](const ImplDecl& impl) {
                  return impl.interface == f.constraints[i] && impl.target == concrete[i];
              });
        if (!implemented) {
            Diagnostic d("E_IMPL_MISSING", "Falta impl " + program.nameOf(f.constraints[i]) + " for " +
                         program.nameOf(concrete[i]), call.span);
            d.related.push_back(f.span);
            throw d;
        }
    }
    const auto& specialized = instance(f, concrete);
    call.text = specialized.name;
    return specialized;
}
void GenericInstances::finish() {
    auto mentionsOpaque = [&](auto& self, Type t) -> bool {
        if (opaque.contains(t)) return true;
        if (program.isArray(t)) return self(self, program.elemOf(t));
        if (const auto* app = program.typeAppOf(t)) {
            for (Type a : app->args) if (self(self, a)) return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < pending.size(); ++i)
        if (!abstractInstances.contains(i)) program.functions.push_back(std::move(pending[i]));
    std::erase_if(program.data, [&](const DataDecl& d){ return mentionsOpaque(mentionsOpaque, d.type); });
}
}
