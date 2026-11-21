#include "veloz.hpp"
#include "generics.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <ostream>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>

namespace veloz {
namespace {

using Integer = std::int64_t;
constexpr auto minInt = std::numeric_limits<Integer>::min();
constexpr auto maxInt = std::numeric_limits<Integer>::max();

// The preconditions guard the C++ operation itself, including MSVC builds.
std::optional<Integer> checkedAdd(Integer a, Integer b) {
    if ((b > 0 && a > maxInt - b) || (b < 0 && a < minInt - b)) return {};
    return a + b;
}

std::optional<Integer> checkedSub(Integer a, Integer b) {
    if ((b > 0 && a < minInt + b) || (b < 0 && a > maxInt + b)) return {};
    return a - b;
}

std::optional<Integer> checkedMul(Integer a, Integer b) {
    if (a > 0 && ((b > 0 && a > maxInt / b) || (b < 0 && b < minInt / a))) return {};
    if (a < 0 && ((b > 0 && a < minInt / b) || (b < 0 && a < maxInt / b))) return {};
    return a * b;
}

template<class Binding>
struct Scopes {
    using Scope = std::unordered_map<std::string, Binding>;
    std::vector<Scope> levels{1};

    Binding* find(const std::string& name) {
        for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
            if (auto value = it->find(name); value != it->end()) return &value->second;
        }
        return nullptr;
    }
};

using Functions = std::unordered_map<std::string, const Function*>;

[[noreturn]] void fail(std::string id, std::string message, Span span, std::string example,
                       std::optional<Span> related = {}) {
    Diagnostic error(std::move(id), std::move(message), span);
    error.example = std::move(example);
    if (related) error.related.push_back(*related);
    throw error;
}

void checkTypes(const Program& program, Type actual, Type expected, Span span, std::optional<Span> related = {}) {
    if (actual == expected) return;
    Diagnostic error("E_TYPE", "Tipo incompatible", span);
    error.actual = program.nameOf(actual);
    error.expected = program.nameOf(expected);
    if (related) error.related.push_back(*related);
    switch (expected) {
        case Type::Int: error.example = "let n: int = 1"; break;
        case Type::Bool: error.example = "let b: bool = true"; break;
        case Type::Str: error.example = R"(let s: str = "hola")"; break;
        case Type::Void: error.example = "fn done() { return }"; break;
        default: error.example = "fn identity(x: " + error.expected + ") -> " + error.expected + " { return x }";
    }
    throw error;
}

void requireValue(Type type, Span where) {
    if (type != Type::Void) return;
    Diagnostic error("E_VALUE", "Se necesita un valor", where);
    error.actual = "void";
    error.expected = "int | bool | str";
    error.example = "fn one() -> int { return 1 }";
    throw error;
}

Integer parseInteger(const Expr& expression) {
    const auto& text = expression.text;
    Integer result{};
    const auto end = text.data() + text.size();
    const auto [cursor, ec] = std::from_chars(text.data(), end, result);
    if (ec != std::errc{} || cursor != end)
        fail("E_INT_RANGE", "Literal fuera del rango de int", expression.span, "9223372036854775807");
    return result;
}

struct Variable {
    Type type;
    bool writable;
    Span declared;
};

struct Constructor {
    Type type;
    const Variant* variant;
    bool isEnum;
};

struct Types {
    const Program& program;
    std::unordered_map<Type, const DataDecl*> declarations;
    std::unordered_map<std::string, Constructor> constructors;

    explicit Types(const Program& p) : program(p) {
        for (const auto& d : p.data) {
            const auto name = p.nameOf(d.type);
            const auto [existing, added] = declarations.emplace(d.type, &d);
            if (!added) fail("E_DUP_TYPE", "Tipo duplicado: " + name, d.span,
                             "struct Pair { first: int }", existing->second->span);
            if (d.isEnum && d.variants.empty())
                fail("E_ENUM_EMPTY", "El enum requiere una variante", d.span, "enum State { Ready }");
            for (const auto& v : d.variants) {
                const auto full = d.isEnum ? name + "." + v.name : name;
                const auto [seen, inserted] = constructors.emplace(full, Constructor{d.type, &v, d.isEnum});
                if (!inserted) fail("E_DUP_VARIANT", "Variante duplicada: " + full, v.span,
                                    "enum State { Ready }", seen->second.variant->span);
                std::unordered_map<std::string, Span> fieldNames;
                for (const auto& f : v.fields) {
                    const auto [original, unique] = fieldNames.emplace(f.name, f.span);
                    if (!unique) fail("E_DUP_FIELD", "Campo duplicado: " + f.name, f.span,
                                      "struct Point { x: int }", original->second);
                }
            }
        }
    }

    void known(Type type, Span where, bool allowVoid = false) const {
        if (type == Type::Int || type == Type::Bool || type == Type::Str) return;
        if (type == Type::Void) {
            if (!allowVoid) requireValue(type, where);
            return;
        }
        if (!declarations.contains(type))
            fail("E_UNKNOWN_TYPE", "Tipo desconocido: " + program.nameOf(type), where, "struct Point { x: int }");
    }

    const Constructor& constructor(const std::string& name, Span span) const {
        const auto it = constructors.find(name);
        if (it == constructors.end())
            fail("E_CONSTRUCTOR", "Constructor desconocido: " + name, span, "struct Point { x: int }");
        return it->second;
    }
};

// Conservative upper bounds include every variant of the native representation.
// The cached height prevents the result from depending on declaration order.
struct Layouts {
    const Types& types;
    struct Size { std::size_t bytes, height; };
    std::unordered_map<Type, Size> sizes;
    std::unordered_set<Type> path;
    Size measure(Type type, Span where) {
        types.known(type, where);
        if (type < Type::NamedBase) return {16, 0};
        if (path.contains(type)) fail("E_TYPE_CYCLE", "Ciclo de campos por valor", where, "struct Leaf { n: int }");
        if (path.size() >= 32) fail("E_LAYOUT", "Tipo con más de 32 niveles", where, "struct Leaf { n: int }");
        if (auto it = sizes.find(type); it != sizes.end()) return it->second;
        path.insert(type);
        const auto d = types.declarations.at(type);
        Size result{16, 1};
        for (const auto& v : d->variants) {
            if (d->isEnum) result.bytes += 16;
            if (result.bytes > 4096) fail("E_LAYOUT", "Disposición del tipo supera el presupuesto de 4096 bytes",
                                          v.span, "enum State { Ready }");
            for (const auto& f : v.fields) {
                const auto child = measure(f.type, f.span);
                result.height = std::max(result.height, 1 + child.height);
                result.bytes += child.bytes;
                if (result.height > 32 || result.bytes > 4096)
                    fail("E_LAYOUT", "Tipo fuera de los límites de 32 niveles y 4096 bytes de disposición",
                         f.span, "struct Pair { n: int }");
            }
        }
        path.erase(type);
        sizes.emplace(type, result);
        return result;
    }
};

std::size_t fieldIndex(const Variant& variant, const std::string& name, Span where) {
    for (std::size_t i = 0; i < variant.fields.size(); ++i)
        if (variant.fields[i].name == name) return i;
    fail("E_FIELD", "Campo desconocido: " + name, where, "struct Point { x: int }", variant.span);
}

struct Checker {
    const Functions& functions;
    const Function& current;
    Scopes<Variable> locals;
    const Types& types;
    GenericInstances& generics;
    const std::unordered_map<Type, std::size_t>& sizes;
    std::size_t frameCost = 256;

    void reserveValue(Type type, Span where) {
        if (current.isTest || type == Type::Void) return;
        // Budget all source values as if simultaneously live. Include a copy
        // for passing or materializing each value. The driver reserves extra
        // space for the machine ABI, registers and runtime calls.
        auto size = sizes.find(type);
        auto bytes = size != sizes.end() ? size->second : 16;
        frameCost += 2 * bytes;
        if (frameCost > 16 * 1024)
            fail("E_FRAME", "La función supera el presupuesto de 16384 bytes para parámetros, locales y temporales",
                 where, "fn first(x: int) -> int { return x }", current.span);
    }

    void requireType(Type actual, Type expected, Span span, std::optional<Span> related = {}) {
        checkTypes(types.program, actual, expected, span, related);
    }

    void declare(const std::string& name, Type type, bool writable, Span span) {
        const auto [it, added] = locals.levels.back().emplace(name, Variable{type, writable, span});
        if (!added) fail("E_DUP_NAME", "Nombre duplicado en este bloque: " + name,
                         span, "let other = 1", it->second.declared);
        reserveValue(type, span);
    }

    Variable& lookup(const std::string& name, Span span) {
        auto* value = locals.find(name);
        if (!value) fail("E_NAME", "Nombre desconocido: " + name, span, "let total = 0");
        return *value;
    }

    void checkCallArity(const Expr& expr, std::size_t expected, std::optional<Span> callee = {}) {
        if (expr.args.size() == expected) return;
        Diagnostic error("E_ARITY", "Cantidad incorrecta de argumentos de " + expr.text, expr.span);
        error.expected = std::to_string(expected);
        error.actual = std::to_string(expr.args.size());
        if (callee) error.related.push_back(*callee);
        error.example = R"(len("abc"))";
        throw error;
    }

    Type inferCall(Expr& expr) {
        if (expr.text == "len") {
            checkCallArity(expr, 1);
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            return Type::Int;
        }
        if (generics.contains(expr.text)) {
            std::vector<Type> actual;
            for (auto& arg : expr.args) actual.push_back(typeOf(arg));
            const auto& callee = generics.resolve(expr, actual);
            checkCallArity(expr, callee.params.size(), callee.span);
            for (std::size_t i = 0; i < actual.size(); ++i)
                requireType(actual[i], callee.params[i].type, expr.args[i].span, callee.params[i].span);
            return callee.result;
        }
        const auto it = functions.find(expr.text);
        if (it == functions.end())
            fail("E_FUNCTION", "Función desconocida: " + expr.text, expr.span, "fn one() -> int { return 1 }");
        const Function& callee = *it->second;
        checkCallArity(expr, callee.params.size(), callee.span);
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            requireType(typeOf(expr.args[i]), callee.params[i].type, expr.args[i].span, callee.params[i].span);
        }
        return callee.result;
    }

    Type inferBinary(Expr& expr) {
        Expr& left = expr.args[0];
        Expr& right = expr.args[1];
        const auto leftType = typeOf(left);
        if (expr.text == "==" || expr.text == "!=") {
            requireValue(leftType, left.span);
            if (leftType >= Type::NamedBase)
                fail("E_EQUALITY", "El agregado se compara por sus campos escalares", left.span, "a.x == b.x");
            requireType(typeOf(right), leftType, right.span, left.span);
            return Type::Bool;
        }
        const bool logic = expr.text == "&&" || expr.text == "||";
        const auto operand = logic ? Type::Bool : Type::Int;
        requireType(leftType, operand, left.span);
        requireType(typeOf(right), operand, right.span, left.span);
        if (logic || expr.text == "<" || expr.text == "<=" || expr.text == ">" || expr.text == ">=")
            return Type::Bool;
        return Type::Int;
    }

    Type construct(Expr& expr) {
        const auto& c = types.constructor(expr.text, expr.span);
        const auto& fields = c.variant->fields;
        checkCallArity(expr, fields.size(), c.variant->span);
        std::unordered_set<std::size_t> used;
        expr.fieldIndices.clear();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto index = c.isEnum ? i : fieldIndex(*c.variant, expr.labels.at(i), expr.args[i].span);
            if (!used.insert(index).second)
                fail("E_DUP_FIELD", "El constructor repite el campo " + fields[index].name, expr.args[i].span,
                     "Point { x: 1 }", fields[index].span);
            requireType(typeOf(expr.args[i]), fields[index].type, expr.args[i].span, fields[index].span);
            expr.fieldIndices.push_back(index);
        }
        return c.type;
    }

    Type field(Expr& expr) {
        auto& base = expr.args[0];
        const auto type = typeOf(base);
        if (type == Type::Str && expr.text == "length") {
            Diagnostic e("E_LENGTH", "La longitud de texto se obtiene con len", expr.span);
            e.example = R"(len("hola"))";
            if (base.span.end <= types.program.source.size()) {
                auto text = types.program.source.substr(base.span.start, base.span.end - base.span.start);
                e.fix = Edit{expr.span, "len(" + text + ")"};
            }
            throw e;
        }
        const auto it = types.declarations.find(type);
        if (it == types.declarations.end() || it->second->isEnum)
            fail("E_FIELD", "El acceso a campos requiere un struct", expr.span, "struct Point { x: int }");
        const auto& v = it->second->variants.at(0);
        const auto index = fieldIndex(v, expr.text, expr.span);
        expr.fieldIndices = {index};
        return v.fields[index].type;
    }

    Type typeOf(Expr& expr) {
        switch (expr.kind) {
            case ExprKind::Integer: parseInteger(expr); expr.type = Type::Int; break;
            case ExprKind::Boolean: expr.type = Type::Bool; break;
            case ExprKind::String: expr.type = Type::Str; break;
            case ExprKind::Name: expr.type = lookup(expr.text, expr.span).type; break;
            case ExprKind::Call: expr.type = inferCall(expr); break;
            case ExprKind::Unary: {
                auto type = expr.text == "!" ? Type::Bool : Type::Int;
                requireType(typeOf(expr.args[0]), type, expr.args[0].span);
                expr.type = type;
                break;
            }
            case ExprKind::Binary: expr.type = inferBinary(expr); break;
            case ExprKind::Field: expr.type = field(expr); break;
            case ExprKind::Construct: expr.type = construct(expr); break;
        }
        reserveValue(expr.type, expr.span);
        return expr.type;
    }

    bool block(std::vector<Stmt>& statements, bool nested = true) {
        if (nested) locals.levels.emplace_back();
        bool returns = false;
        for (auto& statement : statements) {
            if (checkStatement(statement)) returns = true;
        }
        if (nested) locals.levels.pop_back();
        return returns;
    }

    bool match(Stmt& s) {
        const auto type = typeOf(s.exprs[0]);
        auto it = types.declarations.find(type);
        if (it == types.declarations.end() || !it->second->isEnum)
            fail("E_MATCH_TYPE", "match requiere un enum", s.exprs[0].span, "enum State { Ready }");
        const auto& d = *it->second;
        std::unordered_map<std::string, Span> seen;
        bool returns = true;
        for (auto& arm : s.arms) {
            const auto& c = types.constructor(arm.variant, arm.span);
            requireType(c.type, type, arm.span, s.exprs[0].span);
            const auto [prior, fresh] = seen.emplace(arm.variant, arm.span);
            if (!fresh) fail("E_DUP_ARM", "Brazo match repetido: " + arm.variant, arm.span,
                             "match s { State.Ready => return }", prior->second);
            const auto& fields = c.variant->fields;
            if (fields.size() != arm.names.size())
                fail("E_PATTERN_ARITY", "Cantidad incorrecta de nombres en el patrón", arm.span,
                     "match s { State.Value(n) => return n }", c.variant->span);
            locals.levels.emplace_back();
            for (std::size_t i = 0; i < fields.size(); ++i)
                declare(arm.names[i], fields[i].type, false, arm.span);
            if (!block(arm.body, false)) returns = false;
            locals.levels.pop_back();
        }
        const auto prefix = types.program.nameOf(type) + ".";
        for (const auto& variant : d.variants) {
            if (!seen.contains(prefix + variant.name))
                fail("E_EXHAUSTIVE", "Falta la variante " + prefix + variant.name, s.span,
                     "match s { State.Ready => return }", variant.span);
        }
        return returns;
    }

    bool checkStatement(Stmt& s) {
        switch (s.kind) {
            case StmtKind::Let: case StmtKind::Var: {
                auto& init = s.exprs[0];
                const auto type = typeOf(init);
                requireValue(type, init.span);
                if (s.annotation != Type::Unknown) {
                    types.known(s.annotation, s.span);
                    requireType(type, s.annotation, init.span, s.span);
                }
                declare(s.name, type, s.kind == StmtKind::Var, s.span);
                break;
            }
            case StmtKind::Assign: {
                const auto& var = lookup(s.name, s.span);
                if (!var.writable)
                    fail("E_IMMUTABLE", "El nombre es de solo lectura: " + s.name, s.span,
                         "var total = 0\ntotal = 1", var.declared);
                requireType(typeOf(s.exprs[0]), var.type, s.exprs[0].span, var.declared);
                break;
            }
            case StmtKind::Call: typeOf(s.exprs[0]); break;
            case StmtKind::If: {
                requireType(typeOf(s.exprs[0]), Type::Bool, s.exprs[0].span);
                const bool thenReturns = block(s.body);
                const bool elseReturns = block(s.otherwise);
                return thenReturns && elseReturns;
            }
            case StmtKind::While:
                requireType(typeOf(s.exprs[0]), Type::Bool, s.exprs[0].span);
                block(s.body);
                break;
            case StmtKind::For:
                for (auto& bound : s.exprs) requireType(typeOf(bound), Type::Int, bound.span);
                locals.levels.emplace_back();
                declare(s.name, Type::Int, false, s.span);
                block(s.body, false);
                locals.levels.pop_back();
                break;
            case StmtKind::Return:
                if (s.exprs.empty()) requireType(Type::Void, current.result, s.span, current.span);
                else {
                    const auto type = typeOf(s.exprs[0]);
                    requireValue(type, s.exprs[0].span);
                    requireType(type, current.result, s.exprs[0].span, current.span);
                }
                return true;
            case StmtKind::Assert: requireType(typeOf(s.exprs[0]), Type::Bool, s.exprs[0].span); break;
            case StmtKind::Print: {
                const auto type = typeOf(s.exprs[0]);
                requireValue(type, s.exprs[0].span);
                if (type >= Type::NamedBase) fail("E_PRINT", "print admite int, bool o str", s.span, "print(p.x)");
                break;
            }
            case StmtKind::Match: return match(s);
        }
        return false;
    }
};

} // namespace

void check(Program& program) {
    std::unordered_map<Type, std::size_t> sizes;
    {
        Types declared(program);
        Layouts layouts{declared, {}, {}};
        for (const auto& d : program.data) sizes.emplace(d.type, layouts.measure(d.type, d.span).bytes);
    }
    GenericInstances generics(program);
    Types types(program);
    Functions functions, tests;
    for (const auto& fn : program.functions) {
        auto& names = fn.isTest ? tests : functions;
        if (!fn.isTest && (fn.name == "len" || fn.name == "print" || fn.name == "assert"))
            fail("E_RESERVED", "Nombre reservado: " + fn.name, fn.span, "fn count() -> int { return 1 }");
        const auto [it, added] = names.emplace(fn.name, &fn);
        if (!added) fail("E_DUP_FUNCTION", "Declaración duplicada: " + fn.name, fn.span,
                         "fn other() {}", it->second->span);
        if (!fn.isTest && fn.name == "main" && (!fn.params.empty() || fn.result != Type::Void))
            fail("E_MAIN", "main debe carecer de parámetros y devolver void", fn.span, "fn main() {}");
    }
    auto checkFunction = [&](Function& fn) {
        Checker checker{functions, fn, {}, types, generics, sizes};
        types.known(fn.result, fn.span, true);
        checker.reserveValue(fn.result, fn.span);
        for (const auto& param : fn.params) {
            types.known(param.type, param.span);
            checker.declare(param.name, param.type, false, param.span);
        }
        if (!checker.block(fn.body, false) && fn.result != Type::Void)
            fail("E_RETURN", "Hay un camino sin valor de retorno en " + fn.name, fn.span,
                 "fn one() -> int { return 1 }");
    };
    for (auto& fn : program.functions) checkFunction(fn);
    for (std::size_t i = 0; i < generics.pending.size(); ++i) checkFunction(generics.pending[i]);
    generics.finish();
}

namespace {

// Independent, stack-based evaluation. Calls, loops and returns never use the
// host call stack. Only the bounded syntax tree is visited recursively.
struct Aggregate;
using AggregatePtr = std::shared_ptr<const Aggregate>;
using Value = std::variant<std::monostate, Integer, bool, std::string_view, AggregatePtr>;
struct Aggregate {
    std::string_view tag;
    std::vector<Value> fields;
};
enum class Op {
    Literal, Read, Call, Unary, Binary, ShortCircuit, Bind, Write, Discard, Print,
    Assert, IfFalse, Jump, Enter, Leave, RangeEnter, RangeBind, RangeNext, Return,
    Construct, Field, MatchArm
};
struct Instruction {
    Op op;
    Span span;
    const Expr* expr = nullptr;
    const Stmt* stmt = nullptr;
    std::size_t target = 0;
    const MatchArm* arm = nullptr;
};
using Code = std::vector<Instruction>;

struct EvaluationOrder {
    Code code;

    void expression(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Name: code.push_back({Op::Read, e.span, &e}); return;
            case ExprKind::Integer: case ExprKind::Boolean: case ExprKind::String:
                code.push_back({Op::Literal, e.span, &e}); return;
            case ExprKind::Call:
                for (const auto& arg : e.args) expression(arg);
                code.push_back({Op::Call, e.span, &e}); return;
            case ExprKind::Unary:
                expression(e.args[0]);
                code.push_back({Op::Unary, e.span, &e}); return;
            case ExprKind::Field:
                expression(e.args[0]);
                code.push_back({Op::Field, e.span, &e}); return;
            case ExprKind::Construct:
                for (const auto& arg : e.args) expression(arg);
                code.push_back({Op::Construct, e.span, &e}); return;
            case ExprKind::Binary:
                expression(e.args[0]);
                if (e.text == "&&" || e.text == "||") {
                    const auto jump = code.size();
                    code.push_back({Op::ShortCircuit, e.span, &e});
                    expression(e.args[1]);
                    code[jump].target = code.size();
                } else {
                    expression(e.args[1]);
                    code.push_back({Op::Binary, e.span, &e});
                }
                return;
        }
    }

    void block(const std::vector<Stmt>& body) {
        for (const auto& s : body) statement(s);
    }

    void scoped(const std::vector<Stmt>& body, Span span) {
        code.push_back({Op::Enter, span});
        block(body);
        code.push_back({Op::Leave, span});
    }

    void statement(const Stmt& s) {
        const auto start = code.size();
        for (const auto& expr : s.exprs) expression(expr);
        const auto current = code.size();
        switch (s.kind) {
            case StmtKind::Let: case StmtKind::Var: code.push_back({Op::Bind, s.span, nullptr, &s}); return;
            case StmtKind::Assign: code.push_back({Op::Write, s.span, nullptr, &s}); return;
            case StmtKind::Call: code.push_back({Op::Discard, s.span}); return;
            case StmtKind::Print: code.push_back({Op::Print, s.span}); return;
            case StmtKind::Assert: code.push_back({Op::Assert, s.span}); return;
            case StmtKind::Return: code.push_back({Op::Return, s.span, nullptr, &s}); return;
            case StmtKind::If: {
                code.push_back({Op::IfFalse, s.span});
                scoped(s.body, s.span);
                const auto endThen = code.size();
                code.push_back({Op::Jump, s.span});
                code[current].target = code.size();
                scoped(s.otherwise, s.span);
                code[endThen].target = code.size();
                return;
            }
            case StmtKind::While:
                code.push_back({Op::IfFalse, s.span});
                scoped(s.body, s.span);
                code.push_back({Op::Jump, s.span, nullptr, nullptr, start});
                code[current].target = code.size();
                return;
            case StmtKind::For:
                code.push_back({Op::RangeEnter, s.span});
                code.push_back({Op::RangeBind, s.span, nullptr, &s});
                block(s.body);
                code.push_back({Op::RangeNext, s.span, nullptr, nullptr, current + 1});
                code[current].target = code.size();
                return;
            case StmtKind::Match: {
                std::vector<std::size_t> exits;
                for (const auto& arm : s.arms) {
                    const auto attempt = code.size();
                    code.push_back({Op::MatchArm, arm.span, nullptr, nullptr, 0, &arm});
                    block(arm.body);
                    code.push_back({Op::Leave, arm.span});
                    exits.push_back(code.size());
                    code.push_back({Op::Jump, arm.span});
                    code[attempt].target = code.size();
                }
                for (auto end : exits) code[end].target = code.size();
                return;
            }
        }
    }
};

[[noreturn]] void overflow(Span span) {
    fail("R_OVERFLOW", "Desbordamiento de int", span, "9223372036854775807 - 1");
}

Value binary(const Expr& expr, Value lhs, Value rhs) {
    const auto& op = expr.text;
    if (op == "==") return lhs == rhs;
    if (op == "!=") return lhs != rhs;
    const auto a = std::get<Integer>(lhs), b = std::get<Integer>(rhs);
    if (op == "<") return a < b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == ">=") return a >= b;
    if (op == "/" || op == "%") {
        if (b == 0) fail("R_DIV_ZERO", "Divisor cero", expr.span, "let half = 4 / 2");
        if (a == minInt && b == -1) overflow(expr.span);
        return op == "/" ? a / b : a % b;
    }
    std::optional<Integer> result;
    if (op == "+") result = checkedAdd(a, b);
    else if (op == "-") result = checkedSub(a, b);
    else result = checkedMul(a, b);
    if (!result) overflow(expr.span);
    return *result;
}

struct Range { Integer cursor, end; };
struct Frame {
    const Code* code;
    std::size_t ip, base;
    Scopes<Value> locals;
    std::vector<Range> ranges;
};

struct Interpreter {
    Functions functions;
    std::unordered_map<const Function*, Code> bodies;
    std::vector<Frame> frames;
    std::vector<Value> operands;
    std::ostream& out;
    Limits limits;

    Interpreter(const Program& program, std::ostream& stream, Limits budget) : out(stream), limits(budget) {
        for (const auto& fn : program.functions) {
            if (!fn.isTest) functions.emplace(fn.name, &fn);
            EvaluationOrder order;
            order.block(fn.body);
            order.code.push_back({Op::Return, fn.span});
            bodies.emplace(&fn, std::move(order.code));
        }
    }

    Value pop() {
        auto result = operands.back();
        operands.pop_back();
        return result;
    }

    void activate(const Function& fn, Span at) {
        if (frames.size() >= limits.depth)
            fail("R_DEPTH", "Límite de llamadas alcanzado", at, "fn main() {}");
        const auto base = operands.size() - fn.params.size();
        auto& frame = frames.emplace_back(Frame{&bodies.at(&fn), 0, base, {}, {}});
        for (std::size_t i = 0; i < fn.params.size(); ++i)
            frame.locals.levels.back().emplace(fn.params[i].name, operands[base + i]);
        operands.resize(base);
    }

    void print(Value value) {
        if (auto text = std::get_if<std::string_view>(&value)) out << *text;
        else if (auto n = std::get_if<Integer>(&value)) out << *n;
        else out << (std::get<bool>(value) ? "true" : "false");
        out << '\n';
    }

    void execute(const Function& entry) {
        activate(entry, entry.span);
        auto steps = limits.steps;
        while (!frames.empty()) {
            Frame& frame = frames.back();
            const Instruction& i = (*frame.code)[frame.ip++];
            if (steps == 0) fail("R_STEPS", "Presupuesto de ejecución agotado", i.span, "for i in 0..3 { print(i) }");
            --steps;
            switch (i.op) {
                case Op::Literal:
                    if (i.expr->kind == ExprKind::Boolean) operands.emplace_back(i.expr->text == "true");
                    else if (i.expr->kind == ExprKind::Integer) operands.emplace_back(parseInteger(*i.expr));
                    else operands.emplace_back(std::string_view(i.expr->text));
                    break;
                case Op::Read: operands.push_back(*frame.locals.find(i.expr->text)); break;
                case Op::Call:
                    if (i.expr->text == "len") {
                        const auto length = std::get<std::string_view>(pop()).size();
                        if (length > static_cast<std::uint64_t>(maxInt)) overflow(i.span);
                        operands.emplace_back(static_cast<Integer>(length));
                    } else activate(*functions.at(i.expr->text), i.span);
                    break;
                case Op::Unary:
                    if (i.expr->text == "!") operands.emplace_back(!std::get<bool>(pop()));
                    else {
                        const auto value = std::get<Integer>(pop());
                        if (value == minInt) overflow(i.span);
                        operands.emplace_back(-value);
                    }
                    break;
                case Op::Binary: {
                    const auto right = pop();
                    const auto left = pop();
                    operands.push_back(binary(*i.expr, left, right));
                    break;
                }
                case Op::ShortCircuit:
                    if (std::get<bool>(operands.back()) == (i.expr->text == "||")) frame.ip = i.target;
                    else operands.pop_back();
                    break;
                case Op::Construct: {
                    const auto& indices = i.expr->fieldIndices;
                    auto data = std::make_shared<Aggregate>();
                    data->tag = i.expr->text;
                    data->fields.resize(indices.size());
                    for (std::size_t n = indices.size(); n != 0; --n) data->fields[indices[n - 1]] = pop();
                    operands.emplace_back(std::move(data));
                    break;
                }
                case Op::Field: {
                    const auto data = std::get<AggregatePtr>(pop());
                    operands.push_back(data->fields[i.expr->fieldIndices[0]]);
                    break;
                }
                case Op::MatchArm: {
                    const auto data = std::get<AggregatePtr>(operands.back());
                    if (data->tag != i.arm->variant) {
                        frame.ip = i.target;
                        break;
                    }
                    pop();
                    auto& bindings = frame.locals.levels.emplace_back();
                    for (std::size_t n = 0; n < i.arm->names.size(); ++n)
                        bindings.emplace(i.arm->names[n], data->fields[n]);
                    break;
                }
                case Op::Bind: frame.locals.levels.back().emplace(i.stmt->name, pop()); break;
                case Op::Write: *frame.locals.find(i.stmt->name) = pop(); break;
                case Op::Discard: pop(); break;
                case Op::Print: print(pop()); break;
                case Op::Assert:
                    if (!std::get<bool>(pop())) fail("R_ASSERT", "Falló la aserción", i.span, "assert 1 == 1");
                    break;
                case Op::IfFalse: if (!std::get<bool>(pop())) frame.ip = i.target; break;
                case Op::Jump: frame.ip = i.target; break;
                case Op::Enter: frame.locals.levels.emplace_back(); break;
                case Op::Leave: frame.locals.levels.pop_back(); break;
                case Op::RangeEnter: {
                    const auto end = std::get<Integer>(pop());
                    const auto start = std::get<Integer>(pop());
                    if (start >= end) frame.ip = i.target;
                    else frame.ranges.push_back({start, end});
                    break;
                }
                case Op::RangeBind:
                    frame.locals.levels.emplace_back();
                    frame.locals.levels.back().emplace(i.stmt->name, frame.ranges.back().cursor);
                    break;
                case Op::RangeNext: {
                    frame.locals.levels.pop_back();
                    auto& range = frame.ranges.back();
                    // cursor < end <= INT64_MAX, so increment is always representable.
                    if (++range.cursor < range.end) frame.ip = i.target;
                    else frame.ranges.pop_back();
                    break;
                }
                case Op::Return: {
                    const auto result = i.stmt && !i.stmt->exprs.empty() ? pop() : Value{};
                    operands.resize(frame.base);
                    frames.pop_back();
                    if (!frames.empty()) operands.push_back(result);
                    break;
                }
            }
        }
    }
};

} // namespace

void runTests(const Program& program, std::ostream& output, Limits limits) {
    Interpreter interpreter(program, output, limits);
    for (const auto& fn : program.functions) {
        if (fn.isTest) interpreter.execute(fn);
    }
}

void runMain(const Program& program, std::ostream& output, Limits limits) {
    Interpreter interpreter(program, output, limits);
    const auto main = interpreter.functions.find("main");
    if (main == interpreter.functions.end()) fail("E_MAIN", "Falta la función main", {}, "fn main() {}");
    interpreter.execute(*main->second);
}

} // namespace veloz
