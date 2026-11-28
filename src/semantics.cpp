#include "veloz.hpp"
#include "generics.hpp"
#include "layout.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <istream>
#include <optional>
#include <random>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <malloc.h>
#include <share.h>
#ifdef interface
#undef interface
#endif
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
    const Binding* find(const std::string& name) const {
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
        case Type::Float: error.example = "let x: float = 1.5"; break;
        case Type::Bool: error.example = "let b: bool = true"; break;
        case Type::Str: error.example = R"(let s: str = "hola")"; break;
        case Type::StringBuilder: error.example = "var sb: StringBuilder = sbNew()"; break;
        case Type::Void: error.example = "fn done() { return }"; break;
        default: error.example = "fn identity(x: " + error.expected + ") -> " + error.expected + " { return x }";
    }
    throw error;
}

void requireValue(Type type, Span where) {
    if (type != Type::Void) return;
    Diagnostic error("E_VALUE", "Se necesita un valor", where);
    error.actual = "void";
    error.expected = "int | float | bool | str";
    error.example = "fn one() -> int { return 1 }";
    throw error;
}

double parseFloat(const Expr& expression) {
    const auto& text = expression.text;
    double result{};
    const auto end = text.data() + text.size();
    const auto [cursor, ec] = std::from_chars(text.data(), end, result);
    if (ec != std::errc{} || cursor != end || !std::isfinite(result))
        fail("E_FLOAT_RANGE", "Literal fuera del rango de float", expression.span, "3.14");
    return result;
}

std::string formatFloat(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v < 0.0 ? "-Inf" : "Inf";
    if (v == 0.0) v = 0.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
}

bool parseIntExact(std::string_view sv, Integer& out) {
    if (sv.empty()) return false;
    std::size_t i = 0;
    bool neg = false;
    if (sv[i] == '+' || sv[i] == '-') {
        neg = (sv[i] == '-');
        ++i;
    }
    if (i == sv.size()) return false;
    std::uint64_t acc = 0;
    const std::uint64_t limit = neg ? (static_cast<std::uint64_t>(maxInt) + 1ULL)
                                    : static_cast<std::uint64_t>(maxInt);
    for (; i < sv.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(sv[i]);
        if (c < '0' || c > '9') return false;
        const std::uint64_t d = static_cast<std::uint64_t>(c - '0');
        if (acc > (limit - d) / 10ULL) return false;
        acc = acc * 10ULL + d;
    }
    out = neg ? ((acc == limit) ? minInt : -static_cast<Integer>(acc))
              : static_cast<Integer>(acc);
    return true;
}

bool parseFloatExact(std::string_view sv, double& out) {
    if (sv.empty()) return false;
    std::size_t i = 0;
    if (sv[i] == '+' || sv[i] == '-') ++i;
    std::size_t digits = 0;
    while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') { ++i; ++digits; }
    if (i < sv.size() && sv[i] == '.') {
        ++i;
        while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') { ++i; ++digits; }
    }
    if (digits == 0) return false;
    if (i < sv.size() && (sv[i] == 'e' || sv[i] == 'E')) {
        ++i;
        if (i < sv.size() && (sv[i] == '+' || sv[i] == '-')) ++i;
        std::size_t expDigits = 0;
        while (i < sv.size() && sv[i] >= '0' && sv[i] <= '9') { ++i; ++expDigits; }
        if (expDigits == 0) return false;
    }
    if (i != sv.size()) return false;
    std::string copy(sv);
    char* endPtr = nullptr;
    errno = 0;
    const double v = std::strtod(copy.c_str(), &endPtr);
    if (endPtr != copy.c_str() + copy.size() || errno == ERANGE || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string exactFloatLiteral(double v) {
    char buf[64];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec == std::errc{}) return std::string(buf, static_cast<std::size_t>(ptr - buf));
    return formatFloat(v);
}

unsigned checkShiftCount(Integer count, Span span, unsigned width = 64) {
    if (count < 0 || static_cast<std::uint64_t>(count) >= width)
        fail("R_OVERFLOW", "Desplazamiento fuera de rango [0, " + std::to_string(width - 1) + "]", span, "x << 1");
    return static_cast<unsigned>(count);
}

struct ExactInt {
    Type type = Type::Int;
    std::uint64_t bits = 0;
    bool operator==(const ExactInt& other) const = default;
};

constexpr std::uint64_t maskForWidth(unsigned width) {
    switch (width) {
        case 8: return 0xFFULL;
        case 16: return 0xFFFFULL;
        case 32: return 0xFFFFFFFFULL;
        default: return ~0ULL;
    }
}

constexpr std::uint64_t maskForType(Type t) {
    return maskForWidth(Program::intBitWidth(t));
}

constexpr Integer minSignedForWidth(unsigned width) {
    switch (width) {
        case 8: return -128;
        case 16: return -32768;
        case 32: return -2147483648LL;
        default: return minInt;
    }
}

constexpr Integer maxSignedForWidth(unsigned width) {
    switch (width) {
        case 8: return 127;
        case 16: return 32767;
        case 32: return 2147483647LL;
        default: return maxInt;
    }
}

constexpr Integer signExtendWidth(std::uint64_t bits, unsigned width) {
    switch (width) {
        case 8: return static_cast<Integer>(static_cast<std::int8_t>(bits & 0xFFULL));
        case 16: return static_cast<Integer>(static_cast<std::int16_t>(bits & 0xFFFFULL));
        case 32: return static_cast<Integer>(static_cast<std::int32_t>(bits & 0xFFFFFFFFULL));
        default: return static_cast<Integer>(bits);
    }
}

bool isDecimalDigits(std::string_view text) {
    return !text.empty() && !text.starts_with('-') &&
           !text.starts_with("0x") && !text.starts_with("0X") &&
           !text.starts_with("0b") && !text.starts_with("0B");
}

std::uint64_t parseExactIntLiteral(const Program& program, const Expr& expression, Type target, bool negated) {
    std::string_view sv = expression.text;
    const unsigned width = Program::intBitWidth(target);
    const bool isSigned = Program::isSignedInt(target);
    const std::string tName = program.nameOf(target);
    if (!sv.empty() && sv.front() == '-') {
        negated = true;
        sv.remove_prefix(1);
    }
    if (sv.starts_with("0x") || sv.starts_with("0X") || sv.starts_with("0b") || sv.starts_with("0B")) {
        if (negated && !isSigned)
            fail("E_TYPE", "Menos unario no aplica a enteros sin signo", expression.span, "0");
        const int base = (sv[1] == 'x' || sv[1] == 'X') ? 16 : 2;
        std::uint64_t uv{};
        const auto end = sv.data() + sv.size();
        const auto [cursor, ec] = std::from_chars(sv.data() + 2, end, uv, base);
        if (ec != std::errc{} || cursor != end || uv > maskForWidth(width))
            fail("E_INT_RANGE", "Literal fuera del rango de " + tName, expression.span, "0xff");
        if (negated) {
            const std::uint64_t absMax = static_cast<std::uint64_t>(maxSignedForWidth(width)) + 1ULL;
            if (uv > absMax)
                fail("E_INT_RANGE", "Literal fuera del rango de " + tName, expression.span, "0");
            return (0ULL - uv) & maskForWidth(width);
        }
        return uv & maskForWidth(width);
    }
    std::uint64_t uv{};
    const auto end = sv.data() + sv.size();
    const auto [cursor, ec] = std::from_chars(sv.data(), end, uv, 10);
    if (ec != std::errc{} || cursor != end)
        fail("E_INT_RANGE", "Literal fuera del rango de " + tName, expression.span, "0");
    if (!isSigned) {
        if (negated)
            fail("E_TYPE", "Menos unario no aplica a enteros sin signo", expression.span, "0");
        if (uv > maskForWidth(width))
            fail("E_INT_RANGE", "Literal fuera del rango de " + tName, expression.span, "0");
        return uv & maskForWidth(width);
    }
    const std::uint64_t maxPos = static_cast<std::uint64_t>(maxSignedForWidth(width));
    const std::uint64_t maxNegMag = maxPos + 1ULL;
    if (negated) {
        if (uv > maxNegMag)
            fail("E_INT_RANGE", "Literal fuera del rango de " + tName, expression.span, "0");
        return (0ULL - uv) & maskForWidth(width);
    }
    if (uv > maxPos)
        fail("E_INT_RANGE", "Literal fuera del rango de " + tName, expression.span, "0");
    return uv & maskForWidth(width);
}

double parseExactFloatLiteral(const Expr& expression, Type target) {
    const auto& text = expression.text;
    const auto end = text.data() + text.size();
    if (target == Type::F32) {
        float result{};
        const auto [cursor, ec] = std::from_chars(text.data(), end, result);
        if (ec != std::errc{} || cursor != end || !std::isfinite(result))
            fail("E_FLOAT_RANGE", "Literal fuera del rango de f32", expression.span, "3.14");
        return static_cast<double>(result);
    }
    double result{};
    const auto [cursor, ec] = std::from_chars(text.data(), end, result);
    if (ec != std::errc{} || cursor != end || !std::isfinite(result))
        fail("E_FLOAT_RANGE", "Literal fuera del rango de float", expression.span, "3.14");
    return result;
}

std::optional<Type> numericBuiltinType(std::string_view name) {
    if (name == "int" || name == "i64") return Type::Int;
    if (name == "i8") return Type::I8;
    if (name == "i16") return Type::I16;
    if (name == "i32") return Type::I32;
    if (name == "u8") return Type::U8;
    if (name == "u16") return Type::U16;
    if (name == "u32") return Type::U32;
    if (name == "u64") return Type::U64;
    if (name == "float" || name == "f64") return Type::Float;
    if (name == "f32") return Type::F32;
    return {};
}

/// Hint exact literal type when calling `u64(lit)` or `f32(lit)` directly on a literal.
Type numericLiteralHint(Type targetNum, const Expr& arg) {
    if (targetNum == Type::U64 && arg.kind == ExprKind::Integer) return Type::U64;
    if (targetNum == Type::F32 &&
        (arg.kind == ExprKind::Float ||
         (arg.kind == ExprKind::Unary && arg.text == "-" && !arg.args.empty() &&
          arg.args[0].kind == ExprKind::Float))) {
        return Type::F32;
    }
    return Type::Unknown;
}

std::optional<Integer> checkedAddWidth(Integer a, Integer b, unsigned width) {
    if (width == 64) return checkedAdd(a, b);
    const Integer res = a + b; // inputs are <= INT32_MAX, fits without UB in int64_t
    if (res < minSignedForWidth(width) || res > maxSignedForWidth(width)) return {};
    return res;
}

std::optional<Integer> checkedSubWidth(Integer a, Integer b, unsigned width) {
    if (width == 64) return checkedSub(a, b);
    const Integer res = a - b;
    if (res < minSignedForWidth(width) || res > maxSignedForWidth(width)) return {};
    return res;
}

std::optional<Integer> checkedMulWidth(Integer a, Integer b, unsigned width) {
    if (width == 64) return checkedMul(a, b);
    const Integer res = a * b;
    if (res < minSignedForWidth(width) || res > maxSignedForWidth(width)) return {};
    return res;
}

std::optional<std::uint64_t> checkedUAddWidth(std::uint64_t a, std::uint64_t b, unsigned width) {
    const std::uint64_t mask = maskForWidth(width);
    if (a > mask - b) return {};
    return a + b;
}

std::optional<std::uint64_t> checkedUSubWidth(std::uint64_t a, std::uint64_t b) {
    if (a < b) return {};
    return a - b;
}

std::optional<std::uint64_t> checkedUMulWidth(std::uint64_t a, std::uint64_t b, unsigned width) {
    const std::uint64_t mask = maskForWidth(width);
    if (b != 0 && a > mask / b) return {};
    return a * b;
}

struct Variable {
    Type type;
    bool writable;
    Span declared;
    Expr* regionSite = nullptr;
    bool scalarParam = false;
    bool isCaptured = false;
    bool ownedLocal = false;
    bool moved = false;
    Span movedAt{};
    std::string movedTo{};
};

enum class ArrayUse { Ordinary, Inspect, Copy };

struct Constructor {
    Type type;
    std::size_t declIndex;
    std::size_t variantIndex;
    bool isEnum;
};

Type substituteTypeMap(Type type, const std::map<Type, Type>& mapping, Program& program) {
    if (auto it = mapping.find(type); it != mapping.end()) return it->second;
    if (program.isArray(type))
        return program.arrayOf(substituteTypeMap(program.elemOf(type), mapping, program));
    if (const auto* app = program.typeAppOf(type)) {
        const std::string base = app->baseName;
        std::vector<Type> args = app->args;
        for (Type& a : args) a = substituteTypeMap(a, mapping, program);
        return program.typeApp(base, args);
    }
    return type;
}

bool containsSelf(const Program& program, Type t) {
    if (t >= Type::NamedBase && program.nameOf(t) == "Self") return true;
    if (program.isArray(t)) return containsSelf(program, program.elemOf(t));
    if (const auto* app = program.typeAppOf(t)) {
        for (Type a : app->args) if (containsSelf(program, a)) return true;
    }
    return false;
}

void checkTypeVisible(const Program& program, const std::string& fromModule, Type t, Span where,
                      const std::vector<Type>& scopeGenerics = {}) {
    if (std::find(scopeGenerics.begin(), scopeGenerics.end(), t) != scopeGenerics.end()) return;
    if (program.isArray(t)) {
        checkTypeVisible(program, fromModule, program.elemOf(t), where, scopeGenerics);
        return;
    }
    if (const auto* app = program.typeAppOf(t)) {
        if (!app->baseName.starts_with("@") && !program.visible(fromModule, app->baseName))
            fail("E_IMPORT", "Tipo no importado directamente: " + app->baseName, where, "import model");
        for (Type a : app->args) checkTypeVisible(program, fromModule, a, where, scopeGenerics);
        return;
    }
    if (t >= Type::NamedBase && !program.visible(fromModule, program.nameOf(t)))
        fail("E_IMPORT", "Tipo no importado directamente: " + program.nameOf(t), where, "import model");
}

bool isReservedTypeName(std::string_view fullName) {
    const auto dot = fullName.rfind('.');
    const std::string_view shortName = dot == std::string_view::npos ? fullName : fullName.substr(dot + 1);
    return shortName == "StringBuilder" || shortName == "Option" || shortName == "Result" ||
           shortName == "Map" || shortName == "Set" ||
           shortName == "Pool" || shortName == "Handle" || shortName == "Shared" ||
           shortName == "File" || shortName == "TcpListener" || shortName == "Socket" ||
           shortName == "Mutex" || shortName == "Lock" ||
           shortName == "Scope" || shortName == "Region" ||
           shortName == "Task" || shortName == "Frozen" ||
           shortName == "Ptr" || shortName == "MutPtr" || shortName == "Simd" ||
           shortName == "Atomic" || shortName == "RingQueue" ||
           shortName == "Some" || shortName == "None" || shortName == "Ok" || shortName == "Err";
}

bool isOpaqueType(const Program& program, Type t) {
    if (t < Type::NamedBase || program.isArray(t) || program.typeAppOf(t)) return false;
    const std::string name = program.nameOf(t);
    return !name.starts_with("@ClosureEnv.") && name.find('@') != std::string::npos;
}

bool mentionsOpaqueType(const Program& program, Type t) {
    if (isOpaqueType(program, t)) return true;
    if (program.isArray(t)) return mentionsOpaqueType(program, program.elemOf(t));
    if (const auto* app = program.typeAppOf(t)) {
        for (Type a : app->args) if (mentionsOpaqueType(program, a)) return true;
    }
    return false;
}

unsigned simdElemBytes(Type elemT) {
    switch (elemT) {
        case Type::I8:
        case Type::U8: return 1u;
        case Type::I16:
        case Type::U16: return 2u;
        case Type::I32:
        case Type::U32:
        case Type::F32: return 4u;
        case Type::Int:
        case Type::U64:
        case Type::Float: return 8u;
        default: return 0u;
    }
}

void validateSimdType(const Program& program, Type simdT, Span where) {
    if (!program.isSimd(simdT)) {
        Diagnostic d("E_SIMD_TYPE", "Se esperaba un tipo vectorial Simd<T, N>.", where);
        d.example = "Simd<f32, 4>";
        throw d;
    }
    const Type elem = program.simdElem(simdT);
    const unsigned lanes = program.simdLanes(simdT);
    if (lanes != 2u && lanes != 4u && lanes != 8u && lanes != 16u && lanes != 32u) {
        Diagnostic d("E_SIMD_TYPE", "Simd<T, N> requiere N en {2, 4, 8, 16, 32}.", where);
        d.example = "Simd<f32, 4>";
        throw d;
    }
    if (mentionsOpaqueType(program, elem)) return;
    if (!Program::isNumericType(elem)) {
        Diagnostic d("E_SIMD_TYPE", "Simd<T, N> solo admite elementos numéricos (i8..u64, f32, float), no '" +
                     program.nameOf(elem) + "'.", where);
        d.actual = program.nameOf(elem);
        d.example = "Simd<f32, 4>";
        throw d;
    }
    const unsigned totalBytes = simdElemBytes(elem) * lanes;
    if (totalBytes < 8u || totalBytes > 64u) {
        Diagnostic d("E_SIMD_TYPE", "El ancho total de Simd<T, N> debe estar entre 8 y 64 bytes (recibido " +
                     std::to_string(totalBytes) + " bytes).", where);
        d.actual = program.nameOf(simdT);
        d.example = "Simd<f32, 4>";
        throw d;
    }
}

Type simdMaskType(Program& program, Type elemT, unsigned lanes) {
    switch (simdElemBytes(elemT)) {
        case 1u: return program.simdOf(Type::U8, lanes);
        case 2u: return program.simdOf(Type::U16, lanes);
        case 4u: return program.simdOf(Type::U32, lanes);
        default: return program.simdOf(Type::U64, lanes);
    }
}

bool mentionsType(const Program& program, Type container, Type target) {
    if (container == target) return true;
    if (program.isArray(container)) return mentionsType(program, program.elemOf(container), target);
    if (const auto* app = program.typeAppOf(container)) {
        for (Type a : app->args) if (mentionsType(program, a, target)) return true;
    }
    return false;
}

void substituteStmtListMap(std::vector<Stmt>& stmts, const std::map<Type, Type>& mapping, Program& program);

void substituteExprMap(Expr& e, const std::map<Type, Type>& mapping, Program& program) {
    e.assocType = substituteTypeMap(e.assocType, mapping, program);
    for (Type& t : e.typeArgs) t = substituteTypeMap(t, mapping, program);
    if (e.kind == ExprKind::Construct && (e.text == "Self" || e.text.starts_with("Self."))) {
        for (const auto& [k, v] : mapping) {
            if (k >= Type::NamedBase && program.nameOf(k) == "Self") {
                const std::string suffix = e.text.size() > 4 ? e.text.substr(4) : "";
                if (const auto* app = program.typeAppOf(v); app && !app->baseName.starts_with("@")) {
                    e.text = app->baseName + suffix;
                    e.typeArgs = app->args;
                } else {
                    e.text = program.nameOf(v) + suffix;
                }
                break;
            }
        }
    }
    for (auto& arg : e.args) substituteExprMap(arg, mapping, program);
    if (e.closure) {
        for (auto& p : e.closure->params) p.type = substituteTypeMap(p.type, mapping, program);
        e.closure->result = substituteTypeMap(e.closure->result, mapping, program);
        substituteStmtListMap(e.closure->body, mapping, program);
    }
    if (e.matchExpr) {
        substituteExprMap(e.matchExpr->scrutinee, mapping, program);
        for (auto& arm : e.matchExpr->arms) {
            for (Type& t : arm.typeArgs) t = substituteTypeMap(t, mapping, program);
            substituteStmtListMap(arm.body, mapping, program);
            if (arm.resultExpr) substituteExprMap(*arm.resultExpr, mapping, program);
        }
    }
    if (e.ifExpr) {
        substituteExprMap(e.ifExpr->condition, mapping, program);
        substituteStmtListMap(e.ifExpr->thenBody, mapping, program);
        if (e.ifExpr->thenResult) substituteExprMap(*e.ifExpr->thenResult, mapping, program);
        substituteStmtListMap(e.ifExpr->elseBody, mapping, program);
        if (e.ifExpr->elseResult) substituteExprMap(*e.ifExpr->elseResult, mapping, program);
    }
}

void substituteStmtListMap(std::vector<Stmt>& stmts, const std::map<Type, Type>& mapping, Program& program) {
    for (auto& s : stmts) {
        s.annotation = substituteTypeMap(s.annotation, mapping, program);
        for (auto& e : s.exprs) substituteExprMap(e, mapping, program);
        substituteStmtListMap(s.body, mapping, program);
        substituteStmtListMap(s.otherwise, mapping, program);
        for (auto& arm : s.arms) {
            for (Type& t : arm.typeArgs) t = substituteTypeMap(t, mapping, program);
            if (arm.variant.starts_with("Self.")) {
                for (const auto& [k, v] : mapping) {
                    if (k >= Type::NamedBase && program.nameOf(k) == "Self") {
                        const std::string suffix = arm.variant.substr(4);
                        if (const auto* app = program.typeAppOf(v); app && !app->baseName.starts_with("@")) {
                            arm.variant = app->baseName + suffix;
                            arm.typeArgs = app->args;
                        } else {
                            arm.variant = program.nameOf(v) + suffix;
                        }
                        break;
                    }
                }
            }
            substituteStmtListMap(arm.body, mapping, program);
        }
    }
}

void substituteFnMap(Function& fn, const std::map<Type, Type>& mapping, Program& program) {
    for (auto& p : fn.params) p.type = substituteTypeMap(p.type, mapping, program);
    fn.result = substituteTypeMap(fn.result, mapping, program);
    for (auto& req : fn.requiresClauses) substituteExprMap(req, mapping, program);
    for (auto& ens : fn.ensuresClauses) substituteExprMap(ens, mapping, program);
    substituteStmtListMap(fn.body, mapping, program);
}

std::string_view shortTypeName(std::string_view fullName) {
    const auto dot = fullName.rfind('.');
    return dot == std::string_view::npos ? fullName : fullName.substr(dot + 1);
}

std::optional<Edit> identifierFix(const Program& program, Span where, std::string_view wrong, std::string_view right) {
    const auto src = program.sourceOf(where);
    if (where.start > where.end || where.end > src.size() || wrong.empty()) return {};
    const std::string_view slice = src.substr(where.start, where.end - where.start);
    auto isWord = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    };
    std::size_t pos = 0;
    while ((pos = slice.find(wrong, pos)) != std::string_view::npos) {
        const std::size_t absPos = where.start + pos;
        const bool leftOk = absPos == 0 || !isWord(src[absPos - 1]);
        const bool rightOk = absPos + wrong.size() >= src.size() || !isWord(src[absPos + wrong.size()]);
        if (leftOk && rightOk) return Edit{{absPos, absPos + wrong.size(), where.file}, std::string(right)};
        pos += wrong.size();
    }
    return {};
}

bool isCollectionAlias(std::string_view shortName) {
    return shortName == "HashMap" || shortName == "Dict" || shortName == "HashSet" ||
           shortName == "Arena" || shortName == "SlotMap" || shortName == "Slab" ||
           shortName == "Rc" || shortName == "Arc" || shortName == "SharedPtr" ||
           shortName == "FileStream" || shortName == "BufReader" || shortName == "BufWriter" ||
           shortName == "TextIOWrapper" || shortName == "TcpStream" ||
           shortName == "MutexGuard" || shortName == "LockGuard" || shortName == "UniqueLock" ||
           shortName == "Thread" || shortName == "JoinHandle" || shortName == "Future" ||
           shortName == "Promise" || shortName == "Channel" || shortName == "RwLock" || shortName == "Vec" ||
           shortName == "AtomicBool" || shortName == "AtomicI8" || shortName == "AtomicU8" ||
           shortName == "AtomicI16" || shortName == "AtomicU16" || shortName == "AtomicI32" ||
           shortName == "AtomicU32" || shortName == "AtomicI64" || shortName == "AtomicU64" ||
           shortName == "AtomicIsize" || shortName == "AtomicUsize" || shortName == "AtomicInt" ||
           shortName == "MpmcQueue" || shortName == "SpscQueue" || shortName == "RingBuffer" ||
           shortName == "BoundedQueue" || shortName == "Bump" || shortName == "BumpAllocator" ||
           shortName == "ArenaAllocator";
}

[[noreturn]] void failCollectionAlias(const Program& program, std::string_view fullName, Span where) {
    const std::string_view s = shortTypeName(fullName);
    if (s == "HashMap" || s == "Dict") {
        Diagnostic d("E_MAP_NAME", "Turi usa Map<K, V> en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "Map");
        d.example = "let m: Map<str, int> = mapNew()";
        throw d;
    }
    if (s == "HashSet") {
        Diagnostic d("E_SET_NAME", "Turi usa Set<T> en lugar de HashSet.", where);
        d.fix = identifierFix(program, where, s, "Set");
        d.example = "let s: Set<int> = setNew()";
        throw d;
    }
    if (s == "Arena" || s == "SlotMap" || s == "Slab") {
        Diagnostic d("E_POOL_NAME", "Turi usa Pool<T> y Handle<T> en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "Pool");
        d.example = "var p: Pool<Node> = poolNew()";
        throw d;
    }
    if (s == "Rc" || s == "Arc" || s == "SharedPtr") {
        Diagnostic d("E_RC_NAME", "Turi usa Shared<T> en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "Shared");
        d.example = "let s: Shared<int> = sharedNew(42)";
        throw d;
    }
    if (s == "FileStream" || s == "BufReader" || s == "BufWriter" || s == "TextIOWrapper") {
        Diagnostic d("E_RESOURCE_NAME", "Turi usa el recurso con ámbito File en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "File");
        d.example = R"(var f = fileOpen("datos.txt", "r")?)";
        throw d;
    }
    if (s == "TcpStream") {
        Diagnostic d("E_RESOURCE_NAME", "Turi usa Socket en lugar de TcpStream.", where);
        d.fix = identifierFix(program, where, s, "Socket");
        d.example = R"(var s = tcpConnect("127.0.0.1", 8080)?)";
        throw d;
    }
    if (s == "MutexGuard" || s == "LockGuard" || s == "UniqueLock") {
        Diagnostic d("E_RESOURCE_NAME", "Turi usa Lock devuelto por mutexLock(mut m) en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "Lock");
        d.example = "let guard = mutexLock(mut m)";
        throw d;
    }
    if (s == "Thread" || s == "JoinHandle" || s == "Future" || s == "Promise") {
        Diagnostic d("E_CONC_NAME", "Turi usa Scope y Task<T> con taskSpawn(mut s, || ...) en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "Task");
        d.example = "var s = taskScope()\nlet t: Task<int> = taskSpawn(mut s, || 42)";
        throw d;
    }
    if (s == "RwLock") {
        Diagnostic d("E_CONC_NAME", "Turi usa Mutex o Frozen<T> para compartir datos entre tareas en lugar de RwLock.", where);
        d.fix = identifierFix(program, where, s, "Mutex");
        d.example = "let fz = freeze(data)";
        throw d;
    }
    if (s == "Channel") {
        Diagnostic d("E_CONC_NAME", "Turi usa taskJoin(t), taskSpawnMove y Mutex en lugar de Channel.", where);
        d.example = "var s = taskScope()\nlet t = taskSpawn(mut s, || 42)\nlet r = taskJoin(t)";
        throw d;
    }
    if (s == "Vec") {
        Diagnostic d("E_VEC_NAME", "Turi usa [T] para arreglos dinámicos (o Simd<T, N> para vectores SIMD) en lugar de Vec.", where);
        d.example = "let xs: [int] = [1, 2, 3]";
        throw d;
    }
    if (s == "AtomicBool" || s == "AtomicI8" || s == "AtomicU8" || s == "AtomicI16" ||
        s == "AtomicU16" || s == "AtomicI32" || s == "AtomicU32" || s == "AtomicI64" ||
        s == "AtomicU64" || s == "AtomicIsize" || s == "AtomicUsize" || s == "AtomicInt") {
        std::string rep = "Atomic<int>";
        if (s == "AtomicBool") rep = "Atomic<bool>";
        else if (s == "AtomicI8") rep = "Atomic<i8>";
        else if (s == "AtomicU8") rep = "Atomic<u8>";
        else if (s == "AtomicI16") rep = "Atomic<i16>";
        else if (s == "AtomicU16") rep = "Atomic<u16>";
        else if (s == "AtomicI32") rep = "Atomic<i32>";
        else if (s == "AtomicU32") rep = "Atomic<u32>";
        else if (s == "AtomicU64" || s == "AtomicUsize") rep = "Atomic<u64>";
        Diagnostic d("E_ATOMIC_NAME", "Turi usa " + rep + " en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, rep);
        d.example = "let a = atomicNew(0)";
        throw d;
    }
    if (s == "MpmcQueue" || s == "SpscQueue" || s == "RingBuffer" || s == "BoundedQueue") {
        Diagnostic d("E_CONC_NAME", "Turi usa RingQueue<T> en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "RingQueue");
        d.example = "let q = ringNew<int>(64)";
        throw d;
    }
    if (s == "Bump" || s == "BumpAllocator" || s == "ArenaAllocator") {
        Diagnostic d("E_RESOURCE_NAME", "Turi usa el recurso con ámbito Region en lugar de " + std::string(s) + ".", where);
        d.fix = identifierFix(program, where, s, "Region");
        d.example = "var r = regionNew(4096)";
        throw d;
    }
    fail("E_UNKNOWN_TYPE", "Tipo desconocido: " + std::string(fullName), where, "struct Point { x: int }");
}

struct Types {
    Program& program;
    std::unordered_map<Type, std::size_t> declIndices;
    std::unordered_map<std::string, Constructor> constructors;
    std::unordered_map<std::string, DataDecl> genericDecls;
    std::unordered_set<Type> activeInstantiations;
    std::size_t instantiationCount = 0;

    explicit Types(Program& p) : program(p) {
        std::vector<DataDecl> concrete;
        concrete.reserve(p.data.size());
        for (auto& d : p.data) {
            const auto name = p.nameOf(d.type);
            if (isReservedTypeName(name))
                fail("E_DUP_TYPE", "Tipo reservado del núcleo: " + name, d.span, "struct TextBuf { data: str }");
            if (genericDecls.contains(name))
                fail("E_DUP_TYPE", "Tipo duplicado: " + name, d.span, "struct Pair { first: int }", genericDecls.at(name).span);
            if (!d.generics.empty()) {
                for (const auto& conc : concrete) {
                    if (p.nameOf(conc.type) == name)
                        fail("E_DUP_TYPE", "Tipo duplicado: " + name, d.span, "struct Pair { first: int }", conc.span);
                }
                if (d.isEnum && d.variants.empty())
                    fail("E_ENUM_EMPTY", "El enum requiere una variante", d.span, "enum State { Ready }");
                std::unordered_set<std::string> seenVars;
                for (const auto& v : d.variants) {
                    if (!seenVars.insert(v.name).second)
                        fail("E_DUP_VARIANT", "Variante duplicada: " + name + "." + v.name, v.span, "enum State { Ready }", d.span);
                    std::unordered_map<std::string, Span> fieldNames;
                    for (const auto& f : v.fields) {
                        const auto [original, unique] = fieldNames.emplace(f.name, f.span);
                        if (!unique) fail("E_DUP_FIELD", "Campo duplicado: " + f.name, f.span,
                                          "struct Point { x: int }", original->second);
                    }
                }
                genericDecls.emplace(name, std::move(d));
            } else {
                concrete.push_back(std::move(d));
            }
        }
        p.data = std::move(concrete);
        reindexConcrete();
        for (const auto& d : p.data) {
            for (const auto& v : d.variants)
                for (const auto& f : v.fields)
                    checkTypeVisible(p, d.module, f.type, f.span);
        }
        for (const auto& [name, tmpl] : genericDecls) {
            (void)name;
            for (const auto& v : tmpl.variants)
                for (const auto& f : v.fields)
                    checkTypeVisible(p, tmpl.module, f.type, f.span, tmpl.generics);
        }
    }

    void registerConcreteAt(std::size_t idx) {
        const auto& d = program.data[idx];
        const auto name = program.nameOf(d.type);
        const auto [existing, added] = declIndices.emplace(d.type, idx);
        if (!added) fail("E_DUP_TYPE", "Tipo duplicado: " + name, d.span,
                         "struct Pair { first: int }", program.data[existing->second].span);
        if (d.isEnum && d.variants.empty())
            fail("E_ENUM_EMPTY", "El enum requiere una variante", d.span, "enum State { Ready }");
        for (std::size_t vi = 0; vi < d.variants.size(); ++vi) {
            const auto& v = d.variants[vi];
            const auto full = d.isEnum ? name + "." + v.name : name;
            const auto [seen, inserted] = constructors.emplace(full, Constructor{d.type, idx, vi, d.isEnum});
            if (!inserted) fail("E_DUP_VARIANT", "Variante duplicada: " + full, v.span,
                                "enum State { Ready }", program.data[seen->second.declIndex].variants[seen->second.variantIndex].span);
            std::unordered_map<std::string, Span> fieldNames;
            for (const auto& f : v.fields) {
                const auto [original, unique] = fieldNames.emplace(f.name, f.span);
                if (!unique) fail("E_DUP_FIELD", "Campo duplicado: " + f.name, f.span,
                                  "struct Point { x: int }", original->second);
            }
        }
    }

    void reindexConcrete() {
        declIndices.clear();
        constructors.clear();
        for (std::size_t i = 0; i < program.data.size(); ++i) registerConcreteAt(i);
    }

    void syncOpaque() {
        for (std::size_t i = 0; i < program.data.size(); ++i) {
            if (!declIndices.contains(program.data[i].type)) registerConcreteAt(i);
        }
    }

    bool hasDecl(Type t) const { return declIndices.contains(t); }
    const DataDecl& decl(Type t) const { return program.data.at(declIndices.at(t)); }

    bool isHashableKey(Type t) const {
        if (Program::isIntType(t) || t == Type::Bool || t == Type::Str ||
            program.isHandle(t) || isOpaqueType(program, t)) return true;
        if (t < Type::NamedBase || !hasDecl(t)) return false;
        const auto& d = decl(t);
        if (!d.isEnum) return false;
        return std::all_of(d.variants.begin(), d.variants.end(),
                           [](const Variant& v) { return v.fields.empty(); });
    }

    void ensureInstantiated(Type t, Span where) {
        if (program.isArray(t)) {
            ensureInstantiated(program.elemOf(t), where);
            return;
        }
        if (const auto* app = program.typeAppOf(t)) {
            if (declIndices.contains(t) || !activeInstantiations.insert(t).second) return;
            struct Guard {
                std::unordered_set<Type>& s;
                Type v;
                ~Guard() { s.erase(v); }
            } guard{activeInstantiations, t};
            const std::string baseName = app->baseName;
            const std::vector<Type> args = app->args;
            if (baseName == "@Option") {
                if (args.size() != 1)
                    fail("E_GENERIC", "Option<T> requiere exactamente 1 argumento de tipo", where, "Option<int>");
                known(args[0], where, false);
                DataDecl d{t, true, where, {}, {}};
                d.variants.push_back(Variant{"None", where, {}});
                d.variants.push_back(Variant{"Some", where, {Param{"value", args[0], where}}});
                const std::size_t idx = program.data.size();
                program.data.push_back(std::move(d));
                registerConcreteAt(idx);
                return;
            }
            if (baseName == "@Result") {
                if (args.size() != 2)
                    fail("E_GENERIC", "Result<T, E> requiere exactamente 2 argumentos de tipo", where, "Result<int, str>");
                known(args[0], where, false);
                known(args[1], where, false);
                DataDecl d{t, true, where, {}, {}};
                d.variants.push_back(Variant{"Ok", where, {Param{"value", args[0], where}}});
                d.variants.push_back(Variant{"Err", where, {Param{"error", args[1], where}}});
                const std::size_t idx = program.data.size();
                program.data.push_back(std::move(d));
                registerConcreteAt(idx);
                return;
            }
            if (baseName == "@Map") {
                if (args.size() != 2)
                    fail("E_GENERIC", "Map<K, V> requiere exactamente 2 argumentos de tipo", where, "Map<str, int>");
                known(args[0], where, false);
                known(args[1], where, false);
                if (!isHashableKey(args[0])) {
                    Diagnostic d("E_KEY_TYPE", "Tipo de clave no hashable para Map: " + program.nameOf(args[0]) +
                                 " (usa int, str, bool, Handle<T> o enum sin campos)", where);
                    d.expected = "int | str | bool | Handle<T> | enum";
                    d.actual = program.nameOf(args[0]);
                    d.example = "Map<str, int>";
                    throw d;
                }
                if (program.isResource(args[1])) {
                    Diagnostic d("E_RESOURCE_COPY",
                                 "El recurso no copiable '" + program.nameOf(args[1]) +
                                     "' no puede almacenarse en " + program.nameOf(t),
                                 where);
                    d.example = "var arr: [" + program.nameOf(args[1]) + "] = []";
                    throw d;
                }
                return;
            }
            if (baseName == "@Set") {
                if (args.size() != 1)
                    fail("E_GENERIC", "Set<T> requiere exactamente 1 argumento de tipo", where, "Set<int>");
                known(args[0], where, false);
                if (!isHashableKey(args[0])) {
                    Diagnostic d("E_KEY_TYPE", "Tipo de elemento no hashable para Set: " + program.nameOf(args[0]) +
                                 " (usa int, str, bool, Handle<T> o enum sin campos)", where);
                    d.expected = "int | str | bool | Handle<T> | enum";
                    d.actual = program.nameOf(args[0]);
                    d.example = "Set<int>";
                    throw d;
                }
                return;
            }
            if (baseName == "@Pool" || baseName == "@Handle" || baseName == "@Shared" ||
                baseName == "@Task" || baseName == "@Frozen") {
                if (args.size() != 1) {
                    const std::string kind = baseName.substr(1);
                    fail("E_GENERIC", kind + "<T> requiere exactamente 1 argumento de tipo", where, kind + "<int>");
                }
                known(args[0], where, baseName == "@Task");
                if ((baseName == "@Pool" || baseName == "@Shared" || baseName == "@Frozen") &&
                    program.isResource(args[0])) {
                    Diagnostic d("E_RESOURCE_COPY",
                                 "El recurso no copiable '" + program.nameOf(args[0]) +
                                     "' no puede almacenarse en " + program.nameOf(t),
                                 where);
                    d.example = "var p = [" + program.nameOf(args[0]) + "]";
                    throw d;
                }
                return;
            }
            if (baseName == "@Dyn") {
                if (args.size() != 1)
                    fail("E_DYN_TYPE", "dyn requiere exactamente una interfaz", where, "dyn Shape");
                const Type iface = args[0];
                const bool found = std::any_of(program.interfaces.begin(), program.interfaces.end(),
                                               [iface](const InterfaceDecl& id) { return id.type == iface; });
                if (!found)
                    fail("E_DYN_TYPE", "dyn requiere una interfaz declarada: " + program.nameOf(iface),
                         where, "interface Shape { fn area(self) -> int }\nlet s: dyn Shape = ...");
                return;
            }
            if (baseName == "@Fn") {
                if (args.empty())
                    fail("E_TYPE", "Tipo de función inválido", where, "fn(int) -> int");
                known(args[0], where, true);
                for (std::size_t i = 1; i < args.size(); ++i) known(args[i], where, false);
                return;
            }
            if (baseName == "@Ptr" || baseName == "@MutPtr") {
                if (args.size() != 1) {
                    const std::string kind = baseName.substr(1);
                    fail("E_GENERIC", kind + "<T> requiere exactamente 1 argumento de tipo", where, kind + "<u8>");
                }
                known(args[0], where, true);
                return;
            }
            if (program.isSimd(t)) {
                known(args[0], where, false);
                validateSimdType(program, t, where);
                return;
            }
            if (baseName == "@Atomic") {
                if (args.size() != 1)
                    fail("E_GENERIC", "Atomic<T> requiere exactamente 1 argumento de tipo", where, "Atomic<int>");
                known(args[0], where, false);
                if (!mentionsOpaqueType(program, args[0]) && args[0] != Type::Bool && !Program::isIntType(args[0])) {
                    Diagnostic d("E_ATOMIC_TYPE",
                                 "Atomic<T> solo admite bool o tipos enteros (i8..u64, int), no '" + program.nameOf(args[0]) + "'.",
                                 where);
                    d.expected = "bool | int | i8..u64";
                    d.actual = program.nameOf(args[0]);
                    d.example = "let a = atomicNew(0)";
                    throw d;
                }
                return;
            }
            if (baseName == "@RingQueue") {
                if (args.size() != 1)
                    fail("E_GENERIC", "RingQueue<T> requiere exactamente 1 argumento de tipo", where, "RingQueue<int>");
                known(args[0], where, false);
                if (!mentionsOpaqueType(program, args[0])) {
                    if (program.isResource(args[0])) {
                        Diagnostic d("E_RESOURCE_COPY",
                                     "El recurso no copiable '" + program.nameOf(args[0]) +
                                         "' no puede almacenarse en " + program.nameOf(t),
                                     where);
                        d.example = "let q = ringNew<int>(16)";
                        throw d;
                    }
                    if (!program.isSendable(args[0])) {
                        Diagnostic d("E_SEND",
                                     "RingQueue<T> solo admite tipos enviables entre hilos; '" +
                                         program.nameOf(args[0]) + "' no es Send.",
                                     where);
                        d.actual = program.nameOf(args[0]);
                        d.example = "let q = ringNew<Frozen<Data>>(16)";
                        throw d;
                    }
                }
                return;
            }
            auto it = genericDecls.find(baseName);
            if (it == genericDecls.end()) {
                for (const auto& d : program.data) {
                    if (program.nameOf(d.type) == baseName)
                        fail("E_GENERIC", "El tipo no admite parámetros genéricos: " + baseName, where, baseName, d.span);
                }
                const auto sb = shortTypeName(baseName);
                if (isCollectionAlias(sb))
                    failCollectionAlias(program, baseName, where);
                fail("E_UNKNOWN_TYPE", "Tipo desconocido: " + baseName, where, "struct Point { x: int }");
            }
            const DataDecl& tmpl = it->second;
            if (args.size() != tmpl.generics.size())
                fail("E_GENERIC", "Cantidad incorrecta de argumentos de tipo para " + baseName, where, baseName + "<int>", tmpl.span);
            if (instantiationCount++ >= 4096)
                fail("E_LAYOUT", "Se superó el límite de 4096 instanciaciones de tipos genéricos", where, "");
            std::map<Type, Type> mapping;
            for (std::size_t i = 0; i < args.size(); ++i) {
                known(args[i], where, false);
                mapping.emplace(tmpl.generics[i], args[i]);
            }
            DataDecl inst = tmpl;
            inst.type = t;
            inst.generics.clear();
            for (auto& v : inst.variants) {
                for (auto& f : v.fields) f.type = substituteTypeMap(f.type, mapping, program);
            }
            const std::size_t idx = program.data.size();
            program.data.push_back(std::move(inst));
            registerConcreteAt(idx);
            const std::size_t nv = program.data[idx].variants.size();
            for (std::size_t vi = 0; vi < nv; ++vi) {
                const std::size_t nf = program.data[idx].variants[vi].fields.size();
                for (std::size_t fi = 0; fi < nf; ++fi) {
                    const Type ft = program.data[idx].variants[vi].fields[fi].type;
                    const Span fspan = program.data[idx].variants[vi].fields[fi].span;
                    ensureInstantiated(ft, fspan);
                }
            }
        }
    }

    void known(Type type, Span where, bool allowVoid = false) {
        if (Program::isNumericType(type) || type == Type::Bool || type == Type::Str ||
            type == Type::IntArray || type == Type::StringBuilder || Program::isPrimitiveResource(type)) return;
        if (program.isArray(type)) {
            known(program.elemOf(type), where, false);
            return;
        }
        if (program.typeAppOf(type)) {
            ensureInstantiated(type, where);
            return;
        }
        if (type == Type::Void) {
            if (!allowVoid) requireValue(type, where);
            return;
        }
        if (!declIndices.contains(type)) {
            const std::string tname = program.nameOf(type);
            const auto sb = shortTypeName(tname);
            if (isCollectionAlias(sb))
                failCollectionAlias(program, tname, where);
            fail("E_UNKNOWN_TYPE", "Tipo desconocido: " + tname, where, "struct Point { x: int }");
        }
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
        if (Program::isPrimitiveResource(type) || types.program.isHandle(type) ||
            types.program.isShared(type) || types.program.isTask(type) || types.program.isFrozen(type) ||
            types.program.isAtomic(type) || types.program.isRingQueue(type) ||
            types.program.isAnyPtr(type))
            return {8, 0};
        if (types.program.isSimd(type)) {
            const Type elem = types.program.simdElem(type);
            if (mentionsOpaqueType(types.program, elem)) return {16, 0};
            return {static_cast<std::size_t>(simdElemBytes(elem)) * types.program.simdLanes(type), 0};
        }
        if (type < Type::NamedBase || types.program.isArray(type)) return {16, 0};
        if (types.program.isPool(type)) return {24, 0};
        if (types.program.isDyn(type) || types.program.isFn(type)) return {16, 0};
        if (types.program.isMap(type) || types.program.isSet(type)) return {32, 0};
        if (path.contains(type))
            fail("E_TYPE_CYCLE", "Ciclo de campos por valor (para estructuras enlazadas o cíclicas usa Handle<T> en Pool<T> o Shared<T>)",
                 where, "struct Node { val: int, next: Handle<Node>? }");
        if (path.size() >= 32) fail("E_LAYOUT", "Tipo con más de 32 niveles", where, "struct Leaf { n: int }");
        if (auto it = sizes.find(type); it != sizes.end()) return it->second;
        if (!types.hasDecl(type))
            fail("E_UNKNOWN_TYPE", "Tipo desconocido: " + types.program.nameOf(type), where, "struct Point { x: int }");
        path.insert(type);
        const auto& d = types.decl(type);
        Size result{16, 1};
        for (const auto& v : d.variants) {
            if (d.isEnum) result.bytes += 16;
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

bool isSelf(const Program& program, Type type) {
    return type >= Type::NamedBase && program.nameOf(type) == "Self";
}

struct Interfaces {
    Program& program;
    Types& types;
    std::unordered_map<Type, const InterfaceDecl*> declarations;
    std::map<std::pair<Type, Type>, const ImplDecl*> implementations;
    std::map<std::pair<Type, Type>, std::unordered_map<std::string, std::string>> dispatch;
    std::unordered_map<Type, std::unordered_map<std::string, std::string>> inherentMethods;
    struct GenericImplEntry { std::size_t implIdx; const ImplDecl* decl; };
    std::vector<GenericImplEntry> genericInherentImpls;
    std::vector<GenericImplEntry> genericInterfaceImpls;
    std::set<std::pair<std::size_t, std::vector<Type>>> instantiatedGenericImpls;
    std::set<std::pair<Type, Type>> activeImplChecks;
    std::deque<Function> pendingMethods;
    std::vector<Function> abstractChecks;
    Functions* fnTable = nullptr;
    std::size_t nextDynId = 0;

    void known(Type type, const std::string& module, Span where, bool allowVoid = false, bool allowSelf = false,
               const std::vector<Type>& scopeGenerics = {}) {
        if (allowSelf && isSelf(program, type)) return;
        if (std::find(scopeGenerics.begin(), scopeGenerics.end(), type) != scopeGenerics.end()) return;
        if (Program::isNumericType(type) || type == Type::Bool || type == Type::Str ||
            type == Type::IntArray || type == Type::StringBuilder || Program::isPrimitiveResource(type)) return;
        if (program.isArray(type)) {
            known(program.elemOf(type), module, where, false, allowSelf, scopeGenerics);
            return;
        }
        if (const auto* app = program.typeAppOf(type)) {
            const std::string base = app->baseName;
            const std::vector<Type> args = app->args;
            if (base == "@Dyn") {
                types.ensureInstantiated(type, where);
                checkDynInType(type, module, where, true);
                return;
            }
            if (base == "@Fn") {
                if (args.empty())
                    fail("E_TYPE", "Tipo de función inválido", where, "fn(int) -> int");
                known(args[0], module, where, true, allowSelf, scopeGenerics);
                for (std::size_t i = 1; i < args.size(); ++i)
                    known(args[i], module, where, false, allowSelf, scopeGenerics);
                const bool hasScopeGen = std::any_of(scopeGenerics.begin(), scopeGenerics.end(),
                                                     [&](Type g) { return mentionsType(program, type, g); });
                if ((!allowSelf || !containsSelf(program, type)) && !hasScopeGen) {
                    types.ensureInstantiated(type, where);
                    checkDynInType(type, module, where, true);
                }
                return;
            }
            if (!base.starts_with("@") && !program.visible(module, base))
                fail("E_IMPORT", "Tipo no importado directamente: " + base, where, "import model");
            for (Type a : args) known(a, module, where, base == "@Task", allowSelf, scopeGenerics);
            const bool hasScopeGen = std::any_of(scopeGenerics.begin(), scopeGenerics.end(),
                                                 [&](Type g) { return mentionsType(program, type, g); });
            if ((!allowSelf || !containsSelf(program, type)) && !hasScopeGen)
                types.ensureInstantiated(type, where);
            return;
        }
        if (type == Type::Void) {
            if (!allowVoid) requireValue(type, where);
            return;
        }
        if (!program.visible(module, program.nameOf(type)))
            fail("E_IMPORT", "Tipo no importado directamente: " + program.nameOf(type), where, "import model");
        if (!types.hasDecl(type))
            fail("E_UNKNOWN_TYPE", "Tipo desconocido: " + program.nameOf(type), where, "struct Point { x: int }");
    }

    Type substituteSelf(Type type, Type target) const {
        if (isSelf(program, type)) return target;
        if (program.isArray(type)) return program.arrayOf(substituteSelf(program.elemOf(type), target));
        if (const auto* app = program.typeAppOf(type)) {
            const std::string base = app->baseName;
            std::vector<Type> args = app->args;
            for (Type& a : args) a = substituteSelf(a, target);
            return program.typeApp(base, args);
        }
        return type;
    }

    void checkDynSafety(Type iface, Span where) const {
        auto it = declarations.find(iface);
        if (it == declarations.end())
            fail("E_DYN_TYPE", "dyn requiere una interfaz declarada: " + program.nameOf(iface), where, "dyn Shape");
        const auto& decl = *it->second;
        for (const auto& m : decl.methods) {
            if (m.params.empty() || !isSelf(program, m.params[0].type))
                fail("E_DYN_SAFETY", "Para usar dyn " + program.nameOf(iface) +
                     ", el primer parámetro de " + m.sourceName + " debe ser self o mut self",
                     where, "interface Shape { fn area(self) -> int }", m.span);
            for (std::size_t pi = 1; pi < m.params.size(); ++pi) {
                if (containsSelf(program, m.params[pi].type))
                    fail("E_DYN_SAFETY", "Para usar dyn " + program.nameOf(iface) +
                         ", el método " + m.sourceName + " no puede mencionar Self en otros parámetros",
                         where, "interface Shape { fn area(self) -> int }", m.span);
            }
            if (containsSelf(program, m.result))
                fail("E_DYN_SAFETY", "Para usar dyn " + program.nameOf(iface) +
                     ", el método " + m.sourceName + " no puede devolver Self",
                     where, "interface Shape { fn area(self) -> int }", m.span);
        }
    }

    void checkDynInType(Type type, const std::string& module, Span where, bool checkVisibility = true) const {
        if (program.isArray(type)) { checkDynInType(program.elemOf(type), module, where, checkVisibility); return; }
        if (program.isDyn(type)) {
            const Type iface = program.dynInterface(type);
            if (checkVisibility && !program.visible(module, program.nameOf(iface)))
                fail("E_IMPORT", "Interfaz no importada directamente: " + program.nameOf(iface), where, "import contracts");
            checkDynSafety(iface, where);
            return;
        }
        if (const auto* app = program.typeAppOf(type)) {
            for (Type a : app->args) checkDynInType(a, module, where, checkVisibility);
        }
    }

    std::string ownerModuleOf(Type target) const {
        if (const auto* app = program.typeAppOf(target)) {
            if (auto it = types.genericDecls.find(app->baseName); it != types.genericDecls.end())
                return it->second.module;
        }
        if (types.hasDecl(target)) return types.decl(target).module;
        return {};
    }

    bool isUserAggregate(Type target) const {
        if (target < Type::NamedBase || program.isArray(target) || program.isMap(target) ||
            program.isSet(target) || program.isPool(target) || program.isHandle(target) ||
            program.isShared(target) || program.isDyn(target) || program.isOption(target) || program.isResult(target))
            return false;
        return types.hasDecl(target);
    }

    static bool unifyPattern(const Program& p, Type pat, Type actual, std::map<Type, Type>& mapping) {
        if (auto it = mapping.find(pat); it != mapping.end()) {
            if (it->second == Type::Unknown) { it->second = actual; return true; }
            return it->second == actual;
        }
        if (p.isArray(pat)) return p.isArray(actual) && unifyPattern(p, p.elemOf(pat), p.elemOf(actual), mapping);
        const auto* pa = p.typeAppOf(pat);
        const auto* aa = p.typeAppOf(actual);
        if (pa && aa && pa->baseName == aa->baseName && pa->args.size() == aa->args.size()) {
            for (std::size_t k = 0; k < pa->args.size(); ++k)
                if (!unifyPattern(p, pa->args[k], aa->args[k], mapping)) return false;
            return true;
        }
        return pat == actual;
    }

    bool sameInterfaceSignature(const Function& actual, const Function& expected, Type target) const {
        if ((expected.reqPure && !actual.reqPure) ||
            (expected.reqNoAlloc && !actual.reqNoAlloc) ||
            (expected.reqCheckFree && !actual.reqCheckFree))
            return false;
        if (actual.params.size() != expected.params.size()) return false;
        if (substituteSelf(actual.result, target) != substituteSelf(expected.result, target)) return false;
        for (std::size_t i = 0; i < actual.params.size(); ++i) {
            if (actual.params[i].writable != expected.params[i].writable ||
                actual.params[i].byRef != expected.params[i].byRef ||
                substituteSelf(actual.params[i].type, target) != substituteSelf(expected.params[i].type, target))
                return false;
        }
        return true;
    }

    void validateInterfaceImplMethods(const ImplDecl& declaration, const InterfaceDecl& ifaceDecl,
                                      Type rawTarget, const std::vector<Type>& scopeGenerics,
                                      bool instantiateChecks = false) {
        std::unordered_map<std::string, const Function*> methods;
        for (const auto& method : declaration.methods) {
            if (!methods.emplace(method.sourceName, &method).second)
                fail("E_DUP_FUNCTION", "Método duplicado en impl: " + method.sourceName, method.span,
                     "fn area(self) -> int { return 0 }");
            if (std::none_of(ifaceDecl.methods.begin(), ifaceDecl.methods.end(),
                    [&](const Function& expected) { return expected.sourceName == method.sourceName; }))
                fail("E_IMPL_METHOD_EXTRA", "Método adicional en impl: " + method.sourceName, method.span,
                     "fn area(self) -> int { return 0 }");
        }
        for (const auto& expected : ifaceDecl.methods) {
            auto found = methods.find(expected.sourceName);
            if (found == methods.end()) {
                if (!expected.hasDefaultBody)
                    fail("E_IMPL_METHOD_MISSING", "Falta el método " + expected.sourceName + " en impl",
                         declaration.span, "fn " + expected.sourceName + "(self) {}", expected.span);
                continue;
            }
            const auto& actual = *found->second;
            if (!sameInterfaceSignature(actual, expected, rawTarget))
                fail("E_IMPL_SIGNATURE", "La firma del método no coincide tras sustituir Self: " + expected.sourceName,
                     actual.span, "fn " + expected.sourceName + "(self) {}", expected.span);
            if (instantiateChecks) {
                known(substituteSelf(actual.result, rawTarget), declaration.module, actual.span, true, false, scopeGenerics);
                for (const auto& param : actual.params)
                    known(substituteSelf(param.type, rawTarget), declaration.module, param.span, false, false, scopeGenerics);
            }
        }
    }

    Function makeConcreteMethod(const Function& raw, const std::map<Type, Type>& mapping, const std::string& fnName,
                                const std::string& modName, const std::vector<std::pair<Type, Type>>& constraints,
                                const std::vector<Type>& subTypes, bool markSpec) {
        Function copy = raw;
        copy.name = fnName;
        copy.module = modName;
        copy.isImpl = true;
        copy.isSpecialization = markSpec;
        copy.substitutedTypes = subTypes;
        copy.methodConstraints = constraints;
        substituteFnMap(copy, mapping, program);
        return copy;
    }

    Interfaces(Program& input, Types& declaredTypes) : program(input), types(declaredTypes) {
        for (const auto& declaration : program.interfaces) {
            const auto ifaceName = program.nameOf(declaration.type);
            if (isReservedTypeName(ifaceName))
                fail("E_DUP_TYPE", "Tipo reservado del núcleo: " + ifaceName, declaration.span, "interface MyBuf { fn build(self) -> str }");
            if (types.hasDecl(declaration.type) || types.genericDecls.contains(ifaceName))
                fail("E_DUP_TYPE", "La interfaz comparte nombre con un tipo de datos", declaration.span, "struct Item { score: int }");
            if (!declarations.emplace(declaration.type, &declaration).second)
                fail("E_DUP_INTERFACE", "Interfaz duplicada: " + ifaceName, declaration.span,
                     "interface Scored { fn scoreOf(value: Self) -> int }");
        }
        for (const auto& declaration : program.interfaces) {
            const auto ifaceName = program.nameOf(declaration.type);
            std::unordered_map<std::string, Span> names;
            bool hasDefaults = false;
            for (const auto& method : declaration.methods) {
                if (!method.generics.empty())
                    fail("E_INTERFACE", "Los métodos de interfaz no son genéricos", method.span,
                         "fn scoreOf(value: Self) -> int");
                if (!names.emplace(method.sourceName, method.span).second)
                    fail("E_DUP_FUNCTION", "Método de interfaz duplicado: " + method.sourceName, method.span,
                         "fn scoreOf(value: Self) -> int");
                bool hasSelf = false;
                for (const auto& param : method.params) {
                    hasSelf = hasSelf || containsSelf(program, param.type);
                    known(param.type, declaration.module, param.span, false, true);
                    checkDynInType(param.type, declaration.module, param.span);
                }
                if (!hasSelf)
                    fail("E_INTERFACE", "Self debe aparecer en parámetros", method.span,
                         "fn scoreOf(value: Self) -> int");
                known(method.result, declaration.module, method.span, true, true);
                checkDynInType(method.result, declaration.module, method.span);
                hasDefaults = hasDefaults || method.hasDefaultBody ||
                              !method.requiresClauses.empty() || !method.ensuresClauses.empty();
            }
            if (hasDefaults) {
                const Type selfKey = program.intern("Self");
                const Type opaqueSelf = program.intern("Self@iface." + ifaceName);
                program.data.push_back({opaqueSelf, false, declaration.span,
                                        {{program.nameOf(opaqueSelf), declaration.span, {}}}, declaration.module});
                types.syncOpaque();
                std::map<Type, Type> mapping{{selfKey, opaqueSelf}};
                for (const auto& method : declaration.methods) {
                    if (!method.hasDefaultBody && method.requiresClauses.empty() && method.ensuresClauses.empty())
                        continue;
                    abstractChecks.push_back(makeConcreteMethod(
                        method, mapping, "@abstract.iface." + ifaceName + "." + method.sourceName,
                        declaration.module, {{opaqueSelf, declaration.type}}, {opaqueSelf}, true));
                }
            }
        }
        for (const auto& d : program.data) {
            if (isOpaqueType(program, d.type)) continue;
            for (const auto& v : d.variants)
                for (const auto& f : v.fields)
                    checkDynInType(f.type, d.module, f.span);
        }
        for (const auto& [name, tmpl] : types.genericDecls) {
            (void)name;
            for (const auto& v : tmpl.variants)
                for (const auto& f : v.fields)
                    checkDynInType(f.type, tmpl.module, f.span);
        }
        for (const auto& fn : program.functions)
            for (Type constraint : fn.constraints) {
                if (constraint == Type::Unknown) continue;
                if (!declarations.contains(constraint))
                    fail("E_INTERFACE_UNKNOWN", "Interfaz desconocida: " + program.nameOf(constraint), fn.span,
                         "interface Scored { fn scoreOf(value: Self) -> int }");
                if (!program.visible(fn.module, program.nameOf(constraint)))
                    fail("E_IMPORT", "Interfaz no importada directamente: " + program.nameOf(constraint), fn.span,
                         "import contracts");
            }
        const Type selfKey = program.intern("Self");
        for (std::size_t index = 0; index < program.impls.size(); ++index) {
            const auto& declaration = program.impls[index];
            const bool isInherent = (declaration.interface == Type::Unknown);
            const InterfaceDecl* ifaceDecl = nullptr;
            if (!isInherent) {
                auto ifaceIt = declarations.find(declaration.interface);
                if (ifaceIt == declarations.end())
                    fail("E_INTERFACE_UNKNOWN", "Interfaz desconocida en impl: " + program.nameOf(declaration.interface),
                         declaration.span, "interface Scored { fn scoreOf(value: Self) -> int }");
                if (!program.visible(declaration.module, program.nameOf(declaration.interface)))
                    fail("E_IMPORT", "Interfaz no importada directamente: " + program.nameOf(declaration.interface),
                         declaration.span, "import contracts");
                ifaceDecl = ifaceIt->second;
            }
            if (!declaration.generics.empty()) {
                std::set<Type> seen;
                for (std::size_t gi = 0; gi < declaration.generics.size(); ++gi) {
                    const Type g = declaration.generics[gi];
                    const auto gName = program.nameOf(g);
                    if (!seen.insert(g).second || types.hasDecl(g) || types.genericDecls.contains(gName))
                        fail("E_DUP_GENERIC", "El parámetro de tipo debe ser único en impl", declaration.span,
                             "impl<T> Stack<T> { ... }");
                    if (!mentionsType(program, declaration.target, g))
                        fail("E_GENERIC", "El parámetro genérico de impl debe aparecer en el tipo destino",
                             declaration.span, "impl<T> Stack<T> { ... }");
                    const Type c = declaration.constraints[gi];
                    if (c != Type::Unknown) {
                        if (!declarations.contains(c))
                            fail("E_INTERFACE_UNKNOWN", "Interfaz desconocida: " + program.nameOf(c),
                                 declaration.span, "impl<T: Shape> Box<T> { ... }");
                        if (!program.visible(declaration.module, program.nameOf(c)))
                            fail("E_IMPORT", "Interfaz no importada directamente: " + program.nameOf(c),
                                 declaration.span, "import contracts");
                    }
                }
                const auto* targetApp = program.typeAppOf(declaration.target);
                if (isInherent) {
                    if (!targetApp || !types.genericDecls.contains(targetApp->baseName))
                        fail("E_IMPL_INHERENT", "impl<T> requiere un struct o enum genérico declarado",
                             declaration.span, "impl<T> Stack<T> { ... }");
                    const std::string ownerMod = ownerModuleOf(declaration.target);
                    if (ownerMod != declaration.module)
                        fail("E_IMPL_COHERENCE", "Solo se pueden definir métodos inherentes en el módulo donde se declara el tipo (" +
                             ownerMod + ")", declaration.span, "impl<T> Stack<T> { ... }");
                }
                std::map<Type, Type> gMapping;
                std::vector<Type> opaqueArgs;
                std::vector<std::pair<Type, Type>> absConstraints;
                for (std::size_t gi = 0; gi < declaration.generics.size(); ++gi) {
                    const Type g = declaration.generics[gi];
                    const Type og = program.intern(program.nameOf(g) + "@impl." + std::to_string(index));
                    program.data.push_back({og, false, declaration.span,
                                            {{program.nameOf(og), declaration.span, {}}}, declaration.module});
                    types.syncOpaque();
                    gMapping.emplace(g, og);
                    opaqueArgs.push_back(og);
                    if (declaration.constraints[gi] != Type::Unknown)
                        absConstraints.emplace_back(og, declaration.constraints[gi]);
                }
                const Type abstractTarget = substituteTypeMap(declaration.target, gMapping, program);
                if (abstractTarget == Type::Void || isSelf(program, abstractTarget))
                    fail("E_INTERFACE", "El destino de impl debe ser un tipo con valor", declaration.span,
                         "impl<T> Stack<T> { ... }");
                known(abstractTarget, declaration.module, declaration.span);
                gMapping.emplace(selfKey, abstractTarget);
                std::vector<Type> subTypes = opaqueArgs;
                subTypes.push_back(abstractTarget);
                if (isInherent) {
                    std::unordered_set<std::string> seenM;
                    for (const auto& m : declaration.methods) {
                        if (!seenM.insert(m.sourceName).second)
                            fail("E_DUP_FUNCTION", "Método duplicado en impl: " + m.sourceName, m.span,
                                 "fn len(self) -> int { return len(self.items) }");
                        const Type resT = substituteTypeMap(m.result, gMapping, program);
                        known(resT, declaration.module, m.span, true);
                        checkDynInType(resT, declaration.module, m.span);
                        for (const auto& p : m.params) {
                            const Type pt = substituteTypeMap(p.type, gMapping, program);
                            known(pt, declaration.module, p.span);
                            checkDynInType(pt, declaration.module, p.span);
                        }
                        abstractChecks.push_back(makeConcreteMethod(
                            m, gMapping, "@abstract.impl." + std::to_string(index) + "." + m.sourceName,
                            declaration.module, absConstraints, subTypes, true));
                    }
                    genericInherentImpls.push_back({index, &declaration});
                } else {
                    validateInterfaceImplMethods(declaration, *ifaceDecl, declaration.target, declaration.generics, false);
                    absConstraints.emplace_back(abstractTarget, declaration.interface);
                    std::unordered_map<std::string, const Function*> byName;
                    for (const auto& m : declaration.methods) byName.emplace(m.sourceName, &m);
                    for (const auto& expected : ifaceDecl->methods) {
                        auto found = byName.find(expected.sourceName);
                        const Function& srcFn = found != byName.end() ? *found->second : expected;
                        const std::string srcMod = found != byName.end() ? declaration.module : ifaceDecl->module;
                        auto cm = makeConcreteMethod(
                            srcFn, gMapping, "@abstract.impl." + std::to_string(index) + "." + expected.sourceName,
                            srcMod, absConstraints, subTypes, true);
                        known(cm.result, srcMod, cm.span, true);
                        checkDynInType(cm.result, srcMod, cm.span);
                        for (const auto& p : cm.params) {
                            known(p.type, srcMod, p.span);
                            checkDynInType(p.type, srcMod, p.span);
                        }
                        abstractChecks.push_back(std::move(cm));
                    }
                    genericInterfaceImpls.push_back({index, &declaration});
                }
                continue;
            }
            if (declaration.target == Type::Void || isSelf(program, declaration.target))
                fail("E_INTERFACE", "El destino de impl debe ser un tipo con valor", declaration.span,
                     "impl Rect { fn area(self) -> int { return self.w * self.h } }");
            known(declaration.target, declaration.module, declaration.span);
            if (isInherent) {
                if (!isUserAggregate(declaration.target))
                    fail("E_IMPL_INHERENT", "Las impl inherentes requieren un struct o enum declarado; para tipos del núcleo usa impl Interfaz for Tipo",
                         declaration.span, "struct Point { x: int }\nimpl Point { fn getX(self) -> int { return self.x } }");
                const std::string ownerMod = ownerModuleOf(declaration.target);
                if (ownerMod != declaration.module)
                    fail("E_IMPL_COHERENCE", "Solo se pueden definir métodos inherentes en el módulo donde se declara el tipo (" +
                         ownerMod + ")", declaration.span, "impl Point { ... }");
                std::map<Type, Type> mapping{{selfKey, declaration.target}};
                auto& methMap = inherentMethods[declaration.target];
                for (const auto& m : declaration.methods) {
                    const std::string fnName = "@method." + std::to_string(index) + "." + m.sourceName;
                    if (!methMap.emplace(m.sourceName, fnName).second)
                        fail("E_DUP_FUNCTION", "Método duplicado para " + program.nameOf(declaration.target) + ": " + m.sourceName,
                             m.span, "fn other(self) {}");
                    auto cm = makeConcreteMethod(m, mapping, fnName, declaration.module, {}, {declaration.target}, false);
                    known(cm.result, declaration.module, cm.span, true);
                    checkDynInType(cm.result, declaration.module, cm.span);
                    for (const auto& p : cm.params) {
                        known(p.type, declaration.module, p.span);
                        checkDynInType(p.type, declaration.module, p.span);
                    }
                    program.functions.push_back(std::move(cm));
                }
                continue;
            }
            const auto key = std::make_pair(declaration.interface, declaration.target);
            if (!implementations.emplace(key, &declaration).second)
                fail("E_DUP_IMPL", "Impl duplicada para " + program.nameOf(declaration.interface) + " y " +
                     program.nameOf(declaration.target), declaration.span,
                     "impl contracts.Scored for Item { fn scoreOf(value: Item) -> int { return value.score } }");
            validateInterfaceImplMethods(declaration, *ifaceDecl, declaration.target, {}, true);
            std::unordered_map<std::string, const Function*> byName;
            for (const auto& m : declaration.methods) byName.emplace(m.sourceName, &m);
            std::map<Type, Type> mapping{{selfKey, declaration.target}};
            auto& methods = dispatch[key];
            std::vector<std::string> orderedNames;
            for (const auto& expected : ifaceDecl->methods) {
                auto found = byName.find(expected.sourceName);
                const bool fromDefault = (found == byName.end());
                const Function& srcFn = fromDefault ? expected : *found->second;
                const std::string fnName = "@impl." + std::to_string(index) + "." + expected.sourceName;
                const std::string srcMod = fromDefault ? ifaceDecl->module : declaration.module;
                std::vector<std::pair<Type, Type>> constraints;
                if (fromDefault) constraints.emplace_back(declaration.target, declaration.interface);
                auto cm = makeConcreteMethod(srcFn, mapping, fnName, srcMod, constraints,
                                             {declaration.target}, fromDefault);
                checkDynInType(cm.result, srcMod, cm.span);
                for (const auto& p : cm.params) checkDynInType(p.type, srcMod, p.span);
                methods.emplace(expected.sourceName, fnName);
                orderedNames.push_back(fnName);
                program.functions.push_back(std::move(cm));
            }
            program.dynVtables.push_back({declaration.interface, declaration.target, std::move(orderedNames)});
        }
    }

    std::optional<std::string> ensureInherentMethod(Type target, const std::string& methodName, Span at) {
        if (auto tit = inherentMethods.find(target); tit != inherentMethods.end()) {
            if (auto mit = tit->second.find(methodName); mit != tit->second.end()) return mit->second;
        }
        const auto* app = program.typeAppOf(target);
        if (!app || !fnTable) return {};
        const Type selfKey = program.intern("Self");
        std::optional<std::pair<Type, Type>> failedConstraint;
        for (const auto& entry : genericInherentImpls) {
            const ImplDecl& tmpl = *entry.decl;
            std::map<Type, Type> gMapping;
            for (Type g : tmpl.generics) gMapping.emplace(g, Type::Unknown);
            if (!unifyPattern(program, tmpl.target, target, gMapping)) continue;
            bool allResolved = true;
            std::vector<Type> concreteArgs;
            for (Type g : tmpl.generics) {
                Type resolved = gMapping.at(g);
                if (resolved == Type::Unknown) { allResolved = false; break; }
                concreteArgs.push_back(resolved);
            }
            if (!allResolved) continue;
            bool hasNamedMethod = std::any_of(tmpl.methods.begin(), tmpl.methods.end(),
                                              [&](const Function& f) { return f.sourceName == methodName; });
            bool constraintsOk = true;
            std::vector<std::pair<Type, Type>> constraints;
            for (std::size_t gi = 0; gi < tmpl.generics.size(); ++gi) {
                if (tmpl.constraints[gi] == Type::Unknown) continue;
                if (!ensureInterfaceImpl(tmpl.constraints[gi], concreteArgs[gi], at)) {
                    constraintsOk = false;
                    if (hasNamedMethod && !failedConstraint)
                        failedConstraint = std::make_pair(tmpl.constraints[gi], concreteArgs[gi]);
                    break;
                }
                constraints.emplace_back(concreteArgs[gi], tmpl.constraints[gi]);
            }
            if (!constraintsOk) continue;
            if (!instantiatedGenericImpls.emplace(entry.implIdx, concreteArgs).second) continue;
            std::map<Type, Type> fullMap = gMapping;
            fullMap.emplace(selfKey, target);
            std::vector<Type> subTypes = concreteArgs;
            subTypes.push_back(target);
            const std::size_t batchId = nextDynId++;
            auto& methMap = inherentMethods[target];
            for (const auto& m : tmpl.methods) {
                const std::string fnName = "@method.g" + std::to_string(batchId) + "." + m.sourceName;
                if (!methMap.emplace(m.sourceName, fnName).second)
                    fail("E_DUP_FUNCTION", "Método duplicado para " + program.nameOf(target) + ": " + m.sourceName,
                         m.span, "fn other(self) {}");
                auto cm = makeConcreteMethod(m, fullMap, fnName, tmpl.module, constraints, subTypes, true);
                pendingMethods.push_back(std::move(cm));
                fnTable->emplace(fnName, &pendingMethods.back());
            }
        }
        if (auto tit = inherentMethods.find(target); tit != inherentMethods.end()) {
            if (auto mit = tit->second.find(methodName); mit != tit->second.end()) return mit->second;
        }
        if (failedConstraint) {
            fail("E_IMPL_MISSING", "Falta impl " + program.nameOf(failedConstraint->first) + " for " +
                 program.nameOf(failedConstraint->second), at, "impl Iface for Tipo { ... }");
        }
        return {};
    }

    bool ensureInterfaceImpl(Type iface, Type target, Span at) {
        const auto key = std::make_pair(iface, target);
        if (implementations.contains(key)) return true;
        if (isOpaqueType(program, target)) return false;
        const auto* app = program.typeAppOf(target);
        if (!app || !fnTable) return false;
        if (!activeImplChecks.insert(key).second) return false;
        struct Guard { std::set<std::pair<Type, Type>>& s; std::pair<Type, Type> k; ~Guard() { s.erase(k); } } guard{activeImplChecks, key};
        const Type selfKey = program.intern("Self");
        const InterfaceDecl& ifaceDecl = *declarations.at(iface);
        for (const auto& entry : genericInterfaceImpls) {
            const ImplDecl& tmpl = *entry.decl;
            if (tmpl.interface != iface) continue;
            std::map<Type, Type> gMapping;
            for (Type g : tmpl.generics) gMapping.emplace(g, Type::Unknown);
            if (!unifyPattern(program, tmpl.target, target, gMapping)) continue;
            bool allResolved = true;
            std::vector<Type> concreteArgs;
            for (Type g : tmpl.generics) {
                Type resolved = gMapping.at(g);
                if (resolved == Type::Unknown) { allResolved = false; break; }
                concreteArgs.push_back(resolved);
            }
            if (!allResolved) continue;
            bool constraintsOk = true;
            std::vector<std::pair<Type, Type>> constraints;
            for (std::size_t gi = 0; gi < tmpl.generics.size(); ++gi) {
                if (tmpl.constraints[gi] == Type::Unknown) continue;
                if (!ensureInterfaceImpl(tmpl.constraints[gi], concreteArgs[gi], at)) {
                    constraintsOk = false;
                    break;
                }
                constraints.emplace_back(concreteArgs[gi], tmpl.constraints[gi]);
            }
            if (!constraintsOk) continue;
            if (!instantiatedGenericImpls.emplace(entry.implIdx, concreteArgs).second) continue;
            implementations.emplace(key, &tmpl);
            constraints.emplace_back(target, iface);
            std::map<Type, Type> fullMap = gMapping;
            fullMap.emplace(selfKey, target);
            std::vector<Type> subTypes = concreteArgs;
            subTypes.push_back(target);
            std::unordered_map<std::string, const Function*> byName;
            for (const auto& m : tmpl.methods) byName.emplace(m.sourceName, &m);
            const std::size_t batchId = nextDynId++;
            auto& methods = dispatch[key];
            std::vector<std::string> orderedNames;
            for (const auto& expected : ifaceDecl.methods) {
                auto found = byName.find(expected.sourceName);
                const bool fromDefault = (found == byName.end());
                const Function& srcFn = fromDefault ? expected : *found->second;
                const std::string fnName = "@impl.g" + std::to_string(batchId) + "." + expected.sourceName;
                const std::string srcMod = fromDefault ? ifaceDecl.module : tmpl.module;
                auto cm = makeConcreteMethod(srcFn, fullMap, fnName, srcMod, constraints, subTypes, true);
                methods.emplace(expected.sourceName, fnName);
                orderedNames.push_back(fnName);
                pendingMethods.push_back(std::move(cm));
                fnTable->emplace(fnName, &pendingMethods.back());
            }
            program.dynVtables.push_back({iface, target, std::move(orderedNames)});
            return true;
        }
        return false;
    }

    const Function* findInterfaceMethod(Type target, const std::string& methodName, const std::string& fromModule, Span where) {
        if (fnTable && program.typeAppOf(target)) {
            for (const auto& entry : genericInterfaceImpls) {
                const Type iface = entry.decl->interface;
                if (!program.visible(fromModule, program.nameOf(iface))) continue;
                const auto& ifaceDecl = *declarations.at(iface);
                const bool hasMethod = std::any_of(ifaceDecl.methods.begin(), ifaceDecl.methods.end(),
                    [&](const Function& m) {
                        return m.sourceName == methodName && !m.params.empty() && isSelf(program, m.params[0].type);
                    });
                if (hasMethod) ensureInterfaceImpl(iface, target, where);
            }
        }
        const Function* matched = nullptr;
        for (const auto& [key, methodMap] : dispatch) {
            if (key.second != target) continue;
            if (!program.visible(fromModule, program.nameOf(key.first))) continue;
            auto mit = methodMap.find(methodName);
            if (mit == methodMap.end()) continue;
            const auto& ifaceDecl = *declarations.at(key.first);
            auto im = std::find_if(ifaceDecl.methods.begin(), ifaceDecl.methods.end(),
                                   [&](const Function& m) { return m.sourceName == methodName; });
            if (im == ifaceDecl.methods.end() || im->params.empty() || !isSelf(program, im->params[0].type))
                continue;
            if (matched)
                fail("E_OVERLOAD", "La llamada al método " + methodName + " es ambigua entre varias interfaces",
                     where, methodName);
            if (fnTable) matched = fnTable->at(mit->second);
        }
        return matched;
    }

    const Function* findInterfaceAssocMethod(Type target, const std::string& methodName, const std::string& fromModule, Span where) {
        if (fnTable && program.typeAppOf(target)) {
            for (const auto& entry : genericInterfaceImpls) {
                const Type iface = entry.decl->interface;
                if (!program.visible(fromModule, program.nameOf(iface))) continue;
                const auto& ifaceDecl = *declarations.at(iface);
                const bool hasMethod = std::any_of(ifaceDecl.methods.begin(), ifaceDecl.methods.end(),
                    [&](const Function& m) { return m.sourceName == methodName; });
                if (hasMethod) ensureInterfaceImpl(iface, target, where);
            }
        }
        const Function* matched = nullptr;
        for (const auto& [key, methodMap] : dispatch) {
            if (key.second != target) continue;
            if (!program.visible(fromModule, program.nameOf(key.first))) continue;
            auto mit = methodMap.find(methodName);
            if (mit == methodMap.end()) continue;
            if (matched)
                fail("E_OVERLOAD", "La llamada al método asociado " + methodName + " es ambigua entre varias interfaces",
                     where, methodName);
            if (fnTable) matched = fnTable->at(mit->second);
        }
        return matched;
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
    Types& types;
    Interfaces& interfaces;
    GenericInstances& generics;
    const std::unordered_map<Type, std::size_t>& sizes;
    struct LoopCtx { std::string label; bool isFor; };
    std::vector<LoopCtx> loopStack;
    struct CaptureState {
        std::vector<std::string> order;
        std::unordered_map<std::string, Type> types;
    };
    Functions* fnRegistry = nullptr;
    std::deque<Function>* pendingClosures = nullptr;
    std::unordered_map<std::string, std::size_t>* fnRefCache = nullptr;
    std::size_t* nextClosureId = nullptr;
    Checker* parentChecker = nullptr;
    CaptureState* activeCaptures = nullptr;
    Type* inferredReturn = nullptr;
    bool inAbstract = false;
    std::unordered_map<std::string, Variable> capturedVars{};
    struct SavedVarState { bool moved; Span movedAt; std::string movedTo; };
    using MoveSnapshot = std::vector<std::pair<std::size_t, std::unordered_map<std::string, SavedVarState>>>;
    struct LoopMoveFrame {
        std::string label;
        MoveSnapshot entry;
        std::vector<MoveSnapshot> breaks;
    };
    struct DestGuard {
        std::string& slot;
        std::string prev;
        DestGuard(std::string& s, std::string v) : slot(s), prev(std::move(s)) { slot = std::move(v); }
        ~DestGuard() { slot = std::move(prev); }
    };
    std::string activeMoveDest{};
    std::vector<LoopMoveFrame> loopMoveFrames{};
    bool pathTerminated = false;
    SafetyTier activeSafety = SafetyTier::Verified;
    bool inContract = false;
    bool inEnsures = false;
    Expr* preTypedArg0 = nullptr;

    static bool isTrivialCopyType(const Program& p, Type t) {
        if (Program::isNumericType(t) || t == Type::Bool ||
            p.isAnyPtr(t) || p.isHandle(t) || p.isSimd(t))
            return true;
        if (t >= Type::NamedBase) {
            if (const auto* d = p.declaration(t); d && d->isEnum) {
                return std::all_of(d->variants.begin(), d->variants.end(),
                                   [](const Variant& v) { return v.fields.empty(); });
            }
        }
        return false;
    }

    [[noreturn]] void failSafety(Span where, const std::string& action) const {
        Diagnostic d("E_SAFETY", action + " requiere un bloque o función 'trusted' o 'raw'.", where);
        d.example = "trusted { ... }";
        throw d;
    }

    void requireUnsafe(Span where, const std::string& action) const {
        if (activeSafety == SafetyTier::Verified) failSafety(where, action);
    }

    bool isRawMemSafeImpl(Type t, std::vector<Type>& seen) const {
        if (t == Type::Bool || Program::isNumericType(t) ||
            types.program.isAnyPtr(t) || types.program.isHandle(t) ||
            (types.program.isSimd(t) && Program::isNumericType(types.program.simdElem(t))))
            return true;
        if (t < Type::NamedBase || types.program.isArray(t) || types.program.isMap(t) ||
            types.program.isSet(t) || types.program.isPool(t) || types.program.isShared(t) ||
            types.program.isTask(t) || types.program.isFrozen(t) || types.program.isAtomic(t) ||
            types.program.isRingQueue(t) || types.program.isDyn(t) || types.program.isFn(t))
            return false;
        if (std::find(seen.begin(), seen.end(), t) != seen.end()) return true;
        seen.push_back(t);
        if (!types.hasDecl(t)) return false;
        const auto& d = types.decl(t);
        if (d.isEnum) {
            for (const auto& v : d.variants) {
                if (!v.fields.empty()) return false;
            }
            return true;
        }
        for (const auto& f : d.variants.at(0).fields) {
            if (!isRawMemSafeImpl(f.type, seen)) return false;
        }
        return true;
    }

    bool isRawMemSafeType(Type t) const {
        std::vector<Type> seen;
        return isRawMemSafeImpl(t, seen);
    }

    static bool isFfiSafeType(const Program& p, Type t, bool allowVoid) {
        if (allowVoid && t == Type::Void) return true;
        return t == Type::Bool || Program::isNumericType(t) || p.isAnyPtr(t);
    }

    MoveSnapshot saveMoveState() const {
        MoveSnapshot snap;
        snap.reserve(locals.levels.size());
        for (std::size_t lvl = 0; lvl < locals.levels.size(); ++lvl) {
            std::unordered_map<std::string, SavedVarState> levelMap;
            for (const auto& [name, v] : locals.levels[lvl]) {
                if (types.program.isResource(v.type))
                    levelMap.emplace(name, SavedVarState{v.moved, v.movedAt, v.movedTo});
            }
            if (!levelMap.empty()) snap.emplace_back(lvl, std::move(levelMap));
        }
        return snap;
    }

    void restoreMoveState(const MoveSnapshot& snap) {
        for (const auto& [lvl, levelMap] : snap) {
            if (lvl >= locals.levels.size()) continue;
            for (const auto& [name, st] : levelMap) {
                if (auto it = locals.levels[lvl].find(name); it != locals.levels[lvl].end()) {
                    it->second.moved = st.moved;
                    it->second.movedAt = st.movedAt;
                    it->second.movedTo = st.movedTo;
                }
            }
        }
    }

    static void mergeMoveState(MoveSnapshot& dst, const MoveSnapshot& other) {
        for (const auto& [lvl, oMap] : other) {
            for (auto& [dLvl, dMap] : dst) {
                if (dLvl != lvl) continue;
                for (const auto& [name, oSt] : oMap) {
                    if (auto dit = dMap.find(name); dit != dMap.end() && !dit->second.moved && oSt.moved)
                        dit->second = oSt;
                }
                break;
            }
        }
    }

    [[noreturn]] void failUseAfterMove(const std::string& name, Type t, Span useSpan,
                                         Span movedAt, const std::string& movedTo, Span declared) const {
        std::string msg = "El recurso '" + name + "' (" + types.program.nameOf(t) + ") ya fue movido";
        if (!movedTo.empty()) msg += " a " + movedTo;
        msg += " y no puede usarse después del movimiento.";
        Diagnostic d("E_USE_AFTER_MOVE", std::move(msg), useSpan);
        d.related.push_back(movedAt);
        if (declared.start != movedAt.start || declared.end != movedAt.end || declared.file != movedAt.file)
            d.related.push_back(declared);
        d.example = "let nuevo = " + name + "\n// usa 'nuevo' en lugar de '" + name + "', o reasigna '" + name + "'";
        throw d;
    }

    [[noreturn]] void failResourceCopy(const std::string& msg, Span where, std::optional<Span> rel = {}) const {
        Diagnostic d("E_RESOURCE_COPY", msg, where);
        if (rel) d.related.push_back(*rel);
        d.example = "Pasa 'mut x' para operar sin mover o mueve una sola vez un 'let'/'var' dueño.";
        throw d;
    }

    void checkLoopBackEdgeMoves(const MoveSnapshot& entry, Span errSpan) const {
        for (const auto& [lvl, eMap] : entry) {
            if (lvl >= locals.levels.size()) continue;
            for (const auto& [name, eSt] : eMap) {
                if (eSt.moved) continue;
                if (auto it = locals.levels[lvl].find(name); it != locals.levels[lvl].end() && it->second.moved)
                    failUseAfterMove(name, it->second.type, errSpan, it->second.movedAt, it->second.movedTo, it->second.declared);
            }
        }
    }

    LoopMoveFrame& findLoopMoveFrame(const std::string& label) {
        if (label.empty()) return loopMoveFrames.back();
        for (auto it = loopMoveFrames.rbegin(); it != loopMoveFrames.rend(); ++it)
            if (it->label == label) return *it;
        return loopMoveFrames.back();
    }

    void reserveValue(Type, Span) {}

    void requireType(Type actual, Type expected, Span span, std::optional<Span> related = {}) {
        checkTypes(types.program, actual, expected, span, related);
    }

    Type synthesizeFnRef(Expr& expr, const std::string& fnName) {
        visible(fnName, expr.span);
        const Function& callee = *functions.at(fnName);
        for (const auto& p : callee.params) {
            if (p.byRef)
                fail("E_CLOSURE_PARAM",
                     "No se puede usar directamente como valor una función con parámetro mut; envuélvela en |...| ...",
                     expr.span, "|x| " + fnName + "(...)", p.span);
        }
        known(callee.result, expr.span, true);
        std::vector<Type> paramTypes;
        paramTypes.reserve(callee.params.size());
        for (const auto& p : callee.params) {
            known(p.type, expr.span, false);
            paramTypes.push_back(p.type);
        }
        const Type fnType = types.program.fnOf(callee.result, paramTypes);
        if (inAbstract) {
            expr.kind = ExprKind::Closure;
            expr.text.clear();
            expr.args.clear();
            expr.closure = ClosurePtr(new ClosureExpr{callee.params, callee.result, {}, false, 0});
            expr.type = fnType;
            return fnType;
        }
        auto it = fnRefCache->find(fnName);
        std::size_t id = 0;
        if (it != fnRefCache->end()) {
            id = it->second;
        } else {
            id = (*nextClosureId)++;
            const std::string wrapName = "@fnref." + std::to_string(id);
            Expr innerCall{ExprKind::Call, expr.span, fnName, {}};
            std::vector<Param> wrapParams;
            wrapParams.reserve(callee.params.size());
            for (std::size_t pi = 0; pi < callee.params.size(); ++pi) {
                const std::string pName = "__p" + std::to_string(pi);
                wrapParams.push_back({pName, callee.params[pi].type, expr.span, false, false});
                innerCall.args.push_back({ExprKind::Name, expr.span, pName, {}, callee.params[pi].type});
            }
            innerCall.type = callee.result;
            Stmt bodyStmt{};
            bodyStmt.kind = callee.result == Type::Void ? StmtKind::Call : StmtKind::Return;
            bodyStmt.span = expr.span;
            bodyStmt.exprs.push_back(std::move(innerCall));
            Function wrapFn{};
            wrapFn.name = wrapName;
            wrapFn.sourceName = wrapName;
            wrapFn.module = callee.module;
            wrapFn.span = expr.span;
            wrapFn.params = std::move(wrapParams);
            wrapFn.result = callee.result;
            wrapFn.isImpl = true;
            wrapFn.body.push_back(std::move(bodyStmt));
            pendingClosures->push_back(std::move(wrapFn));
            fnRegistry->emplace(wrapName, &pendingClosures->back());
            if (types.program.closures.size() <= id) types.program.closures.resize(id + 1);
            types.program.closures[id] = {id, wrapName, fnType, Type::Void, paramTypes, callee.result};
            fnRefCache->emplace(fnName, id);
        }
        expr.kind = ExprKind::Closure;
        expr.text.clear();
        expr.args.clear();
        expr.closure = ClosurePtr(new ClosureExpr{callee.params, callee.result, {}, false, id});
        expr.type = fnType;
        return fnType;
    }

    static void rewriteClosureCaptures(std::vector<Stmt>& body, Type envStruct,
                                       const std::vector<std::string>& order,
                                       const std::vector<Type>& fieldTypes) {
        std::unordered_map<std::string, std::size_t> capIndex;
        for (std::size_t i = 0; i < order.size(); ++i) capIndex.emplace(order[i], i);
        std::vector<std::unordered_set<std::string>> scopes(1);
        auto isShadowed = [&](const std::string& name) {
            for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
                if (it->contains(name)) return true;
            return false;
        };
        auto rwExpr = [&](auto& selfE, auto& selfB, Expr& e) -> void {
            if (e.kind == ExprKind::Closure) {
                for (auto& capArg : e.args) selfE(selfE, selfB, capArg);
                return;
            }
            if (e.matchExpr) {
                selfE(selfE, selfB, e.matchExpr->scrutinee);
                for (auto& arm : e.matchExpr->arms) {
                    scopes.emplace_back();
                    for (const auto& n : arm.names)
                        if (n != "_") scopes.back().insert(n);
                    selfB(selfB, selfE, arm.body);
                    if (arm.resultExpr) selfE(selfE, selfB, *arm.resultExpr);
                    scopes.pop_back();
                }
                return;
            }
            if (e.ifExpr) {
                selfE(selfE, selfB, e.ifExpr->condition);
                selfB(selfB, selfE, e.ifExpr->thenBody);
                if (e.ifExpr->thenResult) selfE(selfE, selfB, *e.ifExpr->thenResult);
                selfB(selfB, selfE, e.ifExpr->elseBody);
                if (e.ifExpr->elseResult) selfE(selfE, selfB, *e.ifExpr->elseResult);
                return;
            }
            for (auto& a : e.args) selfE(selfE, selfB, a);
            if (e.kind == ExprKind::Name && !isShadowed(e.text)) {
                if (auto it = capIndex.find(e.text); it != capIndex.end()) {
                    const std::size_t fi = it->second;
                    Expr base{ExprKind::Name, e.span, "__env", {}, envStruct};
                    e = Expr{ExprKind::Field, e.span, e.text, {std::move(base)}, fieldTypes.at(fi)};
                    e.fieldIndices = {fi};
                }
            }
        };
        auto rwBlock = [&](auto& selfB, auto& selfE, std::vector<Stmt>& stmts) -> void {
            scopes.emplace_back();
            for (auto& s : stmts) {
                switch (s.kind) {
                    case StmtKind::Let:
                    case StmtKind::Var:
                        for (auto& e : s.exprs) selfE(selfE, selfB, e);
                        if (s.name != "_") scopes.back().insert(s.name);
                        break;
                    case StmtKind::If:
                        for (auto& e : s.exprs) selfE(selfE, selfB, e);
                        selfB(selfB, selfE, s.body);
                        selfB(selfB, selfE, s.otherwise);
                        break;
                    case StmtKind::SafetyBlock:
                        selfB(selfB, selfE, s.body);
                        break;
                    case StmtKind::While:
                        for (auto& e : s.exprs) selfE(selfE, selfB, e);
                        selfB(selfB, selfE, s.body);
                        break;
                    case StmtKind::For:
                    case StmtKind::ForEach:
                        for (auto& e : s.exprs) selfE(selfE, selfB, e);
                        scopes.emplace_back();
                        if (s.name != "_") scopes.back().insert(s.name);
                        selfB(selfB, selfE, s.body);
                        scopes.pop_back();
                        break;
                    case StmtKind::Match:
                        for (auto& e : s.exprs) selfE(selfE, selfB, e);
                        for (auto& arm : s.arms) {
                            scopes.emplace_back();
                            for (const auto& n : arm.names)
                                if (n != "_") scopes.back().insert(n);
                            selfB(selfB, selfE, arm.body);
                            scopes.pop_back();
                        }
                        break;
                    default:
                        for (auto& e : s.exprs) selfE(selfE, selfB, e);
                        break;
                }
            }
            scopes.pop_back();
        };
        rwBlock(rwBlock, rwExpr, body);
    }

    Type inferClosure(Expr& expr, Type expected, bool ownedFirstParam = false, bool requireSendCaptures = false) {
        ClosureExpr& cl = *expr.closure;
        Type expRet = Type::Unknown;
        std::vector<Type> expParams;
        if (expected != Type::Unknown && types.program.isFn(expected)) {
            expRet = types.program.fnResult(expected);
            expParams = types.program.fnParams(expected);
            if (expParams.size() == cl.params.size()) {
                for (std::size_t i = 0; i < cl.params.size(); ++i) {
                    if (cl.params[i].type == Type::Unknown && expParams[i] != Type::Unknown)
                        cl.params[i].type = expParams[i];
                }
            }
        }
        std::vector<Type> paramTypes;
        paramTypes.reserve(cl.params.size());
        for (auto& p : cl.params) {
            if (p.type == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir el tipo del parámetro de closure '" + p.name + "'; anótalo",
                     p.span, "|" + p.name + ": int| " + p.name + " + 1");
            known(p.type, p.span, false);
            paramTypes.push_back(p.type);
        }
        Type explicitRet = cl.result;
        if (explicitRet != Type::Unknown) known(explicitRet, expr.span, true);

        CaptureState caps;
        Type childRetTarget = (explicitRet != Type::Unknown) ? explicitRet
                            : (expRet != Type::Unknown ? expRet : Type::Unknown);
        Type inferredRet = Type::Unknown;

        Function childFn{};
        childFn.name = "@closure.tmp";
        childFn.sourceName = childFn.name;
        childFn.span = expr.span;
        childFn.module = current.module;
        childFn.result = childRetTarget;
        childFn.safety = activeSafety;
        childFn.isImpl = true;
        childFn.isSpecialization = current.isSpecialization;
        childFn.substitutedTypes = current.substitutedTypes;
        childFn.methodConstraints = current.methodConstraints;
        childFn.params = cl.params;
        if (ownedFirstParam && !childFn.params.empty()) {
            childFn.params[0].owned = true;
            childFn.params[0].writable = true;
        }
        childFn.body = std::move(cl.body);

        {
            Checker child{functions, childFn, {}, types, interfaces, generics, sizes, {},
                          fnRegistry, pendingClosures, fnRefCache, nextClosureId, this, &caps,
                          (childRetTarget == Type::Unknown ? &inferredRet : nullptr), inAbstract, {}, {}, {}, false, activeSafety};
            for (std::size_t pi = 0; pi < childFn.params.size(); ++pi) {
                const auto& p = childFn.params[pi];
                const bool ownedP = ownedFirstParam && pi == 0;
                child.declare(p.name, p.type, ownedP, p.span, ownedP);
            }

            if (cl.isExprBody) {
                Expr& bodyExpr = childFn.body.at(0).exprs.at(0);
                Type act = child.typeOf(bodyExpr, ArrayUse::Copy, childRetTarget);
                if (childRetTarget == Type::Void || (childRetTarget == Type::Unknown && act == Type::Void)) {
                    childFn.body[0].kind = StmtKind::Call;
                    childFn.result = Type::Void;
                } else {
                    if (childRetTarget != Type::Unknown) child.coerceIfDyn(bodyExpr, act, childRetTarget, expr.span);
                    requireValue(act, bodyExpr.span);
                    if (childRetTarget != Type::Unknown) {
                        child.requireType(act, childRetTarget, bodyExpr.span, expr.span);
                        childFn.result = childRetTarget;
                    } else {
                        childFn.result = act;
                    }
                }
            } else {
                const bool allReturn = child.block(childFn.body, false);
                if (childFn.result == Type::Unknown) {
                    childFn.result = (inferredRet == Type::Unknown) ? Type::Void : inferredRet;
                }
                if (childFn.result != Type::Void && !allReturn)
                    fail("E_RETURN", "Hay un camino sin valor de retorno en la closure", expr.span,
                         "|x: int| -> int { return x }");
            }
        }

        const Type finalRet = childFn.result;
        const Type fnType = types.program.fnOf(finalRet, paramTypes);
        interfaces.checkDynInType(fnType, current.module, expr.span, false);

        if (requireSendCaptures) {
            for (const auto& cname : caps.order) {
                const Type ct = caps.types.at(cname);
                if (!types.program.isSendable(ct)) {
                    Diagnostic d("E_SEND",
                                 "La captura '" + cname + "' (" + types.program.nameOf(ct) +
                                     ") no puede enviarse entre hilos; usa freeze(" + cname + ") o taskSpawnMove.",
                                 expr.span);
                    d.example = "let fz = freeze(" + cname + ")\nlet t = taskSpawn(mut s, || frozenRead(fz, |x| ...))";
                    throw d;
                }
            }
        }

        if (inAbstract) {
            cl.body = std::move(childFn.body);
            cl.result = finalRet;
            expr.args.clear();
            cl.id = 0;
            return fnType;
        }

        const std::size_t id = (*nextClosureId)++;
        childFn.name = "@closure." + std::to_string(id);
        childFn.sourceName = childFn.name;

        Type envStruct = Type::Void;
        std::vector<Type> capTypes;
        if (!caps.order.empty()) {
            const std::string envName = "@ClosureEnv." + std::to_string(id);
            envStruct = types.program.intern(envName);
            Variant v{envName, expr.span, {}};
            for (const auto& cname : caps.order) {
                const Type ct = caps.types.at(cname);
                capTypes.push_back(ct);
                v.fields.push_back(Param{cname, ct, expr.span, false, false});
            }
            const std::size_t dIdx = types.program.data.size();
            types.program.data.push_back({envStruct, false, expr.span, {std::move(v)}, current.module});
            types.registerConcreteAt(dIdx);
            rewriteClosureCaptures(childFn.body, envStruct, caps.order, capTypes);
            childFn.params.insert(childFn.params.begin(), Param{"__env", envStruct, expr.span, false, false});
        }

        pendingClosures->push_back(std::move(childFn));
        fnRegistry->emplace(pendingClosures->back().name, &pendingClosures->back());
        if (types.program.closures.size() <= id) types.program.closures.resize(id + 1);
        types.program.closures[id] = {id, pendingClosures->back().name, fnType, envStruct, paramTypes, finalRet};

        expr.args.clear();
        for (const auto& cname : caps.order) {
            Expr capArg{ExprKind::Name, expr.span, cname, {}};
            (void)typeOf(capArg, ArrayUse::Copy);
            expr.args.push_back(std::move(capArg));
        }
        cl.id = id;
        cl.result = finalRet;
        cl.body.clear();
        return fnType;
    }

    Type inferClosureCall(Expr& expr) {
        if (!expr.typeArgs.empty())
            fail("E_GENERIC", "Las closures no aceptan argumentos de tipo <...>", expr.span, "f(x)");
        const Type calleeT = typeOf(expr.args[0], ArrayUse::Ordinary);
        if (!types.program.isFn(calleeT)) {
            Diagnostic d("E_CALL_TYPE", "No se puede llamar un valor de tipo " + types.program.nameOf(calleeT), expr.args[0].span);
            d.actual = types.program.nameOf(calleeT);
            d.expected = "fn(...)";
            throw d;
        }
        const auto paramTypes = types.program.fnParams(calleeT);
        const std::size_t got = expr.args.size() - 1;
        if (got != paramTypes.size()) {
            Diagnostic d("E_ARITY", "Cantidad incorrecta de argumentos al llamar closure", expr.span);
            d.expected = std::to_string(paramTypes.size());
            d.actual = std::to_string(got);
            throw d;
        }
        for (std::size_t i = 0; i < paramTypes.size(); ++i) {
            Expr& arg = expr.args[i + 1];
            if (arg.isMutArg) fail("E_MUT_ARG", "Las closures no admiten parámetros mut", arg.span, "f(x)");
            Type act = typeOf(arg, ArrayUse::Ordinary, paramTypes[i]);
            coerceIfDyn(arg, act, paramTypes[i], expr.args[0].span);
            requireType(act, paramTypes[i], arg.span, expr.args[0].span);
        }
        const Type retT = types.program.fnResult(calleeT);
        types.ensureInstantiated(retT, expr.span);
        interfaces.checkDynInType(retT, current.module, expr.span, false);
        return retT;
    }
    void visible(const std::string& name, Span where) const {
        if (!types.program.visible(current.module, name))
            fail("E_IMPORT", "El módulo usado no es un import directo: " + name, where, "import stats");
    }
    void known(Type type, Span where, bool allowVoid = false) {
        if (types.program.isArray(type)) {
            known(types.program.elemOf(type), where, false);
            return;
        }
        if (const auto* app = types.program.typeAppOf(type)) {
            const std::string base = app->baseName;
            const std::vector<Type> args = app->args;
            if (base == "@Dyn") {
                types.ensureInstantiated(type, where);
                const bool substituted = current.isSpecialization &&
                    std::find(current.substitutedTypes.begin(), current.substitutedTypes.end(), type) !=
                        current.substitutedTypes.end();
                interfaces.checkDynInType(type, current.module, where, !substituted);
                return;
            }
            if (base == "@Fn") {
                if (args.empty())
                    fail("E_TYPE", "Tipo de función inválido", where, "fn(int) -> int");
                known(args[0], where, true);
                for (std::size_t i = 1; i < args.size(); ++i) known(args[i], where, false);
                types.ensureInstantiated(type, where);
                interfaces.checkDynInType(type, current.module, where, false);
                return;
            }
            if (!base.starts_with("@")) visible(base, where);
            for (Type a : args) known(a, where, base == "@Task" || base == "@Ptr" || base == "@MutPtr");
            types.ensureInstantiated(type, where);
            interfaces.checkDynInType(type, current.module, where, false);
            return;
        }
        types.known(type, where, allowVoid);
        const bool substituted = current.isSpecialization &&
            std::find(current.substitutedTypes.begin(), current.substitutedTypes.end(), type) !=
                current.substitutedTypes.end();
        if (!substituted && type >= Type::NamedBase && types.program.nameOf(type).find('@') == std::string::npos)
            visible(types.program.nameOf(type), where);
    }

    Variable& declare(const std::string& name, Type type, bool writable, Span span, bool isOwnedLocal = true) {
        const auto [it, added] = locals.levels.back().emplace(name, Variable{type, writable, span});
        if (!added) fail("E_DUP_NAME", "Nombre duplicado en este bloque: " + name,
                         span, "let other = 1", it->second.declared);
        it->second.ownedLocal = isOwnedLocal;
        reserveValue(type, span);
        return it->second;
    }

    struct ResolvedCapture { Type type; Span declared; };
    std::optional<ResolvedCapture> resolveInEnclosing(const std::string& name, std::optional<Span> useSpan = {}) {
        if (Variable* local = locals.find(name)) {
            if (types.program.isResource(local->type))
                failResourceCopy("Las closures no pueden capturar el recurso no copiable '" + name + "' (" +
                                 types.program.nameOf(local->type) + ").",
                                 useSpan.value_or(local->declared), local->declared);
            if (local->moved)
                failUseAfterMove(name, local->type, useSpan.value_or(local->declared), local->movedAt,
                                 local->movedTo, local->declared);
            if (local->regionSite) local->regionSite->allocation = ArrayAllocation::Owner;
            return ResolvedCapture{local->type, local->declared};
        }
        if (!parentChecker) return {};
        auto outer = parentChecker->resolveInEnclosing(name, useSpan);
        if (outer && activeCaptures) {
            if (activeCaptures->types.emplace(name, outer->type).second)
                activeCaptures->order.push_back(name);
        }
        return outer;
    }

    Variable* lookupOptional(const std::string& name, std::optional<Span> useSpan = {}) {
        if (auto* value = locals.find(name)) return value;
        if (!parentChecker) return nullptr;
        auto outer = parentChecker->resolveInEnclosing(name, useSpan);
        if (!outer) return nullptr;
        if (activeCaptures && activeCaptures->types.emplace(name, outer->type).second)
            activeCaptures->order.push_back(name);
        auto [it, inserted] = capturedVars.emplace(
            name, Variable{outer->type, false, outer->declared, nullptr, false, true});
        (void)inserted;
        return &it->second;
    }

    Variable& lookup(const std::string& name, Span span) {
        if (auto* value = lookupOptional(name, span)) return *value;
        if (name == "null" || name == "nil" || name == "undefined") {
            Diagnostic d("E_NULL", "Turi no tiene null; usa None con un tipo opcional T?.", span);
            d.fix = Edit{span, "None"};
            d.example = "let x: int? = None";
            throw d;
        }
        fail("E_NAME", "Nombre desconocido: " + name, span, "let total = 0");
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

    bool isUnitEnum(Type t) const {
        if (t < Type::NamedBase || !types.hasDecl(t)) return false;
        const auto& d = types.decl(t);
        if (!d.isEnum) return false;
        return std::all_of(d.variants.begin(), d.variants.end(),
                           [](const Variant& v) { return v.fields.empty(); });
    }

    static void foldConstExpr(Expr& expr, const ConstDecl& cd) {
        expr.args.clear();
        expr.labels.clear();
        expr.type = cd.type;
        if (Program::isSignedInt(cd.type)) {
            expr.kind = ExprKind::Integer;
            expr.text = std::to_string(cd.intVal);
        } else if (Program::isUnsignedInt(cd.type)) {
            expr.kind = ExprKind::Integer;
            expr.text = std::to_string(cd.uintVal);
        } else if (Program::isFloatType(cd.type)) {
            expr.kind = ExprKind::Float;
            expr.text = exactFloatLiteral(cd.floatVal);
        } else if (cd.type == Type::Bool) {
            expr.kind = ExprKind::Boolean;
            expr.text = cd.boolVal ? "true" : "false";
        } else if (cd.type == Type::Str) {
            expr.kind = ExprKind::String;
            expr.text = cd.strVal;
        }
    }

    static std::optional<std::pair<const Expr*, std::string>> lvaluePath(const Expr& e) {
        std::vector<const Expr*> steps;
        const Expr* cur = &e;
        while (cur->kind == ExprKind::Field || cur->kind == ExprKind::Index) {
            steps.push_back(cur);
            cur = &cur->args.at(0);
        }
        if (cur->kind != ExprKind::Name) return {};
        std::string full = cur->text;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            if ((*it)->kind == ExprKind::Index) break;
            full += ".";
            full += (*it)->text;
        }
        return std::make_pair(cur, std::move(full));
    }

    static bool pathsOverlap(const std::string& a, const std::string& b) {
        return a == b ||
               (a.size() > b.size() && a.starts_with(b) && a[b.size()] == '.') ||
               (b.size() > a.size() && b.starts_with(a) && b[a.size()] == '.');
    }

    std::pair<Variable*, std::string> resolveMutPath(Expr& arg, const std::string& example,
                                                     std::optional<Span> paramSpan = {}, bool skipTypeCheck = false) {
        auto lp = lvaluePath(arg);
        if (!lp)
            fail("E_IMMUTABLE", "El argumento mutable debe ser una variable, campo o elemento de arreglo mutable (no temporal)",
                 arg.span, example, paramSpan);
        Variable& v = lookup(lp->first->text, lp->first->span);
        if (v.isCaptured)
            fail("E_CLOSURE_CAPTURE_MUT", "Las closures capturan '" + lp->first->text +
                 "' por valor inmutable; crea 'var copia = " + lp->first->text + "' dentro si necesitas mutarlo.",
                 arg.span, "var copia = " + lp->first->text, v.declared);
        if (!v.writable) {
            Diagnostic d("E_IMMUTABLE", "No se puede modificar un let o valor de solo lectura como mut: " + lp->first->text, arg.span);
            d.related.push_back(v.declared);
            d.example = example;
            throw d;
        }
        if (v.regionSite) v.regionSite->allocation = ArrayAllocation::Owner;
        if (!skipTypeCheck) (void)typeOf(arg, ArrayUse::Ordinary);
        for (const Expr* p = &arg; p->kind == ExprKind::Field || p->kind == ExprKind::Index; p = &p->args.at(0)) {
            if (p->kind == ExprKind::Index && !types.program.isArray(p->args.at(0).type)) {
                Diagnostic d("E_IMMUTABLE", "Solo se pueden pasar por referencia mut elementos de un arreglo [T]", arg.span);
                d.example = example;
                throw d;
            }
            if (p->kind == ExprKind::Field) {
                const Type bt = p->args.at(0).type;
                if (types.hasDecl(bt) && types.decl(bt).packed) {
                    Diagnostic d("E_PACKED_REF",
                                 "No se puede tomar una referencia mut a un campo de un struct @packed; asigna el campo directamente.",
                                 arg.span);
                    d.example = "p.campo = valor";
                    throw d;
                }
            }
        }
        return {&v, std::move(lp->second)};
    }

    void validateReadPlace(Expr& arg, const std::string& example) {
        auto lp = lvaluePath(arg);
        if (!lp)
            fail("E_IMMUTABLE", "El argumento de ptrOf debe ser una variable, campo o elemento de arreglo con dirección",
                 arg.span, example);
        Variable& v = lookup(lp->first->text, lp->first->span);
        if (v.regionSite) v.regionSite->allocation = ArrayAllocation::Owner;
        (void)typeOf(arg, ArrayUse::Ordinary);
        for (const Expr* p = &arg; p->kind == ExprKind::Field || p->kind == ExprKind::Index; p = &p->args.at(0)) {
            if (p->kind == ExprKind::Index && !types.program.isArray(p->args.at(0).type)) {
                Diagnostic d("E_IMMUTABLE", "Solo se puede tomar dirección de elementos de un arreglo [T]", arg.span);
                d.example = example;
                throw d;
            }
            if (p->kind == ExprKind::Field) {
                const Type bt = p->args.at(0).type;
                if (types.hasDecl(bt) && types.decl(bt).packed) {
                    Diagnostic d("E_PACKED_REF",
                                 "No se puede tomar un puntero a un campo de un struct @packed.",
                                 arg.span);
                    d.example = "let copia = p.campo";
                    throw d;
                }
            }
        }
    }

    // Envuelve una expresión ya tipada en una llamada integrada de un argumento, sin volver a tiparla.
    Type wrapTyped(Expr& e, const char* builtin) {
        Expr inner = std::move(e);
        Expr call{ExprKind::Call, inner.span, builtin, {}};
        call.args.push_back(std::move(inner));
        e = std::move(call);
        Expr* const outer = preTypedArg0;
        preTypedArg0 = &e.args[0];
        const Type t = typeOf(e);
        preTypedArg0 = outer;
        return t;
    }
    // Un valor en celda (el campo que cierra un tipo recursivo) se lee como el valor que contiene.
    Type derefCell(Expr& e, Type t) {
        if (strictSyntax() || !types.program.isShared(t)) return t;
        const Type inner = types.program.sharedElem(t);
        types.ensureInstantiated(inner, e.span);
        if (!types.hasDecl(inner)) return t;
        return wrapTyped(e, "sharedGet");
    }

    void coerceIfDyn(Expr& expr, Type& actual, Type expected, std::optional<Span> related = {}) {
        // Celdas automáticas de los tipos recursivos: T entra en Shared<T> y sale de él sin escribirlo.
        if (!strictSyntax() && expected != Type::Unknown && actual != Type::Unknown && actual != expected) {
            if (types.program.isShared(expected) && types.program.sharedElem(expected) == actual) {
                actual = wrapTyped(expr, "sharedNew");
                return;
            }
            if (types.program.isShared(actual) && types.program.sharedElem(actual) == expected) {
                actual = wrapTyped(expr, "sharedGet");
                return;
            }
            // Lo mismo dentro de un opcional: `T?` y el campo `T?` guardado en celda.
            if (types.program.isOption(expected) && types.program.isOption(actual)) {
                const Type want = types.program.optionInner(expected);
                const Type have = types.program.optionInner(actual);
                const auto helper = [&](const std::string& name) {
                    return std::any_of(types.program.functions.begin(), types.program.functions.end(),
                                       [&](const Function& f) { return f.sourceName == name; });
                };
                if (types.program.isShared(have) && types.program.sharedElem(have) == want) {
                    const std::string name = "tzCellGet" + types.program.nameOf(want);
                    if (helper(name)) {
                        actual = wrapTyped(expr, name.c_str());
                        return;
                    }
                }
                if (types.program.isShared(want) && types.program.sharedElem(want) == have) {
                    const std::string name = "tzCellNew" + types.program.nameOf(have);
                    if (helper(name)) {
                        actual = wrapTyped(expr, name.c_str());
                        return;
                    }
                }
            }
        }
        if (expected == Type::Unknown || !types.program.isDyn(expected) ||
            actual == expected || types.program.isDyn(actual))
            return;
        const Type iface = types.program.dynInterface(expected);
        if (types.program.isResource(actual))
            failResourceCopy("El recurso no copiable '" + types.program.nameOf(actual) +
                             "' no puede almacenarse en " + types.program.nameOf(expected) + ".",
                             expr.span, related);
        interfaces.checkDynSafety(iface, expr.span);
        bool ok = false;
        if (isOpaqueType(types.program, actual)) {
            ok = std::any_of(current.methodConstraints.begin(), current.methodConstraints.end(),
                             [&](const auto& mc) { return mc.first == actual && mc.second == iface; });
        } else {
            ok = interfaces.ensureInterfaceImpl(iface, actual, expr.span);
        }
        if (!ok) {
            Diagnostic d("E_IMPL_MISSING", "Falta impl " + types.program.nameOf(iface) + " for " +
                         types.program.nameOf(actual), expr.span);
            if (related) d.related.push_back(*related);
            d.expected = types.program.nameOf(expected);
            d.actual = types.program.nameOf(actual);
            d.example = "impl " + types.program.nameOf(iface) + " for " + types.program.nameOf(actual) + " { ... }";
            throw d;
        }
        Expr inner = std::move(expr);
        expr = Expr{ExprKind::BoxDyn, inner.span, {}, {}};
        expr.args.push_back(std::move(inner));
        expr.type = expected;
        expr.assocType = iface;
        actual = expected;
    }

    static std::optional<std::size_t> overlapsIndex(const std::vector<std::string>& set, const std::string& p) {
        for (std::size_t i = 0; i < set.size(); ++i) {
            if (pathsOverlap(set[i], p)) return i;
        }
        return {};
    }

    [[noreturn]] static void failAliasMut(const std::string& valuePath, const std::string& ownerPath,
                                          Span valueSpan, Span ownerSpan) {
        Diagnostic d("E_ALIAS_MUT",
                     "Conflicto de acceso exclusivo: el valor '" + valuePath +
                     "' ya está en uso por el dueño '" + ownerPath + "'.",
                     valueSpan);
        if (ownerSpan.start != ownerSpan.end || ownerSpan.file != valueSpan.file)
            d.related.push_back(ownerSpan);
        d.example = "swap(mut a, mut b)";
        throw d;
    }

    void checkCallArguments(Expr& expr, const std::vector<Param>& params, bool isInterfaceDispatch = false,
                            Type selfTarget = Type::Unknown, std::size_t startArg = 0,
                            const std::vector<std::string>& initialMutPaths = {},
                            const std::vector<std::string>& initialAllPaths = {},
                            const std::vector<Span>& initialMutSpans = {},
                            const std::vector<Span>& initialAllSpans = {}) {
        checkCallArity(expr, params.size(), params.empty() ? std::optional<Span>{} : params.front().span);
        std::vector<std::string> mutPaths = initialMutPaths, allPaths = initialAllPaths;
        std::vector<Span> mutSpans = initialMutSpans, allSpans = initialAllSpans;
        mutSpans.resize(mutPaths.size(), expr.span);
        allSpans.resize(allPaths.size(), expr.span);
        std::vector<Type> argTypes(expr.args.size(), Type::Unknown);
        for (std::size_t i = startArg; i < expr.args.size(); ++i) {
            Expr& arg = expr.args[i];
            const Param& param = params[i];
            const Type expected = isInterfaceDispatch ? interfaces.substituteSelf(param.type, selfTarget) : param.type;
            if (param.byRef) {
                if (inContract)
                    fail("E_CONTRACT", "Los contratos requires/ensures no pueden mutar variables ni pasar argumentos mut.",
                         arg.span, "fn f(a: [int], i: int) -> int requires i >= 0 && i < len(a)", param.span);
                if (!arg.isMutArg) {
                    Diagnostic d("E_MUT_REQUIRED", "Falta mut delante del argumento para parámetro mutable: " + param.name, arg.span);
                    d.related.push_back(param.span);
                    d.fix = Edit{{arg.span.start, arg.span.start}, "mut "};
                    d.example = "bump(mut x)";
                    throw d;
                }
                auto [v, path] = resolveMutPath(arg, "bump(mut x)", param.span, false);
                (void)v;
                if (auto idx = overlapsIndex(mutPaths, path))
                    failAliasMut(path, mutPaths[*idx], arg.span, mutSpans[*idx]);
                if (auto idx = overlapsIndex(allPaths, path))
                    failAliasMut(path, allPaths[*idx], arg.span, allSpans[*idx]);
                mutPaths.push_back(path);
                mutSpans.push_back(arg.span);
                allPaths.push_back(std::move(path));
                allSpans.push_back(arg.span);
                argTypes[i] = arg.type;
            } else {
                if (arg.isMutArg)
                    fail("E_MUT_ARG", "El parámetro no admite mut", arg.span, "f(x)", param.span);
                argTypes[i] = typeOf(arg, ArrayUse::Ordinary, expected);
                coerceIfDyn(arg, argTypes[i], expected, param.span);
                if (!isTrivialCopyType(types.program, argTypes[i])) {
                    if (auto lp = lvaluePath(arg)) {
                        if (auto idx = overlapsIndex(mutPaths, lp->second))
                            failAliasMut(lp->second, mutPaths[*idx], arg.span, mutSpans[*idx]);
                        allPaths.push_back(std::move(lp->second));
                        allSpans.push_back(arg.span);
                    }
                }
            }
            requireType(argTypes[i], expected, arg.span, param.span);
        }
    }

    Type inferInterface(Expr& expr) {
        const Function* method = nullptr;
        Type self = Type::Unknown, interface = Type::Unknown;
        for (const auto& [constrained, candidate] : current.methodConstraints) {
            const auto& declaration = *interfaces.declarations.at(candidate);
            for (const auto& candidateMethod : declaration.methods) {
                if (candidateMethod.sourceName != expr.text) continue;
                if (method) fail("E_OVERLOAD", "La llamada de interfaz es ambigua", expr.span, "scoreOf(value)");
                method = &candidateMethod;
                self = constrained;
                interface = candidate;
            }
        }
        if (!method) fail("E_FUNCTION", "Función o método de constraint desconocido: " + expr.text, expr.span,
                          "fn score<T: contracts.Scored>(value: T) -> int { return scoreOf(value) }");
        checkCallArguments(expr, method->params, true, self);
        const auto result = interfaces.substituteSelf(method->result, self);
        if (mentionsOpaqueType(types.program, self)) {
            expr.text = "@abstract." + expr.text;
            return result;
        }
        const auto key = std::make_pair(interface, self);
        if (!interfaces.ensureInterfaceImpl(interface, self, expr.span))
            fail("E_IMPL_MISSING", "Falta impl " + types.program.nameOf(interface) + " for " +
                 types.program.nameOf(self), expr.span, "impl contracts.Scored for Item { fn scoreOf(value: Item) -> int { return value.score } }");
        expr.text = interfaces.dispatch.at(key).at(method->sourceName);
        return result;
    }

    Variable& checkMutTarget(Expr& arg, std::string_view fn, const std::string& example) {
        if (inContract)
            fail("E_CONTRACT", "Los contratos requires/ensures no pueden mutar variables ni pasar argumentos mut.",
                 arg.span, "fn f(x: int) -> int requires x >= 0");
        if (!arg.isMutArg) {
            Diagnostic d("E_MUT_REQUIRED", "Falta mut delante del argumento: " + std::string(fn), arg.span);
            d.fix = Edit{{arg.span.start, arg.span.start}, "mut "};
            d.example = example;
            throw d;
        }
        auto [v, path] = resolveMutPath(arg, example);
        (void)path;
        return *v;
    }

    Type checkMutArrayBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, "var a: [int] = []\npush(mut a, 1)");
        if (!types.program.isArray(arg.type)) {
            Diagnostic d("E_TYPE", expr.text + " requiere un array", arg.span);
            d.expected = "[T]";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
        return arg.type;
    }

    Type checkMutCollectionBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, "var a: [int] = []\nclear(mut a)");
        if (!types.program.isArray(arg.type) && !types.program.isMap(arg.type) &&
            !types.program.isSet(arg.type) && !types.program.isPool(arg.type)) {
            Diagnostic d("E_TYPE", expr.text + " requiere [T], Map<K, V>, Set<T> o Pool<T>", arg.span);
            d.expected = "[T] | Map<K, V> | Set<T> | Pool<T>";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
        return arg.type;
    }

    Type checkMutPoolBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, "var p: Pool<Node> = poolNew()\npoolAdd(mut p, n)");
        if (!types.program.isPool(arg.type)) {
            Diagnostic d("E_TYPE", expr.text + " requiere Pool<T>", arg.span);
            d.expected = "Pool<T>";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
        return arg.type;
    }

    Type checkMutSharedBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, "var s: Shared<int> = sharedNew(1)\nsharedSet(mut s, 2)");
        if (!types.program.isShared(arg.type)) {
            Diagnostic d("E_TYPE", expr.text + " requiere Shared<T>", arg.span);
            d.expected = "Shared<T>";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
        // Queda anotado qué celdas se modifican en su sitio: las demás se pueden leer prestadas.
        const Type elem = types.program.sharedElem(arg.type);
        if (isOpaqueType(types.program, elem)) types.program.anyCellMutated = true;
        else if (!types.program.cellMutated(elem)) types.program.mutatedCells.push_back(elem);
        return arg.type;
    }

    Type checkMutMapBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, R"(var m: Map<str, int> = mapNew()
mapSet(mut m, "a", 1))");
        if (!types.program.isMap(arg.type)) {
            Diagnostic d("E_TYPE", expr.text + " requiere Map<K, V>", arg.span);
            d.expected = "Map<K, V>";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
        return arg.type;
    }

    Type checkMutSetBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, "var s: Set<int> = setNew()\nsetAdd(mut s, 1)");
        if (!types.program.isSet(arg.type)) {
            Diagnostic d("E_TYPE", expr.text + " requiere Set<T>", arg.span);
            d.expected = "Set<T>";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
        return arg.type;
    }

    void checkMutSbBuiltin(Expr& expr) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, "var sb = sbNew()\nsbPush(mut sb, \"a\")");
        if (arg.type != Type::StringBuilder) {
            Diagnostic d("E_TYPE", expr.text + " requiere StringBuilder", arg.span);
            d.expected = "StringBuilder";
            d.actual = types.program.nameOf(arg.type);
            throw d;
        }
    }

    void checkMutResourceBuiltin(Expr& expr, Type expectedRes, const std::string& example) {
        Expr& arg = expr.args[0];
        checkMutTarget(arg, expr.text, example);
        if (arg.type != expectedRes) {
            Diagnostic d("E_TYPE", expr.text + " requiere " + types.program.nameOf(expectedRes), arg.span);
            d.expected = types.program.nameOf(expectedRes);
            d.actual = types.program.nameOf(arg.type);
            d.example = example;
            throw d;
        }
    }

    Type inferBuiltinCall(Expr& expr, Type expected = Type::Unknown) {
        const bool acceptsTypeArgs = (expr.text == "mapNew" || expr.text == "mapSecure" || expr.text == "mapWithCap" ||
                                      expr.text == "setNew" || expr.text == "setSecure" || expr.text == "setWithCap" ||
                                      expr.text == "poolNew" || expr.text == "poolWithCap" ||
                                      expr.text == "sizeOf" || expr.text == "alignOf" || expr.text == "bitcast" ||
                                      expr.text == "ptrNull" || expr.text == "mutPtrNull" ||
                                      expr.text == "ptrCast" || expr.text == "mutPtrCast" ||
                                      expr.text == "ptrFromAddr" || expr.text == "mutPtrFromAddr" ||
                                      expr.text == "ptrAlloc" || expr.text == "ptrAllocAligned" ||
                                      expr.text == "simdSplat" || expr.text == "simdLanes" ||
                                      expr.text == "simdFromArray" || expr.text == "simdLoad" ||
                                      expr.text == "simdLoadAligned" ||
                                      expr.text == "atomicNew" || expr.text == "ringNew" ||
                                      expr.text == "regionAlloc" || expr.text == "regionAllocAligned");
        if (!expr.typeArgs.empty() && !acceptsTypeArgs)
            fail("E_GENERIC", "La función integrada " + expr.text + " no acepta argumentos de tipo <...>",
                 expr.span, expr.text + "(...)");
        auto rejectMutArgs = [&]() {
            for (const auto& arg : expr.args)
                if (arg.isMutArg) fail("E_MUT_ARG", "Esta función no admite mut", arg.span, "len(a)");
        };
        auto requireSimdTypeArg = [&](std::string_view fnLabel) -> Type {
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", std::string(fnLabel) + " requiere exactamente 1 argumento de tipo <Simd<T, N>>", expr.span);
                d.example = std::string(fnLabel) + "<Simd<f32, 4>>(...)";
                throw d;
            }
            const Type vt = expr.typeArgs[0];
            known(vt, expr.span, false);
            validateSimdType(types.program, vt, expr.span);
            return vt;
        };
        auto requireSimdValue = [&](Expr& arg, Type hint, std::string_view fnLabel) -> Type {
            const Type vt = typeOf(arg, ArrayUse::Ordinary, hint);
            if (!types.program.isSimd(vt)) {
                Diagnostic d("E_TYPE", std::string(fnLabel) + " requiere Simd<T, N>", arg.span);
                d.expected = "Simd<T, N>";
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            if (!mentionsOpaqueType(types.program, vt)) validateSimdType(types.program, vt, arg.span);
            return vt;
        };
        if (expr.text == "simdSplat") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type vt = requireSimdTypeArg("simdSplat");
            const Type elemT = types.program.simdElem(vt);
            requireType(typeOf(expr.args[0], ArrayUse::Ordinary, elemT), elemT, expr.args[0].span);
            return vt;
        }
        if (expr.text == "simdLanes") {
            checkCallArity(expr, 0);
            (void)requireSimdTypeArg("simdLanes");
            return Type::Int;
        }
        if (expr.text == "simdFromArray") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type vt = requireSimdTypeArg("simdFromArray");
            const Type elemT = types.program.simdElem(vt);
            const Type arrT = types.program.arrayOf(elemT);
            types.ensureInstantiated(arrT, expr.span);
            requireType(typeOf(expr.args[0], ArrayUse::Inspect, arrT), arrT, expr.args[0].span);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            return vt;
        }
        if (expr.text == "simdToArray") {
            checkCallArity(expr, 3);
            if (expr.args[0].isMutArg) fail("E_MUT_ARG", "simdToArray no admite mut en el vector", expr.args[0].span, "simdToArray(v, mut xs, 0)");
            if (expr.args[2].isMutArg) fail("E_MUT_ARG", "simdToArray no admite mut en el índice", expr.args[2].span, "simdToArray(v, mut xs, 0)");
            if (!expr.args[1].isMutArg) {
                Diagnostic d("E_MUT_REQUIRED", "Falta mut delante del arreglo en simdToArray", expr.args[1].span);
                d.fix = Edit{{expr.args[1].span.start, expr.args[1].span.start}, "mut "};
                d.example = "simdToArray(v, mut xs, 0)";
                throw d;
            }
            const Type vt = requireSimdValue(expr.args[0], Type::Unknown, "simdToArray");
            const Type elemT = types.program.simdElem(vt);
            const Type expectedArr = types.program.arrayOf(elemT);
            resolveMutPath(expr.args[1], "simdToArray(v, mut xs, 0)");
            requireType(expr.args[1].type, expectedArr, expr.args[1].span, expr.args[0].span);
            requireType(typeOf(expr.args[2], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[2].span);
            return Type::Void;
        }
        if (expr.text == "simdGet") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], Type::Unknown, "simdGet");
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            return types.program.simdElem(vt);
        }
        if (expr.text == "simdSet") {
            checkCallArity(expr, 3); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], expected, "simdSet");
            const Type elemT = types.program.simdElem(vt);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            requireType(typeOf(expr.args[2], ArrayUse::Ordinary, elemT), elemT, expr.args[2].span);
            return vt;
        }
        if (expr.text == "simdLoad" || expr.text == "simdLoadAligned") {
            requireUnsafe(expr.span, expr.text);
            checkCallArity(expr, 1); rejectMutArgs();
            const Type vt = requireSimdTypeArg(expr.text);
            const Type elemT = types.program.simdElem(vt);
            const Type pt = typeOf(expr.args[0]);
            if (pt != types.program.ptrOf(elemT) && pt != types.program.mutPtrOf(elemT)) {
                Diagnostic d("E_TYPE", expr.text + " requiere Ptr<" + types.program.nameOf(elemT) + "> o MutPtr<" +
                             types.program.nameOf(elemT) + ">", expr.args[0].span);
                d.expected = "Ptr<" + types.program.nameOf(elemT) + "> | MutPtr<" + types.program.nameOf(elemT) + ">";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            return vt;
        }
        if (expr.text == "simdStore" || expr.text == "simdStoreAligned") {
            requireUnsafe(expr.span, expr.text);
            checkCallArity(expr, 2); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(pt)) {
                Diagnostic d("E_TYPE", expr.text + " requiere MutPtr<T> como destino", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            const Type vt = requireSimdValue(expr.args[1], Type::Unknown, expr.text);
            const Type pElem = types.program.ptrElem(pt);
            const Type vElem = types.program.simdElem(vt);
            if (pElem != vElem) {
                Diagnostic d("E_TYPE", expr.text + " requiere vector con elemento " + types.program.nameOf(pElem), expr.args[1].span);
                d.expected = "Simd<" + types.program.nameOf(pElem) + ", N>";
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            return Type::Void;
        }
        if (expr.text == "simdFma") {
            checkCallArity(expr, 3); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], expected, "simdFma");
            if (!Program::isFloatType(types.program.simdElem(vt))) {
                Diagnostic d("E_TYPE", "simdFma requiere carriles flotantes (f32 o float)", expr.args[0].span);
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, vt), vt, expr.args[1].span, expr.args[0].span);
            requireType(typeOf(expr.args[2], ArrayUse::Ordinary, vt), vt, expr.args[2].span, expr.args[0].span);
            return vt;
        }
        if (expr.text == "simdMin" || expr.text == "simdMax") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], expected, expr.text);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, vt), vt, expr.args[1].span, expr.args[0].span);
            return vt;
        }
        if (expr.text == "simdAbs") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], expected, "simdAbs");
            const Type elemT = types.program.simdElem(vt);
            if (!Program::isSignedInt(elemT) && !Program::isFloatType(elemT)) {
                Diagnostic d("E_TYPE", "simdAbs requiere carriles con signo o flotantes", expr.args[0].span);
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            return vt;
        }
        if (expr.text == "simdSqrt") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], expected, "simdSqrt");
            if (!Program::isFloatType(types.program.simdElem(vt))) {
                Diagnostic d("E_TYPE", "simdSqrt requiere carriles flotantes (f32 o float)", expr.args[0].span);
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            return vt;
        }
        if (expr.text == "simdSum" || expr.text == "simdReduceMin" || expr.text == "simdReduceMax") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], Type::Unknown, expr.text);
            return types.program.simdElem(vt);
        }
        if (expr.text == "simdCmpEq" || expr.text == "simdCmpLt" || expr.text == "simdCmpLe" ||
            expr.text == "simdCmpGt" || expr.text == "simdCmpGe") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type vt = requireSimdValue(expr.args[0], Type::Unknown, expr.text);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, vt), vt, expr.args[1].span, expr.args[0].span);
            const Type maskT = simdMaskType(types.program, types.program.simdElem(vt), types.program.simdLanes(vt));
            types.ensureInstantiated(maskT, expr.span);
            return maskT;
        }
        if (expr.text == "simdSelect") {
            checkCallArity(expr, 3); rejectMutArgs();
            const Type mt = requireSimdValue(expr.args[0], Type::Unknown, "simdSelect");
            if (!Program::isIntType(types.program.simdElem(mt))) {
                Diagnostic d("E_TYPE", "La máscara de simdSelect requiere carriles enteros", expr.args[0].span);
                d.actual = types.program.nameOf(mt);
                throw d;
            }
            const Type at = requireSimdValue(expr.args[1], expected, "simdSelect");
            if (types.program.simdLanes(mt) != types.program.simdLanes(at)) {
                Diagnostic d("E_TYPE", "simdSelect requiere máscara y vectores con el mismo número de carriles", expr.args[1].span);
                d.expected = std::to_string(types.program.simdLanes(mt)) + " carriles";
                d.actual = types.program.nameOf(at);
                throw d;
            }
            requireType(typeOf(expr.args[2], ArrayUse::Ordinary, at), at, expr.args[2].span, expr.args[1].span);
            return at;
        }
        if (expr.text == "clz" || expr.text == "ctz" || expr.text == "popcnt" || expr.text == "bswap") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type expT = Program::isIntType(expected) ? expected : Type::Unknown;
            const Type vt = typeOf(expr.args[0], ArrayUse::Ordinary, expT);
            if (!Program::isIntType(vt)) {
                Diagnostic d("E_TYPE", expr.text + " requiere un argumento entero", expr.args[0].span);
                d.expected = "int | i8..u64";
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            return vt;
        }
        if (expr.text == "rotl" || expr.text == "rotr") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type expT = Program::isIntType(expected) ? expected : Type::Unknown;
            const Type vt = typeOf(expr.args[0], ArrayUse::Ordinary, expT);
            if (!Program::isIntType(vt)) {
                Diagnostic d("E_TYPE", expr.text + " requiere un argumento entero", expr.args[0].span);
                d.expected = "int | i8..u64";
                d.actual = types.program.nameOf(vt);
                throw d;
            }
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, vt), vt, expr.args[1].span, expr.args[0].span);
            return vt;
        }
        if (expr.text == "prefetchRead") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isAnyPtr(pt)) {
                Diagnostic d("E_TYPE", "prefetchRead requiere Ptr<T> o MutPtr<T>", expr.args[0].span);
                d.expected = "Ptr<T> | MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            return Type::Void;
        }
        if (expr.text == "prefetchWrite") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(pt)) {
                Diagnostic d("E_TYPE", "prefetchWrite requiere MutPtr<T>", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            return Type::Void;
        }
        if (expr.text == "spinHint") {
            checkCallArity(expr, 0);
            return Type::Void;
        }
        auto requireRawElem = [&](Type elemT, Span where, std::string_view fnLabel) {
            if (elemT == Type::Void) {
                Diagnostic d("E_PTR_TYPE", std::string(fnLabel) + " no admite void como tipo elemento (convierte primero con ptrCast).", where);
                d.example = "let p = ptrCast<u8>(rawVoidPtr)";
                throw d;
            }
            if (!mentionsOpaqueType(types.program, elemT) && !isRawMemSafeType(elemT)) {
                Diagnostic d("E_PTR_TYPE", std::string(fnLabel) + " no admite tipos administrados o recursos: " + types.program.nameOf(elemT), where);
                d.actual = types.program.nameOf(elemT);
                d.example = "let p = ptrAlloc<u8>(64)";
                throw d;
            }
        };
        if (expr.text == "ptrNull" || expr.text == "mutPtrNull") {
            checkCallArity(expr, 0);
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", expr.text + " requiere exactamente 1 argumento de tipo <T>", expr.span);
                d.example = expr.text + "<u8>()";
                throw d;
            }
            const Type targetT = expr.typeArgs[0];
            known(targetT, expr.span, true);
            const Type ptrT = (expr.text == "ptrNull")
                ? types.program.ptrOf(targetT) : types.program.mutPtrOf(targetT);
            if (!mentionsOpaqueType(types.program, ptrT)) types.ensureInstantiated(ptrT, expr.span);
            return ptrT;
        }
        if (expr.text == "ptrIsNull" || expr.text == "ptrAddr") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isAnyPtr(pt)) {
                Diagnostic d("E_TYPE", expr.text + " requiere Ptr<T> o MutPtr<T>", expr.args[0].span);
                d.expected = "Ptr<T> | MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                d.example = expr.text + "(p)";
                throw d;
            }
            return expr.text == "ptrIsNull" ? Type::Bool : Type::U64;
        }
        if (expr.text == "ptrConst") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(pt)) {
                Diagnostic d("E_TYPE", "ptrConst requiere MutPtr<T>", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                d.example = "let cp: Ptr<u8> = ptrConst(mp)";
                throw d;
            }
            const Type resT = types.program.ptrOf(types.program.ptrElem(pt));
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "ptrMut") {
            requireUnsafe(expr.span, "ptrMut");
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isPtr(pt)) {
                Diagnostic d("E_TYPE", "ptrMut requiere Ptr<T>", expr.args[0].span);
                d.expected = "Ptr<T>";
                d.actual = types.program.nameOf(pt);
                d.example = "let mp: MutPtr<u8> = ptrMut(cp)";
                throw d;
            }
            const Type resT = types.program.mutPtrOf(types.program.ptrElem(pt));
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "ptrOf") {
            checkCallArity(expr, 1); rejectMutArgs();
            validateReadPlace(expr.args[0], "ptrOf(x)");
            const Type resT = types.program.ptrOf(expr.args[0].type);
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "mutPtrOf") {
            checkCallArity(expr, 1);
            if (!expr.args[0].isMutArg) {
                Diagnostic d("E_MUT_REQUIRED", "Falta mut delante del argumento de mutPtrOf", expr.args[0].span);
                d.fix = Edit{{expr.args[0].span.start, expr.args[0].span.start}, "mut "};
                d.example = "mutPtrOf(mut x)";
                throw d;
            }
            resolveMutPath(expr.args[0], "mutPtrOf(mut x)");
            const Type resT = types.program.mutPtrOf(expr.args[0].type);
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "slicePtr") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type arrT = typeOf(expr.args[0], ArrayUse::Ordinary);
            if (!types.program.isArray(arrT)) {
                Diagnostic d("E_TYPE", "slicePtr requiere un arreglo [T]", expr.args[0].span);
                d.expected = "[T]";
                d.actual = types.program.nameOf(arrT);
                d.example = "let p = slicePtr(xs)";
                throw d;
            }
            const Type resT = types.program.ptrOf(types.program.elemOf(arrT));
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "mutSlicePtr") {
            checkCallArity(expr, 1);
            if (!expr.args[0].isMutArg) {
                Diagnostic d("E_MUT_REQUIRED", "Falta mut delante del arreglo en mutSlicePtr", expr.args[0].span);
                d.fix = Edit{{expr.args[0].span.start, expr.args[0].span.start}, "mut "};
                d.example = "mutSlicePtr(mut xs)";
                throw d;
            }
            resolveMutPath(expr.args[0], "mutSlicePtr(mut xs)");
            const Type arrT = expr.args[0].type;
            if (!types.program.isArray(arrT)) {
                Diagnostic d("E_TYPE", "mutSlicePtr requiere un arreglo [T]", expr.args[0].span);
                d.expected = "[T]";
                d.actual = types.program.nameOf(arrT);
                d.example = "let p = mutSlicePtr(mut xs)";
                throw d;
            }
            const Type resT = types.program.mutPtrOf(types.program.elemOf(arrT));
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "cstr") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            const Type resT = types.program.arrayOf(Type::U8);
            types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "fnPtr") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Expr& target = expr.args[0];
            std::string ref;
            std::string disp;
            if (target.kind == ExprKind::Name) {
                disp = target.text;
                ref = (!current.module.empty() && target.text.find('.') == std::string::npos)
                    ? current.module + "." + target.text : target.text;
            } else if (target.kind == ExprKind::Field && target.args.size() == 1 &&
                       target.args[0].kind == ExprKind::Name) {
                disp = target.args[0].text + "." + target.text;
                ref = disp;
            } else {
                Diagnostic d("E_FFI_ABI", "fnPtr requiere el nombre de una función extern \"C\".", target.span);
                d.example = "fnPtr(miCallback)";
                throw d;
            }
            const auto fit = functions.find(ref);
            if (fit == functions.end()) {
                fail("E_FUNCTION", "Función desconocida para fnPtr: " + disp, target.span, "fnPtr(miCallback)");
            }
            visible(ref, target.span);
            const Function& callee = *fit->second;
            if (!callee.isExternC || !callee.generics.empty()) {
                Diagnostic d("E_FFI_ABI", "fnPtr solo admite funciones declaradas con extern \"C\".", target.span);
                d.related.push_back(callee.span);
                d.example = "extern \"C\" fn cb(x: i32) -> i32 { return x }\nlet p = fnPtr(cb)";
                throw d;
            }
            expr.labels = {callee.cSymbol};
            expr.args.clear();
            const Type pVoid = types.program.ptrOf(Type::Void);
            types.ensureInstantiated(pVoid, expr.span);
            return pVoid;
        }
        if (expr.text == "ptrCast" || expr.text == "mutPtrCast") {
            requireUnsafe(expr.span, expr.text);
            checkCallArity(expr, 1); rejectMutArgs();
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", expr.text + " requiere exactamente 1 argumento de tipo <U>", expr.span);
                d.example = expr.text + "<u32>(p)";
                throw d;
            }
            const Type targetT = expr.typeArgs[0];
            known(targetT, expr.span, true);
            const Type pt = typeOf(expr.args[0]);
            if (expr.text == "ptrCast") {
                if (!types.program.isAnyPtr(pt)) {
                    Diagnostic d("E_TYPE", "ptrCast requiere Ptr<T> o MutPtr<T>", expr.args[0].span);
                    d.expected = "Ptr<T> | MutPtr<T>";
                    d.actual = types.program.nameOf(pt);
                    throw d;
                }
                const Type resT = types.program.ptrOf(targetT);
                if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
                return resT;
            }
            if (!types.program.isMutPtr(pt)) {
                Diagnostic d("E_TYPE", "mutPtrCast requiere MutPtr<T>", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            const Type resT = types.program.mutPtrOf(targetT);
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "ptrFromAddr" || expr.text == "mutPtrFromAddr") {
            requireUnsafe(expr.span, expr.text);
            checkCallArity(expr, 1); rejectMutArgs();
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", expr.text + " requiere exactamente 1 argumento de tipo <T>", expr.span);
                d.example = expr.text + "<u8>(addr)";
                throw d;
            }
            const Type targetT = expr.typeArgs[0];
            known(targetT, expr.span, true);
            const Type at = typeOf(expr.args[0], ArrayUse::Ordinary, Type::U64);
            if (at != Type::U64 && at != Type::Int) {
                Diagnostic d("E_TYPE", expr.text + " requiere u64 o int como dirección", expr.args[0].span);
                d.expected = "u64 | int";
                d.actual = types.program.nameOf(at);
                throw d;
            }
            const Type resT = (expr.text == "ptrFromAddr")
                ? types.program.ptrOf(targetT) : types.program.mutPtrOf(targetT);
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "ptrAdd") {
            requireUnsafe(expr.span, "ptrAdd");
            checkCallArity(expr, 2); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isAnyPtr(pt)) {
                Diagnostic d("E_TYPE", "ptrAdd requiere Ptr<T> o MutPtr<T>", expr.args[0].span);
                d.expected = "Ptr<T> | MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            const Type elemT = types.program.ptrElem(pt);
            if (elemT == Type::Void) {
                Diagnostic d("E_PTR_TYPE", "ptrAdd no admite void como tipo elemento (convierte primero con ptrCast).", expr.args[0].span);
                d.example = "ptrAdd(ptrCast<u8>(p), 4)";
                throw d;
            }
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            return pt;
        }
        if (expr.text == "ptrRead") {
            requireUnsafe(expr.span, "ptrRead");
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isAnyPtr(pt)) {
                Diagnostic d("E_TYPE", "ptrRead requiere Ptr<T> o MutPtr<T>", expr.args[0].span);
                d.expected = "Ptr<T> | MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            const Type elemT = types.program.ptrElem(pt);
            requireRawElem(elemT, expr.args[0].span, "ptrRead");
            if (!mentionsOpaqueType(types.program, elemT)) {
                Layouts layouts{types, {}, {}};
                layouts.measure(elemT, expr.span);
            }
            return elemT;
        }
        if (expr.text == "ptrWrite") {
            requireUnsafe(expr.span, "ptrWrite");
            checkCallArity(expr, 2); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(pt)) {
                Diagnostic d("E_TYPE", "ptrWrite requiere MutPtr<T>", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            const Type elemT = types.program.ptrElem(pt);
            requireRawElem(elemT, expr.args[0].span, "ptrWrite");
            if (!mentionsOpaqueType(types.program, elemT)) {
                Layouts layouts{types, {}, {}};
                layouts.measure(elemT, expr.span);
            }
            Type vt = typeOf(expr.args[1], ArrayUse::Ordinary, elemT);
            requireType(vt, elemT, expr.args[1].span, expr.args[0].span);
            return Type::Void;
        }
        if (expr.text == "ptrAlloc" || expr.text == "ptrAllocAligned") {
            requireUnsafe(expr.span, expr.text);
            checkCallArity(expr, expr.text == "ptrAlloc" ? 1 : 2);
            rejectMutArgs();
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", expr.text + " requiere exactamente 1 argumento de tipo <T>", expr.span);
                d.example = expr.text + "<u8>(64)";
                throw d;
            }
            const Type elemT = expr.typeArgs[0];
            known(elemT, expr.span, true);
            requireRawElem(elemT, expr.span, expr.text);
            if (!mentionsOpaqueType(types.program, elemT)) {
                types.ensureInstantiated(elemT, expr.span);
                Layouts layouts{types, {}, {}};
                layouts.measure(elemT, expr.span);
            }
            requireType(typeOf(expr.args[0], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[0].span);
            if (expr.text == "ptrAllocAligned")
                requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            const Type resT = types.program.mutPtrOf(elemT);
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "ptrFree") {
            requireUnsafe(expr.span, "ptrFree");
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(pt)) {
                Diagnostic d("E_TYPE", "ptrFree requiere MutPtr<T>", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            return Type::Void;
        }
        if (expr.text == "ptrCopy") {
            requireUnsafe(expr.span, "ptrCopy");
            checkCallArity(expr, 3); rejectMutArgs();
            const Type dstT = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(dstT)) {
                Diagnostic d("E_TYPE", "ptrCopy requiere MutPtr<T> como destino", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(dstT);
                throw d;
            }
            const Type elemT = types.program.ptrElem(dstT);
            requireRawElem(elemT, expr.args[0].span, "ptrCopy");
            const Type srcT = typeOf(expr.args[1]);
            if (!types.program.isAnyPtr(srcT) || types.program.ptrElem(srcT) != elemT) {
                Diagnostic d("E_TYPE", "ptrCopy requiere Ptr<" + types.program.nameOf(elemT) + "> o MutPtr<" +
                             types.program.nameOf(elemT) + "> como origen", expr.args[1].span);
                d.expected = "Ptr<" + types.program.nameOf(elemT) + "> | MutPtr<" + types.program.nameOf(elemT) + ">";
                d.actual = types.program.nameOf(srcT);
                throw d;
            }
            requireType(typeOf(expr.args[2], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[2].span);
            return Type::Void;
        }
        if (expr.text == "ptrZero") {
            requireUnsafe(expr.span, "ptrZero");
            checkCallArity(expr, 2); rejectMutArgs();
            const Type dstT = typeOf(expr.args[0]);
            if (!types.program.isMutPtr(dstT)) {
                Diagnostic d("E_TYPE", "ptrZero requiere MutPtr<T>", expr.args[0].span);
                d.expected = "MutPtr<T>";
                d.actual = types.program.nameOf(dstT);
                throw d;
            }
            const Type elemT = types.program.ptrElem(dstT);
            requireRawElem(elemT, expr.args[0].span, "ptrZero");
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "strFromCStr") {
            requireUnsafe(expr.span, "strFromCStr");
            checkCallArity(expr, 1); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (pt != types.program.ptrOf(Type::U8) && pt != types.program.mutPtrOf(Type::U8)) {
                Diagnostic d("E_TYPE", "strFromCStr requiere Ptr<u8> o MutPtr<u8>", expr.args[0].span);
                d.expected = "Ptr<u8> | MutPtr<u8>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            return Type::Str;
        }
        if (expr.text == "strFromPtr") {
            requireUnsafe(expr.span, "strFromPtr");
            checkCallArity(expr, 2); rejectMutArgs();
            const Type pt = typeOf(expr.args[0]);
            if (pt != types.program.ptrOf(Type::U8) && pt != types.program.mutPtrOf(Type::U8)) {
                Diagnostic d("E_TYPE", "strFromPtr requiere Ptr<u8> o MutPtr<u8>", expr.args[0].span);
                d.expected = "Ptr<u8> | MutPtr<u8>";
                d.actual = types.program.nameOf(pt);
                throw d;
            }
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            return Type::Str;
        }
        if (expr.text == "sizeOf" || expr.text == "alignOf") {
            checkCallArity(expr, 0);
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", expr.text + " requiere exactamente 1 argumento de tipo <T>", expr.span);
                d.example = expr.text + "<u32>()";
                throw d;
            }
            const Type targetT = expr.typeArgs[0];
            known(targetT, expr.span, false);
            if (!mentionsOpaqueType(types.program, targetT)) {
                types.ensureInstantiated(targetT, expr.span);
                Layouts layouts{types, {}, {}};
                layouts.measure(targetT, expr.span);
            }
            return Type::Int;
        }
        if (expr.text == "bitcast") {
            checkCallArity(expr, 1);
            rejectMutArgs();
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", "bitcast requiere exactamente 1 argumento de tipo <To>", expr.span);
                d.example = "bitcast<u32>(x)";
                throw d;
            }
            const Type toT = expr.typeArgs[0];
            known(toT, expr.span, false);
            const Type fromT = typeOf(expr.args[0]);
            requireValue(fromT, expr.args[0].span);
            if (!mentionsOpaqueType(types.program, toT) && !mentionsOpaqueType(types.program, fromT)) {
                types.ensureInstantiated(toT, expr.span);
                types.ensureInstantiated(fromT, expr.args[0].span);
                if (!isBitcastable(types.program, fromT)) {
                    Diagnostic d("E_BITCAST_TYPE", "Tipo origen no compatible con bitcast: " + types.program.nameOf(fromT) +
                                 " (requiere numérico o struct de campos numéricos)", expr.args[0].span);
                    d.actual = types.program.nameOf(fromT);
                    d.example = "let u = bitcast<u32>(f32(1.0))";
                    throw d;
                }
                if (!isBitcastable(types.program, toT)) {
                    Diagnostic d("E_BITCAST_TYPE", "Tipo destino no compatible con bitcast: " + types.program.nameOf(toT) +
                                 " (requiere numérico o struct de campos numéricos)", expr.span);
                    d.actual = types.program.nameOf(toT);
                    d.example = "let u = bitcast<u32>(f32(1.0))";
                    throw d;
                }
                Layouts layouts{types, {}, {}};
                layouts.measure(fromT, expr.args[0].span);
                layouts.measure(toT, expr.span);
                LayoutCalculator lc(types.program);
                const auto szFrom = lc.sizeAlignOf(fromT).first;
                const auto szTo = lc.sizeAlignOf(toT).first;
                if (szFrom != szTo) {
                    Diagnostic d("E_BITCAST_SIZE", "bitcast requiere tipos del mismo tamaño (" +
                                 types.program.nameOf(fromT) + " = " + std::to_string(szFrom) + " bytes, " +
                                 types.program.nameOf(toT) + " = " + std::to_string(szTo) + " bytes)", expr.span);
                    d.expected = std::to_string(szFrom) + " bytes";
                    d.actual = std::to_string(szTo) + " bytes";
                    d.example = "let u = bitcast<u32>(f32(1.0))";
                    throw d;
                }
            }
            return toT;
        }
        if (auto targetNum = numericBuiltinType(expr.text)) {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type expArg = numericLiteralHint(*targetNum, expr.args[0]);
            const auto t = typeOf(expr.args[0], ArrayUse::Ordinary, expArg);
            if (t == Type::Str && !strictSyntax() && (expr.text == "int" || expr.text == "float")) {
                // int("12") y float("1.5") convierten el texto, como en Python y JavaScript.
                expr.text = expr.text == "int" ? "parseInt" : "parseFloat";
                return inferBuiltinCall(expr, expected);
            }
            if (Program::isFloatType(*targetNum)) {
                if (Program::isNumericType(t)) return *targetNum;
                Diagnostic d("E_TYPE", expr.text + "(...) admite tipos numéricos", expr.args[0].span);
                d.expected = "int | i8..u64 | float | f32";
                d.actual = types.program.nameOf(t);
                d.example = expr.text + "(42)";
                throw d;
            }
            if (Program::isNumericType(t) || isUnitEnum(t)) return *targetNum;
            Diagnostic d("E_TYPE", expr.text + "(...) admite tipos numéricos o un enum sin campos", expr.args[0].span);
            d.expected = "int | i8..u64 | float | f32 | enum";
            d.actual = types.program.nameOf(t);
            d.example = expr.text + "(3.9)";
            throw d;
        }
        if (expr.text == "writeByte") { checkCallArity(expr, 1); rejectMutArgs(); requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span); return Type::Void; }
        if (expr.text == "write" || expr.text == "ewrite" || expr.text == "eprint") {
            checkCallArity(expr, 1); rejectMutArgs(); requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span); return Type::Void;
        }
        if (expr.text == "flush") { checkCallArity(expr, 0); return Type::Void; }
        if (expr.text == "readFile" || expr.text == "envGet" || expr.text == "execCapture") {
            checkCallArity(expr, 1); rejectMutArgs(); requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span); return Type::Str;
        }
        if (expr.text == "writeFile" || expr.text == "appendFile") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return Type::Bool;
        }
        if (expr.text == "fileExists" || expr.text == "removeFile" || expr.text == "makeDir") {
            checkCallArity(expr, 1); rejectMutArgs(); requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span); return Type::Bool;
        }
        if (expr.text == "argsCount" || expr.text == "clockNs") { checkCallArity(expr, 0); return Type::Int; }
        if (expr.text == "argAt" || expr.text == "execStatus") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), expr.text == "argAt" ? Type::Int : Type::Str, expr.args[0].span);
            return expr.text == "argAt" ? Type::Str : Type::Int;
        }
        if (expr.text == "sleepMs") { checkCallArity(expr, 1); rejectMutArgs(); requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span); return Type::Void; }
        if (expr.text == "wrappingAdd" || expr.text == "wrappingSub" || expr.text == "wrappingMul" ||
            expr.text == "saturatingAdd" || expr.text == "saturatingSub" || expr.text == "saturatingMul") {
            checkCallArity(expr, 2); rejectMutArgs();
            Type exp0 = Program::isIntType(expected) ? expected : Type::Unknown;
            if (exp0 == Type::Unknown && isLiteralHintCandidate(expr.args[0])) {
                const Type pt = probeExprType(expr.args[1], {});
                if (Program::isIntType(pt)) exp0 = pt;
            }
            const Type lt = typeOf(expr.args[0], ArrayUse::Ordinary, exp0);
            if (!Program::isIntType(lt))
                fail("E_TYPE", expr.text + " requiere tipos enteros", expr.args[0].span, expr.text + "(1, 2)");
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, lt), lt, expr.args[1].span, expr.args[0].span);
            return lt;
        }
        if (expr.text == "abs") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type exp0 = (Program::isSignedInt(expected) || Program::isFloatType(expected)) ? expected : Type::Unknown;
            const auto t = typeOf(expr.args[0], ArrayUse::Ordinary, exp0);
            if (Program::isSignedInt(t) || Program::isFloatType(t)) return t;
            fail("E_TYPE", "abs admite enteros con signo o flotantes", expr.args[0].span, "abs(-3)");
        }
        if (expr.text == "min" || expr.text == "max") {
            checkCallArity(expr, 2); rejectMutArgs();
            Type exp0 = Program::isNumericType(expected) ? expected : Type::Unknown;
            if (exp0 == Type::Unknown && isLiteralHintCandidate(expr.args[0])) {
                const Type pt = probeExprType(expr.args[1], {});
                if (Program::isNumericType(pt)) exp0 = pt;
            }
            const auto lt = typeOf(expr.args[0], ArrayUse::Ordinary, exp0);
            if (!Program::isNumericType(lt))
                fail("E_TYPE", expr.text + " admite tipos numéricos", expr.args[0].span, expr.text + "(1, 2)");
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, lt), lt, expr.args[1].span, expr.args[0].span);
            return lt;
        }
        if (expr.text == "sqrt" || expr.text == "floor" || expr.text == "ceil" || expr.text == "round" ||
            expr.text == "sin" || expr.text == "cos") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type exp0 = Program::isFloatType(expected) ? expected : Type::Unknown;
            const Type t = typeOf(expr.args[0], ArrayUse::Ordinary, exp0);
            if (!Program::isFloatType(t))
                fail("E_TYPE", expr.text + " admite float o f32", expr.args[0].span, expr.text + "(1.5)");
            return t;
        }
        if (expr.text == "pow") {
            checkCallArity(expr, 2); rejectMutArgs();
            Type exp0 = Program::isFloatType(expected) ? expected : Type::Unknown;
            if (exp0 == Type::Unknown && isLiteralHintCandidate(expr.args[0])) {
                const Type pt = probeExprType(expr.args[1], {});
                if (Program::isFloatType(pt)) exp0 = pt;
            }
            const Type t0 = typeOf(expr.args[0], ArrayUse::Ordinary, exp0);
            if (!Program::isFloatType(t0))
                fail("E_TYPE", "pow admite float o f32", expr.args[0].span, "pow(2.0, 3.0)");
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, t0), t0, expr.args[1].span, expr.args[0].span);
            return t0;
        }
        if (expr.text == "isNaN" || expr.text == "isInf") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type t = typeOf(expr.args[0]);
            if (!Program::isFloatType(t))
                fail("E_TYPE", expr.text + " admite float o f32", expr.args[0].span, expr.text + "(1.5)");
            return Type::Bool;
        }
        if (expr.text == "str") {
            checkCallArity(expr, 1); rejectMutArgs();
            const auto t = typeOf(expr.args[0]);
            if (Program::isNumericType(t) || t == Type::Bool || t == Type::Str) return Type::Str;
            Diagnostic d("E_TYPE", "str(...) admite tipos numéricos, bool o str", expr.args[0].span);
            d.expected = "int | i8..u64 | float | f32 | bool | str"; d.actual = types.program.nameOf(t); d.example = "str(42)";
            throw d;
        }
        if (expr.text == "strFromByte") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span);
            return Type::Str;
        }
        if (expr.text == "parseInt") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            return Type::Int;
        }
        if (expr.text == "parseFloat") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            return Type::Float;
        }
        if (expr.text == "tryParseInt") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return Type::Int;
        }
        if (expr.text == "tryParseFloat") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Float, expr.args[1].span);
            return Type::Float;
        }
        if (expr.text == "strContains" || expr.text == "strStartsWith" || expr.text == "strEndsWith") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return Type::Bool;
        }
        if (expr.text == "strIndexOf") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return Type::Int;
        }
        if (expr.text == "sbNew") {
            checkCallArity(expr, 0);
            return Type::StringBuilder;
        }
        if (expr.text == "sbWithCap") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span);
            return Type::StringBuilder;
        }
        if (expr.text == "sbPush") {
            checkCallArity(expr, 2); checkMutSbBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "sbPush solo admite mut en StringBuilder", expr.args[1].span, R"(sbPush(mut sb, "x"))");
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "sbPushByte") {
            checkCallArity(expr, 2); checkMutSbBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "sbPushByte solo admite mut en StringBuilder", expr.args[1].span, "sbPushByte(mut sb, 65)");
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "sbPushInt") {
            checkCallArity(expr, 2); checkMutSbBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "sbPushInt solo admite mut en StringBuilder", expr.args[1].span, "sbPushInt(mut sb, 42)");
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "sbPushFloat") {
            checkCallArity(expr, 2); checkMutSbBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "sbPushFloat solo admite mut en StringBuilder", expr.args[1].span, "sbPushFloat(mut sb, 1.5)");
            requireType(typeOf(expr.args[1]), Type::Float, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "sbClear") {
            checkCallArity(expr, 1); checkMutSbBuiltin(expr);
            return Type::Void;
        }
        if (expr.text == "sbStr") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::StringBuilder, expr.args[0].span);
            return Type::Str;
        }
        if (expr.text == "sbTake") {
            checkCallArity(expr, 1); checkMutSbBuiltin(expr);
            return Type::Str;
        }
        if (expr.text == "cap") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type t = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isArray(t) && t != Type::StringBuilder &&
                !types.program.isMap(t) && !types.program.isSet(t) && !types.program.isPool(t)) {
                Diagnostic d("E_TYPE", "cap admite [T], StringBuilder, Map<K, V>, Set<T> o Pool<T>", expr.args[0].span);
                d.expected = "[T] | StringBuilder | Map<K, V> | Set<T> | Pool<T>";
                d.actual = types.program.nameOf(t);
                throw d;
            }
            return Type::Int;
        }
        if (expr.text == "push") {
            checkCallArity(expr, 2);
            const Type arrT = checkMutArrayBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "push solo admite mut en el array", expr.args[1].span, "push(mut a, v)");
            const Type elemT = types.program.elemOf(arrT);
            DestGuard dg(activeMoveDest, "push(...)");
            Type valT = typeOf(expr.args[1], ArrayUse::Copy, elemT);
            coerceIfDyn(expr.args[1], valT, elemT, expr.span);
            requireType(valT, elemT, expr.args[1].span, expr.span);
            return Type::Void;
        }
        if (expr.text == "pop") {
            checkCallArity(expr, 1);
            return types.program.elemOf(checkMutArrayBuiltin(expr));
        }
        if (expr.text == "clear") {
            checkCallArity(expr, 1);
            checkMutCollectionBuiltin(expr);
            return Type::Void;
        }
        if (expr.text == "truncate") {
            checkCallArity(expr, 2);
            checkMutArrayBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "truncate solo admite mut en el array", expr.args[1].span, "truncate(mut a, 4)");
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "reserve") {
            checkCallArity(expr, 2);
            checkMutCollectionBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "reserve solo admite mut en la colección", expr.args[1].span, "reserve(mut a, 16)");
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return Type::Void;
        }
        if (expr.text == "sort" || expr.text == "sortStable") {
            checkCallArity(expr, 1);
            const Type arrT = checkMutArrayBuiltin(expr);
            const Type elemT = types.program.elemOf(arrT);
            if (!Program::isNumericType(elemT) && elemT != Type::Str && elemT != Type::Bool) {
                Diagnostic d("E_TYPE", expr.text + " admite arreglos numéricos, [str] o [bool]", expr.args[0].span);
                d.expected = "[int] | [i8..u64] | [float] | [f32] | [str] | [bool]";
                d.actual = types.program.nameOf(arrT);
                d.example = expr.text + "(mut arr)";
                throw d;
            }
            return Type::Void;
        }
        if (expr.text == "mapNew" || expr.text == "mapSecure") {
            checkCallArity(expr, 0);
            Type target = expected;
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != 2)
                    fail("E_GENERIC", expr.text + " requiere 2 argumentos de tipo <K, V>", expr.span,
                         "var m = " + expr.text + "<str, int>()");
                for (Type ta : expr.typeArgs) known(ta, expr.span, false);
                const Type explicitMapT = types.program.mapOf(expr.typeArgs[0], expr.typeArgs[1]);
                if (expected != Type::Unknown && expected != explicitMapT) {
                    Diagnostic d("E_TYPE", "Tipo incompatible para " + expr.text, expr.span);
                    d.expected = types.program.nameOf(expected);
                    d.actual = types.program.nameOf(explicitMapT);
                    d.example = "var m: Map<str, int> = " + expr.text + "()";
                    throw d;
                }
                target = explicitMapT;
            }
            if (target == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir Map<K, V> para " + expr.text + "; anota el tipo",
                     expr.span, "let m: Map<str, int> = " + expr.text + "()");
            if (!types.program.isMap(target)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para " + expr.text, expr.span);
                d.expected = types.program.nameOf(target);
                d.actual = "Map<K, V>";
                d.example = "let m: Map<str, int> = " + expr.text + "()";
                throw d;
            }
            types.ensureInstantiated(target, expr.span);
            return target;
        }
        if (expr.text == "mapWithCap") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span);
            Type target = expected;
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != 2)
                    fail("E_GENERIC", "mapWithCap requiere 2 argumentos de tipo <K, V>", expr.span,
                         "var m = mapWithCap<str, int>(16)");
                for (Type ta : expr.typeArgs) known(ta, expr.span, false);
                const Type explicitMapT = types.program.mapOf(expr.typeArgs[0], expr.typeArgs[1]);
                if (expected != Type::Unknown && expected != explicitMapT) {
                    Diagnostic d("E_TYPE", "Tipo incompatible para mapWithCap", expr.span);
                    d.expected = types.program.nameOf(expected);
                    d.actual = types.program.nameOf(explicitMapT);
                    d.example = "var m: Map<str, int> = mapWithCap(16)";
                    throw d;
                }
                target = explicitMapT;
            }
            if (target == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir Map<K, V> para mapWithCap; anota el tipo",
                     expr.span, "let m: Map<str, int> = mapWithCap(16)");
            if (!types.program.isMap(target)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para mapWithCap", expr.span);
                d.expected = types.program.nameOf(target);
                d.actual = "Map<K, V>";
                d.example = "let m: Map<str, int> = mapWithCap(16)";
                throw d;
            }
            types.ensureInstantiated(target, expr.span);
            return target;
        }
        if (expr.text == "mapSet") {
            checkCallArity(expr, 3);
            const Type mapT = checkMutMapBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "mapSet solo admite mut en el mapa", expr.args[1].span, R"(mapSet(mut m, "k", 1))");
            if (expr.args[2].isMutArg) fail("E_MUT_ARG", "mapSet solo admite mut en el mapa", expr.args[2].span, R"(mapSet(mut m, "k", 1))");
            const Type kT = types.program.mapKey(mapT);
            const Type vT = types.program.mapVal(mapT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect, kT), kT, expr.args[1].span, expr.span);
            Type valT = typeOf(expr.args[2], ArrayUse::Copy, vT);
            coerceIfDyn(expr.args[2], valT, vT, expr.span);
            requireType(valT, vT, expr.args[2].span, expr.span);
            return Type::Void;
        }
        if (expr.text == "mapGet") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type mapT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isMap(mapT)) {
                Diagnostic d("E_TYPE", "mapGet requiere Map<K, V>", expr.args[0].span);
                d.expected = "Map<K, V>"; d.actual = types.program.nameOf(mapT); d.example = R"(mapGet(m, "k"))";
                throw d;
            }
            const Type kT = types.program.mapKey(mapT);
            const Type vT = types.program.mapVal(mapT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect, kT), kT, expr.args[1].span, expr.args[0].span);
            const Type optT = types.program.optionOf(vT);
            types.ensureInstantiated(optT, expr.span);
            return optT;
        }
        if (expr.text == "mapHas") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type mapT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isMap(mapT)) {
                Diagnostic d("E_TYPE", "mapHas requiere Map<K, V>", expr.args[0].span);
                d.expected = "Map<K, V>"; d.actual = types.program.nameOf(mapT); d.example = R"(mapHas(m, "k"))";
                throw d;
            }
            const Type kT = types.program.mapKey(mapT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect, kT), kT, expr.args[1].span, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "mapDel") {
            checkCallArity(expr, 2);
            const Type mapT = checkMutMapBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "mapDel solo admite mut en el mapa", expr.args[1].span, R"(mapDel(mut m, "k"))");
            const Type kT = types.program.mapKey(mapT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect, kT), kT, expr.args[1].span, expr.span);
            return Type::Bool;
        }
        if (expr.text == "mapKeys" || expr.text == "mapValues") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type mapT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isMap(mapT)) {
                Diagnostic d("E_TYPE", expr.text + " requiere Map<K, V>", expr.args[0].span);
                d.expected = "Map<K, V>"; d.actual = types.program.nameOf(mapT); d.example = expr.text + "(m)";
                throw d;
            }
            return types.program.arrayOf(expr.text == "mapKeys" ? types.program.mapKey(mapT) : types.program.mapVal(mapT));
        }
        if (expr.text == "setNew" || expr.text == "setSecure") {
            checkCallArity(expr, 0);
            Type target = expected;
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != 1)
                    fail("E_GENERIC", expr.text + " requiere 1 argumento de tipo <T>", expr.span,
                         "var s = " + expr.text + "<int>()");
                known(expr.typeArgs[0], expr.span, false);
                const Type explicitSetT = types.program.setOf(expr.typeArgs[0]);
                if (expected != Type::Unknown && expected != explicitSetT) {
                    Diagnostic d("E_TYPE", "Tipo incompatible para " + expr.text, expr.span);
                    d.expected = types.program.nameOf(expected);
                    d.actual = types.program.nameOf(explicitSetT);
                    d.example = "var s: Set<int> = " + expr.text + "()";
                    throw d;
                }
                target = explicitSetT;
            }
            if (target == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir Set<T> para " + expr.text + "; anota el tipo",
                     expr.span, "let s: Set<int> = " + expr.text + "()");
            if (!types.program.isSet(target)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para " + expr.text, expr.span);
                d.expected = types.program.nameOf(target);
                d.actual = "Set<T>";
                d.example = "let s: Set<int> = " + expr.text + "()";
                throw d;
            }
            types.ensureInstantiated(target, expr.span);
            return target;
        }
        if (expr.text == "setWithCap") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span);
            Type target = expected;
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != 1)
                    fail("E_GENERIC", "setWithCap requiere 1 argumento de tipo <T>", expr.span,
                         "var s = setWithCap<int>(16)");
                known(expr.typeArgs[0], expr.span, false);
                const Type explicitSetT = types.program.setOf(expr.typeArgs[0]);
                if (expected != Type::Unknown && expected != explicitSetT) {
                    Diagnostic d("E_TYPE", "Tipo incompatible para setWithCap", expr.span);
                    d.expected = types.program.nameOf(expected);
                    d.actual = types.program.nameOf(explicitSetT);
                    d.example = "var s: Set<int> = setWithCap(16)";
                    throw d;
                }
                target = explicitSetT;
            }
            if (target == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir Set<T> para setWithCap; anota el tipo",
                     expr.span, "let s: Set<int> = setWithCap(16)");
            if (!types.program.isSet(target)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para setWithCap", expr.span);
                d.expected = types.program.nameOf(target);
                d.actual = "Set<T>";
                d.example = "let s: Set<int> = setWithCap(16)";
                throw d;
            }
            types.ensureInstantiated(target, expr.span);
            return target;
        }
        if (expr.text == "setAdd" || expr.text == "setDel") {
            checkCallArity(expr, 2);
            const Type setT = checkMutSetBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", expr.text + " solo admite mut en el conjunto", expr.args[1].span, expr.text + "(mut s, x)");
            const Type elemT = types.program.setElem(setT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect, elemT), elemT, expr.args[1].span, expr.span);
            return Type::Bool;
        }
        if (expr.text == "setHas") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type setT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isSet(setT)) {
                Diagnostic d("E_TYPE", "setHas requiere Set<T>", expr.args[0].span);
                d.expected = "Set<T>"; d.actual = types.program.nameOf(setT); d.example = "setHas(s, x)";
                throw d;
            }
            const Type elemT = types.program.setElem(setT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect, elemT), elemT, expr.args[1].span, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "setItems") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type setT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isSet(setT)) {
                Diagnostic d("E_TYPE", "setItems requiere Set<T>", expr.args[0].span);
                d.expected = "Set<T>"; d.actual = types.program.nameOf(setT); d.example = "setItems(s)";
                throw d;
            }
            return types.program.arrayOf(types.program.setElem(setT));
        }
        if (expr.text == "isSome" || expr.text == "isNone") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type t = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isOption(t)) {
                Diagnostic d("E_TYPE", expr.text + " requiere un valor opcional T?", expr.args[0].span);
                d.expected = "T?"; d.actual = types.program.nameOf(t); d.example = expr.text + "(Some(1))";
                throw d;
            }
            return Type::Bool;
        }
        if (expr.text == "isOk" || expr.text == "isErr") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type t = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isResult(t)) {
                Diagnostic d("E_TYPE", expr.text + " requiere Result<T, E>", expr.args[0].span);
                d.expected = "Result<T, E>"; d.actual = types.program.nameOf(t); d.example = expr.text + "(res)";
                throw d;
            }
            return Type::Bool;
        }
        if (expr.text == "unwrapOr") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type t = typeOf(expr.args[0], ArrayUse::Copy);
            Type okT = types.program.isOption(t) ? types.program.optionInner(t)
                     : (types.program.isResult(t) ? types.program.resultOk(t) : Type::Unknown);
            if (okT == Type::Unknown) {
                Diagnostic d("E_TYPE", "unwrapOr requiere T? o Result<T, E>", expr.args[0].span);
                d.expected = "T? | Result<T, E>"; d.actual = types.program.nameOf(t); d.example = "unwrapOr(opt, 0)";
                throw d;
            }
            types.ensureInstantiated(okT, expr.span);
            Type fbT = typeOf(expr.args[1], ArrayUse::Copy, okT);
            coerceIfDyn(expr.args[1], fbT, okT, expr.args[0].span);
            requireType(fbT, okT, expr.args[1].span, expr.args[0].span);
            return okT;
        }
        if (expr.text == "poolNew") {
            checkCallArity(expr, 0);
            Type target = expected;
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != 1)
                    fail("E_GENERIC", "poolNew requiere 1 argumento de tipo <T>", expr.span,
                         "var p = poolNew<int>()");
                known(expr.typeArgs[0], expr.span, false);
                const Type explicitPoolT = types.program.poolOf(expr.typeArgs[0]);
                if (expected != Type::Unknown && expected != explicitPoolT) {
                    Diagnostic d("E_TYPE", "Tipo incompatible para poolNew", expr.span);
                    d.expected = types.program.nameOf(expected);
                    d.actual = types.program.nameOf(explicitPoolT);
                    d.example = "var p: Pool<int> = poolNew()";
                    throw d;
                }
                target = explicitPoolT;
            }
            if (target == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir Pool<T> para poolNew; anota el tipo",
                     expr.span, "var p: Pool<int> = poolNew()");
            if (!types.program.isPool(target)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para poolNew", expr.span);
                d.expected = types.program.nameOf(target);
                d.actual = "Pool<T>";
                d.example = "var p: Pool<int> = poolNew()";
                throw d;
            }
            types.ensureInstantiated(target, expr.span);
            return target;
        }
        if (expr.text == "poolWithCap") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span);
            Type target = expected;
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != 1)
                    fail("E_GENERIC", "poolWithCap requiere 1 argumento de tipo <T>", expr.span,
                         "var p = poolWithCap<int>(16)");
                known(expr.typeArgs[0], expr.span, false);
                const Type explicitPoolT = types.program.poolOf(expr.typeArgs[0]);
                if (expected != Type::Unknown && expected != explicitPoolT) {
                    Diagnostic d("E_TYPE", "Tipo incompatible para poolWithCap", expr.span);
                    d.expected = types.program.nameOf(expected);
                    d.actual = types.program.nameOf(explicitPoolT);
                    d.example = "var p: Pool<int> = poolWithCap(16)";
                    throw d;
                }
                target = explicitPoolT;
            }
            if (target == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir Pool<T> para poolWithCap; anota el tipo",
                     expr.span, "var p: Pool<int> = poolWithCap(16)");
            if (!types.program.isPool(target)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para poolWithCap", expr.span);
                d.expected = types.program.nameOf(target);
                d.actual = "Pool<T>";
                d.example = "var p: Pool<int> = poolWithCap(16)";
                throw d;
            }
            types.ensureInstantiated(target, expr.span);
            return target;
        }
        if (expr.text == "poolAdd") {
            checkCallArity(expr, 2);
            const Type poolT = checkMutPoolBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "poolAdd solo admite mut en el pool", expr.args[1].span, "poolAdd(mut p, x)");
            const Type elemT = types.program.poolElem(poolT);
            Type valT = typeOf(expr.args[1], ArrayUse::Copy, elemT);
            coerceIfDyn(expr.args[1], valT, elemT, expr.span);
            requireType(valT, elemT, expr.args[1].span, expr.span);
            return types.program.handleOf(elemT);
        }
        if (expr.text == "poolGet") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type poolT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isPool(poolT)) {
                Diagnostic d("E_TYPE", "poolGet requiere Pool<T>", expr.args[0].span);
                d.expected = "Pool<T>"; d.actual = types.program.nameOf(poolT); d.example = "poolGet(p, h)";
                throw d;
            }
            const Type elemT = types.program.poolElem(poolT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect), types.program.handleOf(elemT), expr.args[1].span, expr.args[0].span);
            const Type optT = types.program.optionOf(elemT);
            types.ensureInstantiated(optT, expr.span);
            return optT;
        }
        if (expr.text == "poolSet") {
            checkCallArity(expr, 3);
            const Type poolT = checkMutPoolBuiltin(expr);
            if (expr.args[1].isMutArg || expr.args[2].isMutArg)
                fail("E_MUT_ARG", "poolSet solo admite mut en el pool",
                     expr.args[1].isMutArg ? expr.args[1].span : expr.args[2].span, "poolSet(mut p, h, v)");
            const Type elemT = types.program.poolElem(poolT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect), types.program.handleOf(elemT), expr.args[1].span, expr.span);
            Type valT = typeOf(expr.args[2], ArrayUse::Copy, elemT);
            coerceIfDyn(expr.args[2], valT, elemT, expr.span);
            requireType(valT, elemT, expr.args[2].span, expr.span);
            return Type::Bool;
        }
        if (expr.text == "poolRemove") {
            checkCallArity(expr, 2);
            const Type poolT = checkMutPoolBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "poolRemove solo admite mut en el pool", expr.args[1].span, "poolRemove(mut p, h)");
            const Type elemT = types.program.poolElem(poolT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect), types.program.handleOf(elemT), expr.args[1].span, expr.span);
            const Type optT = types.program.optionOf(elemT);
            types.ensureInstantiated(optT, expr.span);
            return optT;
        }
        if (expr.text == "poolHas") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type poolT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isPool(poolT)) {
                Diagnostic d("E_TYPE", "poolHas requiere Pool<T>", expr.args[0].span);
                d.expected = "Pool<T>"; d.actual = types.program.nameOf(poolT); d.example = "poolHas(p, h)";
                throw d;
            }
            const Type elemT = types.program.poolElem(poolT);
            requireType(typeOf(expr.args[1], ArrayUse::Inspect), types.program.handleOf(elemT), expr.args[1].span, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "poolHandles") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type poolT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isPool(poolT)) {
                Diagnostic d("E_TYPE", "poolHandles requiere Pool<T>", expr.args[0].span);
                d.expected = "Pool<T>"; d.actual = types.program.nameOf(poolT); d.example = "poolHandles(p)";
                throw d;
            }
            return types.program.arrayOf(types.program.handleOf(types.program.poolElem(poolT)));
        }
        if (expr.text == "sharedNew") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type elemExp = (expected != Type::Unknown && types.program.isShared(expected))
                ? types.program.sharedElem(expected) : Type::Unknown;
            Type valT = typeOf(expr.args[0], ArrayUse::Copy, elemExp);
            coerceIfDyn(expr.args[0], valT, elemExp, expr.span);
            requireValue(valT, expr.args[0].span);
            const Type shT = types.program.sharedOf(valT);
            types.ensureInstantiated(shT, expr.span);
            return shT;
        }
        if (expr.text == "sharedGet") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type shT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isShared(shT)) {
                Diagnostic d("E_TYPE", "sharedGet requiere Shared<T>", expr.args[0].span);
                d.expected = "Shared<T>"; d.actual = types.program.nameOf(shT); d.example = "sharedGet(s)";
                throw d;
            }
            return types.program.sharedElem(shT);
        }
        if (expr.text == "sharedSet") {
            checkCallArity(expr, 2);
            const Type shT = checkMutSharedBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "sharedSet solo admite mut en Shared<T>", expr.args[1].span, "sharedSet(mut s, v)");
            const Type elemT = types.program.sharedElem(shT);
            Type valT = typeOf(expr.args[1], ArrayUse::Copy, elemT);
            coerceIfDyn(expr.args[1], valT, elemT, expr.span);
            requireType(valT, elemT, expr.args[1].span, expr.span);
            return Type::Void;
        }
        if (expr.text == "sharedUpdate") {
            checkCallArity(expr, 2);
            const Type shT = checkMutSharedBuiltin(expr);
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "sharedUpdate solo admite mut en Shared<T>", expr.args[1].span, "sharedUpdate(mut s, |x| x + 1)");
            const Type elemT = types.program.sharedElem(shT);
            const Type expectedFn = types.program.fnOf(elemT, {elemT});
            const Type fnT = typeOf(expr.args[1], ArrayUse::Ordinary, expectedFn);
            requireType(fnT, expectedFn, expr.args[1].span, expr.span);
            return Type::Void;
        }
        auto resStr = [&](Type okT) -> Type {
            types.ensureInstantiated(okT, expr.span);
            const Type rt = types.program.resultOf(okT, Type::Str);
            types.ensureInstantiated(rt, expr.span);
            return rt;
        };
        if (expr.text == "fileOpen") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return resStr(Type::File);
        }
        if (expr.text == "tryReadFile") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            return resStr(Type::Str);
        }
        if (expr.text == "tryWriteFile") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return resStr(Type::Bool);
        }
        if (expr.text == "fileRead") {
            checkCallArity(expr, 2);
            checkMutResourceBuiltin(expr, Type::File, "fileRead(mut f, 1024)");
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "fileRead solo admite mut en File", expr.args[1].span, "fileRead(mut f, 1024)");
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return resStr(Type::Str);
        }
        if (expr.text == "fileReadAll") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::File, "fileReadAll(mut f)");
            return resStr(Type::Str);
        }
        if (expr.text == "fileReadLine") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::File, "fileReadLine(mut f)");
            return resStr(types.program.optionOf(Type::Str));
        }
        if (expr.text == "fileWrite") {
            checkCallArity(expr, 2);
            checkMutResourceBuiltin(expr, Type::File, R"(fileWrite(mut f, "hola"))");
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "fileWrite solo admite mut en File", expr.args[1].span, R"(fileWrite(mut f, "hola"))");
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return resStr(Type::Int);
        }
        if (expr.text == "fileFlush") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::File, "fileFlush(mut f)");
            return resStr(Type::Bool);
        }
        if (expr.text == "fileClose") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::File, "fileClose(mut f)");
            return Type::Void;
        }
        if (expr.text == "fileIsOpen") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::File, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "tcpListen") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return resStr(Type::TcpListener);
        }
        if (expr.text == "tcpLocalPort") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::TcpListener, expr.args[0].span);
            return Type::Int;
        }
        if (expr.text == "tcpAccept") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::TcpListener, "tcpAccept(mut listener)");
            return resStr(Type::Socket);
        }
        if (expr.text == "tcpConnect") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return resStr(Type::Socket);
        }
        if (expr.text == "socketRead") {
            checkCallArity(expr, 2);
            checkMutResourceBuiltin(expr, Type::Socket, "socketRead(mut s, 1024)");
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "socketRead solo admite mut en Socket", expr.args[1].span, "socketRead(mut s, 1024)");
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            return resStr(Type::Str);
        }
        if (expr.text == "socketWrite") {
            checkCallArity(expr, 2);
            checkMutResourceBuiltin(expr, Type::Socket, R"(socketWrite(mut s, "ping"))");
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "socketWrite solo admite mut en Socket", expr.args[1].span, R"(socketWrite(mut s, "ping"))");
            requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
            return resStr(Type::Int);
        }
        if (expr.text == "socketClose") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::Socket, "socketClose(mut s)");
            return Type::Void;
        }
        if (expr.text == "socketIsOpen") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::Socket, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "listenerClose") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::TcpListener, "listenerClose(mut listener)");
            return Type::Void;
        }
        if (expr.text == "listenerIsOpen") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::TcpListener, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "mutexNew") {
            checkCallArity(expr, 0);
            return Type::Mutex;
        }
        if (expr.text == "mutexLock") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::Mutex, "mutexLock(mut m)");
            return Type::Lock;
        }
        if (expr.text == "mutexTryLock") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::Mutex, "mutexTryLock(mut m)");
            const Type optT = types.program.optionOf(Type::Lock);
            types.ensureInstantiated(optT, expr.span);
            return optT;
        }
        if (expr.text == "unlock") {
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::Lock, "unlock(mut g)");
            return Type::Void;
        }
        if (expr.text == "lockIsHeld") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::Lock, expr.args[0].span);
            return Type::Bool;
        }
        auto failNotSend = [&](Type badT, Span where, std::string msg, std::string ex) {
            Diagnostic d("E_SEND", std::move(msg), where);
            d.actual = types.program.nameOf(badT);
            d.example = std::move(ex);
            throw d;
        };
        if (expr.text == "taskScope") {
            checkCallArity(expr, 0);
            return Type::Scope;
        }
        if (expr.text == "taskSpawn") {
            checkCallArity(expr, 2);
            checkMutResourceBuiltin(expr, Type::Scope, "taskSpawn(mut s, || 42)");
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "taskSpawn solo admite mut en Scope", expr.args[1].span, "taskSpawn(mut s, || 42)");
            if (expr.args[1].kind != ExprKind::Closure) {
                Diagnostic d("E_SEND", "taskSpawn requiere una closure literal || ... para validar sus capturas entre hilos.", expr.args[1].span);
                d.example = "let t = taskSpawn(mut s, || work())";
                throw d;
            }
            if (!expr.args[1].closure->params.empty()) {
                Diagnostic d("E_ARITY", "La closure de taskSpawn no lleva parámetros; usa || ...", expr.args[1].span);
                d.expected = "0";
                d.actual = std::to_string(expr.args[1].closure->params.size());
                throw d;
            }
            const Type expRet = (expected != Type::Unknown && types.program.isTask(expected))
                ? types.program.taskElem(expected) : Type::Unknown;
            const Type expFn = (expRet != Type::Unknown) ? types.program.fnOf(expRet, {}) : Type::Unknown;
            const Type fnT = inferClosure(expr.args[1], expFn, false, true);
            expr.args[1].type = fnT;
            const Type retT = types.program.fnResult(fnT);
            if (retT != Type::Void && !types.program.isSendable(retT)) {
                failNotSend(retT, expr.args[1].span,
                            "El retorno de taskSpawn (" + types.program.nameOf(retT) + ") no puede enviarse entre hilos.",
                            "let t = taskSpawn(mut s, || 42)");
            }
            const Type taskT = types.program.taskOf(retT);
            types.ensureInstantiated(taskT, expr.span);
            return taskT;
        }
        if (expr.text == "taskSpawnMove") {
            checkCallArity(expr, 3);
            checkMutResourceBuiltin(expr, Type::Scope, "taskSpawnMove(mut s, val, |v| ...)");
            if (expr.args[1].isMutArg) fail("E_MUT_ARG", "taskSpawnMove mueve el valor sin mut en el segundo argumento", expr.args[1].span, "taskSpawnMove(mut s, val, |v| ...)");
            if (expr.args[2].isMutArg) fail("E_MUT_ARG", "taskSpawnMove solo admite mut en Scope", expr.args[2].span, "taskSpawnMove(mut s, val, |v| ...)");
            DestGuard dg(activeMoveDest, "taskSpawnMove");
            const Type valT = typeOf(expr.args[1], ArrayUse::Copy);
            requireValue(valT, expr.args[1].span);
            if (!types.program.isSendable(valT)) {
                failNotSend(valT, expr.args[1].span,
                            "El valor '" + types.program.nameOf(valT) + "' no puede moverse a otro hilo.",
                            "let fz = freeze(val)");
            }
            if (expr.args[2].kind != ExprKind::Closure) {
                Diagnostic d("E_SEND", "taskSpawnMove requiere una closure literal |v| ... para validar sus capturas entre hilos.", expr.args[2].span);
                d.example = "let t = taskSpawnMove(mut s, xs, |v| len(v))";
                throw d;
            }
            if (expr.args[2].closure->params.size() != 1) {
                Diagnostic d("E_ARITY", "La closure de taskSpawnMove requiere exactamente 1 parámetro |v|", expr.args[2].span);
                d.expected = "1";
                d.actual = std::to_string(expr.args[2].closure->params.size());
                throw d;
            }
            const Type expRet = (expected != Type::Unknown && types.program.isTask(expected))
                ? types.program.taskElem(expected) : Type::Unknown;
            const Type expFn = types.program.fnOf(expRet, {valT});
            const Type fnT = inferClosure(expr.args[2], expFn, true, true);
            expr.args[2].type = fnT;
            const auto params = types.program.fnParams(fnT);
            requireType(params.at(0), valT, expr.args[2].span, expr.args[1].span);
            const Type retT = types.program.fnResult(fnT);
            if (retT != Type::Void && !types.program.isSendable(retT)) {
                failNotSend(retT, expr.args[2].span,
                            "El retorno de taskSpawnMove (" + types.program.nameOf(retT) + ") no puede enviarse entre hilos.",
                            "let t = taskSpawnMove(mut s, val, |v| ...)");
            }
            const Type taskT = types.program.taskOf(retT);
            types.ensureInstantiated(taskT, expr.span);
            return taskT;
        }
        if (expr.text == "taskJoin") {
            checkCallArity(expr, 1); rejectMutArgs();
            DestGuard dg(activeMoveDest, "taskJoin");
            const Type tT = typeOf(expr.args[0], ArrayUse::Copy);
            if (!types.program.isTask(tT)) {
                Diagnostic d("E_TYPE", "taskJoin requiere Task<T>", expr.args[0].span);
                d.expected = "Task<T>";
                d.actual = types.program.nameOf(tT);
                d.example = "let x = taskJoin(t)";
                throw d;
            }
            return types.program.taskElem(tT);
        }
        if (expr.text == "parallelMap") {
            checkCallArity(expr, 2); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Int, expr.args[0].span);
            if (expr.args[1].kind != ExprKind::Closure) {
                Diagnostic d("E_SEND", "parallelMap requiere una closure literal |i| ... para validar sus capturas entre hilos.", expr.args[1].span);
                d.example = "let ys = parallelMap(n, |i| i * 2)";
                throw d;
            }
            if (expr.args[1].closure->params.size() != 1) {
                Diagnostic d("E_ARITY", "La closure de parallelMap requiere exactamente 1 parámetro |i|", expr.args[1].span);
                d.expected = "1";
                d.actual = std::to_string(expr.args[1].closure->params.size());
                throw d;
            }
            const Type expElem = (expected != Type::Unknown && types.program.isArray(expected))
                ? types.program.elemOf(expected) : Type::Unknown;
            const Type fnT = inferClosure(expr.args[1], types.program.fnOf(expElem, {Type::Int}), false, true);
            expr.args[1].type = fnT;
            const auto params = types.program.fnParams(fnT);
            requireType(params.at(0), Type::Int, expr.args[1].span, expr.args[0].span);
            const Type retT = types.program.fnResult(fnT);
            requireValue(retT, expr.args[1].span);
            if (!types.program.isSendable(retT)) {
                failNotSend(retT, expr.args[1].span,
                            "El retorno de parallelMap (" + types.program.nameOf(retT) + ") no puede enviarse entre hilos.",
                            "let ys = parallelMap(n, |i| i * 2)");
            }
            const Type arrT = types.program.arrayOf(retT);
            types.ensureInstantiated(arrT, expr.span);
            return arrT;
        }
        if (expr.text == "freeze") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type elemExp = (expected != Type::Unknown && types.program.isFrozen(expected))
                ? types.program.frozenElem(expected) : Type::Unknown;
            Type valT = typeOf(expr.args[0], ArrayUse::Copy, elemExp);
            coerceIfDyn(expr.args[0], valT, elemExp, expr.span);
            requireValue(valT, expr.args[0].span);
            if (types.program.isResource(valT)) {
                failResourceCopy("freeze no admite recursos no copiables ('" + types.program.nameOf(valT) + "'); muévelo con taskSpawnMove.",
                                 expr.args[0].span);
            }
            if (!types.program.isSendable(valT)) {
                failNotSend(valT, expr.args[0].span,
                            "freeze requiere un valor enviable entre hilos, no '" + types.program.nameOf(valT) + "'.",
                            "let fz = freeze(arr)");
            }
            const Type fzT = types.program.frozenOf(valT);
            types.ensureInstantiated(fzT, expr.span);
            return fzT;
        }
        if (expr.text == "frozenGet") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type fzT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isFrozen(fzT)) {
                Diagnostic d("E_TYPE", "frozenGet requiere Frozen<T>", expr.args[0].span);
                d.expected = "Frozen<T>";
                d.actual = types.program.nameOf(fzT);
                d.example = "let xs = frozenGet(fz)";
                throw d;
            }
            return types.program.frozenElem(fzT);
        }
        if (expr.text == "frozenRead") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type fzT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isFrozen(fzT)) {
                Diagnostic d("E_TYPE", "frozenRead requiere Frozen<T>", expr.args[0].span);
                d.expected = "Frozen<T>";
                d.actual = types.program.nameOf(fzT);
                d.example = "let n = frozenRead(fz, |x| len(x))";
                throw d;
            }
            const Type elemT = types.program.frozenElem(fzT);
            const Type expFn = types.program.fnOf(expected, {elemT});
            const Type fnT = typeOf(expr.args[1], ArrayUse::Ordinary, expFn);
            if (!types.program.isFn(fnT)) {
                Diagnostic d("E_TYPE", "frozenRead requiere una closure o función fn(T) -> R", expr.args[1].span);
                d.expected = "fn(" + types.program.nameOf(elemT) + ") -> R";
                d.actual = types.program.nameOf(fnT);
                throw d;
            }
            const auto params = types.program.fnParams(fnT);
            if (params.size() != 1) {
                Diagnostic d("E_ARITY", "La función de frozenRead requiere exactamente 1 parámetro", expr.args[1].span);
                d.expected = "1";
                d.actual = std::to_string(params.size());
                throw d;
            }
            requireType(params[0], elemT, expr.args[1].span, expr.args[0].span);
            return types.program.fnResult(fnT);
        }
        auto parseAtomicOrdering = [&](const Expr& arg, std::string_view fnLabel,
                                       bool allowRelaxed, bool allowAcquire, bool allowRelease, bool allowAcqRel) -> unsigned {
            std::string allowed;
            auto addAllowed = [&](std::string_view name) {
                if (!allowed.empty()) allowed += " | ";
                allowed += "\"";
                allowed += name;
                allowed += "\"";
            };
            if (allowRelaxed) addAllowed("relaxed");
            if (allowAcquire) addAllowed("acquire");
            if (allowRelease) addAllowed("release");
            if (allowAcqRel) addAllowed("acqRel");
            addAllowed("seqCst");
            if (arg.kind != ExprKind::String) {
                Diagnostic d("E_ORDERING",
                             std::string(fnLabel) + " requiere un literal de cadena constante para el ordenamiento (" + allowed + ").",
                             arg.span);
                d.expected = allowed;
                d.example = std::string(fnLabel) + "(..., \"seqCst\")";
                throw d;
            }
            const std::string& o = arg.text;
            if (o == "relaxed" && allowRelaxed) return 0u;
            if (o == "acquire" && allowAcquire) return 1u;
            if (o == "release" && allowRelease) return 2u;
            if (o == "acqRel" && allowAcqRel) return 3u;
            if (o == "seqCst") return 4u;
            Diagnostic d("E_ORDERING",
                         "Ordenamiento de memoria inválido '" + o + "' para " + std::string(fnLabel) +
                             "; usa " + allowed + ".",
                         arg.span);
            d.expected = allowed;
            d.actual = "\"" + o + "\"";
            d.example = std::string(fnLabel) + "(..., \"seqCst\")";
            throw d;
        };
        auto requireAtomicArg = [&](Expr& arg, std::string_view fnLabel, bool requireIntElem) -> Type {
            const Type at = typeOf(arg, ArrayUse::Inspect);
            if (!types.program.isAtomic(at)) {
                Diagnostic d("E_TYPE", std::string(fnLabel) + " requiere Atomic<T>", arg.span);
                d.expected = "Atomic<T>";
                d.actual = types.program.nameOf(at);
                d.example = std::string(fnLabel) + "(a, ...)";
                throw d;
            }
            const Type elemT = types.program.atomicElem(at);
            if (!mentionsOpaqueType(types.program, elemT) &&
                ((requireIntElem && !Program::isIntType(elemT)) ||
                 (!requireIntElem && elemT != Type::Bool && !Program::isIntType(elemT)))) {
                Diagnostic d("E_ATOMIC_TYPE",
                             std::string(fnLabel) + " no admite Atomic<" + types.program.nameOf(elemT) + ">" +
                                 (requireIntElem ? " (requiere tipo entero)" : ""),
                             arg.span);
                d.expected = requireIntElem ? "int | i8..u64" : "bool | int | i8..u64";
                d.actual = types.program.nameOf(elemT);
                throw d;
            }
            return elemT;
        };
        if (expr.text == "atomicNew") {
            checkCallArity(expr, 1); rejectMutArgs();
            Type hint = Type::Unknown;
            if (expr.typeArgs.size() > 1) {
                Diagnostic d("E_GENERIC", "atomicNew admite a lo sumo 1 argumento de tipo <T>", expr.span);
                d.example = "atomicNew<u64>(0)";
                throw d;
            }
            if (expr.typeArgs.size() == 1) {
                known(expr.typeArgs[0], expr.span, false);
                hint = expr.typeArgs[0];
            } else if (expected != Type::Unknown && types.program.isAtomic(expected)) {
                hint = types.program.atomicElem(expected);
            }
            const Type valT = typeOf(expr.args[0], ArrayUse::Ordinary, hint);
            if (expr.typeArgs.size() == 1) requireType(valT, hint, expr.args[0].span);
            if (!mentionsOpaqueType(types.program, valT) && valT != Type::Bool && !Program::isIntType(valT)) {
                Diagnostic d("E_ATOMIC_TYPE",
                             "atomicNew solo admite bool o tipos enteros (i8..u64, int), no '" + types.program.nameOf(valT) + "'.",
                             expr.args[0].span);
                d.expected = "bool | int | i8..u64";
                d.actual = types.program.nameOf(valT);
                d.example = "let a = atomicNew(0)";
                throw d;
            }
            const Type at = types.program.atomicOf(valT);
            if (!mentionsOpaqueType(types.program, at)) types.ensureInstantiated(at, expr.span);
            return at;
        }
        if (expr.text == "atomicLoad" || expr.text == "atomicLoadOrd") {
            const bool hasOrd = (expr.text == "atomicLoadOrd");
            checkCallArity(expr, hasOrd ? 2 : 1); rejectMutArgs();
            const Type elemT = requireAtomicArg(expr.args[0], expr.text, false);
            if (hasOrd) {
                requireType(typeOf(expr.args[1]), Type::Str, expr.args[1].span);
                (void)parseAtomicOrdering(expr.args[1], expr.text, true, true, false, false);
            }
            return elemT;
        }
        if (expr.text == "atomicStore" || expr.text == "atomicStoreOrd") {
            const bool hasOrd = (expr.text == "atomicStoreOrd");
            checkCallArity(expr, hasOrd ? 3 : 2); rejectMutArgs();
            const Type elemT = requireAtomicArg(expr.args[0], expr.text, false);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, elemT), elemT, expr.args[1].span, expr.args[0].span);
            if (hasOrd) {
                requireType(typeOf(expr.args[2]), Type::Str, expr.args[2].span);
                (void)parseAtomicOrdering(expr.args[2], expr.text, true, false, true, false);
            }
            return Type::Void;
        }
        if (expr.text == "atomicSwap" || expr.text == "atomicSwapOrd") {
            const bool hasOrd = (expr.text == "atomicSwapOrd");
            checkCallArity(expr, hasOrd ? 3 : 2); rejectMutArgs();
            const Type elemT = requireAtomicArg(expr.args[0], expr.text, false);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, elemT), elemT, expr.args[1].span, expr.args[0].span);
            if (hasOrd) {
                requireType(typeOf(expr.args[2]), Type::Str, expr.args[2].span);
                (void)parseAtomicOrdering(expr.args[2], expr.text, true, true, true, true);
            }
            return elemT;
        }
        if (expr.text == "atomicCas" || expr.text == "atomicCasOrd") {
            const bool hasOrd = (expr.text == "atomicCasOrd");
            checkCallArity(expr, hasOrd ? 4 : 3); rejectMutArgs();
            const Type elemT = requireAtomicArg(expr.args[0], expr.text, false);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, elemT), elemT, expr.args[1].span, expr.args[0].span);
            requireType(typeOf(expr.args[2], ArrayUse::Ordinary, elemT), elemT, expr.args[2].span, expr.args[0].span);
            if (hasOrd) {
                requireType(typeOf(expr.args[3]), Type::Str, expr.args[3].span);
                (void)parseAtomicOrdering(expr.args[3], expr.text, true, true, true, true);
            }
            return Type::Bool;
        }
        if (expr.text == "atomicAdd" || expr.text == "atomicAddOrd" ||
            expr.text == "atomicSub" || expr.text == "atomicSubOrd" ||
            expr.text == "atomicAnd" || expr.text == "atomicAndOrd" ||
            expr.text == "atomicOr" || expr.text == "atomicOrOrd" ||
            expr.text == "atomicXor" || expr.text == "atomicXorOrd") {
            const bool hasOrd = expr.text.ends_with("Ord");
            checkCallArity(expr, hasOrd ? 3 : 2); rejectMutArgs();
            const Type elemT = requireAtomicArg(expr.args[0], expr.text, true);
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, elemT), elemT, expr.args[1].span, expr.args[0].span);
            if (hasOrd) {
                requireType(typeOf(expr.args[2]), Type::Str, expr.args[2].span);
                (void)parseAtomicOrdering(expr.args[2], expr.text, true, true, true, true);
            }
            return elemT;
        }
        if (expr.text == "atomicFence") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0]), Type::Str, expr.args[0].span);
            (void)parseAtomicOrdering(expr.args[0], "atomicFence", false, true, true, true);
            return Type::Void;
        }
        if (expr.text == "ringNew") {
            checkCallArity(expr, 1); rejectMutArgs();
            Type elemT = Type::Unknown;
            if (expr.typeArgs.size() > 1) {
                Diagnostic d("E_GENERIC", "ringNew requiere exactamente 1 argumento de tipo <T>", expr.span);
                d.example = "let q = ringNew<int>(64)";
                throw d;
            }
            if (expr.typeArgs.size() == 1) elemT = expr.typeArgs[0];
            else if (expected != Type::Unknown && types.program.isRingQueue(expected)) elemT = types.program.ringQueueElem(expected);
            if (elemT == Type::Unknown) {
                Diagnostic d("E_GENERIC", "ringNew requiere argumento de tipo <T> o anotación RingQueue<T>", expr.span);
                d.example = "let q = ringNew<int>(64)";
                throw d;
            }
            known(elemT, expr.span, false);
            requireType(typeOf(expr.args[0], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[0].span);
            const Type rqT = types.program.ringQueueOf(elemT);
            if (!mentionsOpaqueType(types.program, rqT)) types.ensureInstantiated(rqT, expr.span);
            return rqT;
        }
        if (expr.text == "ringPush") {
            checkCallArity(expr, 2); rejectMutArgs();
            const Type rqT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isRingQueue(rqT)) {
                Diagnostic d("E_TYPE", "ringPush requiere RingQueue<T>", expr.args[0].span);
                d.expected = "RingQueue<T>";
                d.actual = types.program.nameOf(rqT);
                d.example = "ringPush(q, x)";
                throw d;
            }
            const Type elemT = types.program.ringQueueElem(rqT);
            const Type valT = typeOf(expr.args[1], ArrayUse::Copy, elemT);
            requireType(valT, elemT, expr.args[1].span, expr.args[0].span);
            return Type::Bool;
        }
        if (expr.text == "ringPop" || expr.text == "ringLen" || expr.text == "ringCap") {
            checkCallArity(expr, 1); rejectMutArgs();
            const Type rqT = typeOf(expr.args[0], ArrayUse::Inspect);
            if (!types.program.isRingQueue(rqT)) {
                Diagnostic d("E_TYPE", expr.text + " requiere RingQueue<T>", expr.args[0].span);
                d.expected = "RingQueue<T>";
                d.actual = types.program.nameOf(rqT);
                d.example = expr.text + "(q)";
                throw d;
            }
            if (expr.text == "ringPop") {
                const Type optT = types.program.optionOf(types.program.ringQueueElem(rqT));
                if (!mentionsOpaqueType(types.program, optT)) types.ensureInstantiated(optT, expr.span);
                return optT;
            }
            return Type::Int;
        }
        if (expr.text == "regionNew") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[0].span);
            return Type::Region;
        }
        if (expr.text == "regionAlloc" || expr.text == "regionAllocAligned") {
            requireUnsafe(expr.span, expr.text);
            const bool isAligned = (expr.text == "regionAllocAligned");
            checkCallArity(expr, isAligned ? 3 : 2);
            if (expr.typeArgs.size() != 1) {
                Diagnostic d("E_GENERIC", expr.text + " requiere exactamente 1 argumento de tipo <T>", expr.span);
                d.example = isAligned ? "regionAllocAligned<f32>(mut r, 8, 32)" : "regionAlloc<i32>(mut r, 64)";
                throw d;
            }
            const Type elemT = expr.typeArgs[0];
            known(elemT, expr.span, true);
            requireRawElem(elemT, expr.span, expr.text);
            if (!mentionsOpaqueType(types.program, elemT)) {
                types.ensureInstantiated(elemT, expr.span);
                Layouts layouts{types, {}, {}};
                layouts.measure(elemT, expr.span);
            }
            checkMutResourceBuiltin(expr, Type::Region, isAligned ? "regionAllocAligned<f32>(mut r, 8, 32)" : "regionAlloc<u8>(mut r, 64)");
            for (std::size_t k = 1; k < expr.args.size(); ++k) {
                if (expr.args[k].isMutArg) fail("E_MUT_ARG", expr.text + " solo admite mut en Region", expr.args[k].span, "regionAlloc<u8>(mut r, 64)");
            }
            requireType(typeOf(expr.args[1], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[1].span);
            if (isAligned) requireType(typeOf(expr.args[2], ArrayUse::Ordinary, Type::Int), Type::Int, expr.args[2].span);
            const Type resT = types.program.mutPtrOf(elemT);
            if (!mentionsOpaqueType(types.program, resT)) types.ensureInstantiated(resT, expr.span);
            return resT;
        }
        if (expr.text == "regionReset") {
            requireUnsafe(expr.span, "regionReset");
            checkCallArity(expr, 1);
            checkMutResourceBuiltin(expr, Type::Region, "regionReset(mut r)");
            return Type::Void;
        }
        if (expr.text == "regionUsed" || expr.text == "regionCapacity") {
            checkCallArity(expr, 1); rejectMutArgs();
            requireType(typeOf(expr.args[0], ArrayUse::Inspect), Type::Region, expr.args[0].span);
            return Type::Int;
        }
        return Type::Unknown;
    }

    Type inferCall(Expr& expr, Type expected = Type::Unknown) {
        if (expr.text == "ord" && expr.args.size() == 1 && !strictSyntax() && !expr.args[0].isMutArg &&
            !isCharLiteral(expr.args[0]) && probeExprType(expr.args[0], {}) == Type::Int) {
            // `ord(s[i])`: `s[i]` ya es el código del byte, así que ord no tiene nada que convertir.
            Expr code = std::move(expr.args[0]);
            expr = std::move(code);
            return typeOf(expr, ArrayUse::Ordinary, expected);
        }
        if (expr.text == "readByte") {
            if (!expr.typeArgs.empty())
                fail("E_GENERIC", "readByte no acepta argumentos de tipo <...>", expr.span, "readByte()");
            checkCallArity(expr, 0);
            return Type::Int;
        }
        if (expr.text == "len") {
            if (!expr.typeArgs.empty())
                fail("E_GENERIC", "len no acepta argumentos de tipo <...>", expr.span, "len(a)");
            checkCallArity(expr, 1);
            if (expr.args[0].isMutArg) fail("E_MUT_ARG", "len no admite mut", expr.args[0].span, "len(a)");
            auto arg = typeOf(expr.args[0], ArrayUse::Inspect);
            if (arg != Type::Str && arg != Type::StringBuilder &&
                !types.program.isArray(arg) && !types.program.isMap(arg) &&
                !types.program.isSet(arg) && !types.program.isPool(arg)) {
                Diagnostic d("E_TYPE", "len admite str, StringBuilder, [T], Map<K, V>, Set<T> o Pool<T>", expr.args[0].span);
                d.expected = "str | StringBuilder | [T] | Map<K, V> | Set<T> | Pool<T>";
                d.actual = types.program.nameOf(arg);
                d.example = "len(repeat(1, 4))";
                throw d;
            }
            return Type::Int;
        }
        if (expr.text == "repeat") {
            if (!expr.typeArgs.empty())
                fail("E_GENERIC", "repeat no acepta argumentos de tipo <...>", expr.span, "repeat(0, 8)");
            checkCallArity(expr, 2);
            for (auto& arg : expr.args)
                if (arg.isMutArg) fail("E_MUT_ARG", "repeat no admite mut", arg.span, "repeat(0, 8)");
            const Type elemExpected = (expected != Type::Unknown && types.program.isArray(expected))
                ? types.program.elemOf(expected) : Type::Unknown;
            Type elemT = typeOf(expr.args[0], ArrayUse::Copy, elemExpected);
            coerceIfDyn(expr.args[0], elemT, elemExpected, expr.span);
            requireValue(elemT, expr.args[0].span);
            if (types.program.isResource(elemT))
                failResourceCopy("repeat no admite recursos no copiables ('" + types.program.nameOf(elemT) + "').", expr.args[0].span);
            requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
            expr.allocation = ArrayAllocation::Owner;
            return types.program.arrayOf(elemT);
        }
        if (numericBuiltinType(expr.text) || expr.text == "str" ||
            expr.text == "sizeOf" || expr.text == "alignOf" || expr.text == "bitcast")
            return inferBuiltinCall(expr, expected);
        if (expr.text.find('.') == std::string::npos) {
            if (auto* varPtr = lookupOptional(expr.text, expr.span)) {
                if (!expr.typeArgs.empty())
                    fail("E_GENERIC", "Las closures no aceptan argumentos de tipo <...>", expr.span, expr.text + "(...)");
                if (types.program.isFn(varPtr->type)) {
                    Expr calleeName{ExprKind::Name, {expr.span.start, expr.span.start + expr.text.size(), expr.span.file}, expr.text, {}};
                    expr.args.insert(expr.args.begin(), std::move(calleeName));
                    expr.kind = ExprKind::ClosureCall;
                    expr.text.clear();
                    return inferClosureCall(expr);
                }
                Diagnostic d("E_CALL_TYPE", "No se puede llamar un valor de tipo " + types.program.nameOf(varPtr->type), expr.span);
                d.actual = types.program.nameOf(varPtr->type);
                d.expected = "fn(...)";
                throw d;
            }
        }
        const auto sourceName = expr.text;
        auto reference = expr.text.find('.') == std::string::npos && !current.module.empty()
            ? current.module + "." + expr.text : expr.text;
        // La biblioteca base vive en el módulo global y se ve desde cualquier módulo.
        if (reference != sourceName && !generics.contains(reference) && !functions.contains(reference)) {
            if (generics.contains(sourceName)) {
                if (generics.templateOf(sourceName).module.empty()) reference = sourceName;
            } else if (auto global = functions.find(sourceName); global != functions.end() && global->second->module.empty()) {
                reference = sourceName;
            }
        }
        if (generics.contains(reference)) {
            visible(reference, expr.span);
            expr.text = reference;
            const Function& tmpl = generics.templateOf(reference);
            checkCallArity(expr, tmpl.params.size(), tmpl.span);
            std::map<Type, Type> deduced;
            for (Type g : tmpl.generics) deduced.emplace(g, Type::Unknown);
            if (!expr.typeArgs.empty()) {
                if (expr.typeArgs.size() != tmpl.generics.size())
                    fail("E_GENERIC", "Cantidad incorrecta de argumentos de tipo para " + sourceName,
                         expr.span, sourceName + "<int>(...)");
                for (std::size_t k = 0; k < expr.typeArgs.size(); ++k) {
                    known(expr.typeArgs[k], expr.span, false);
                    deduced[tmpl.generics[k]] = expr.typeArgs[k];
                }
            }
            auto partialUnify = [&](auto& self, Type paramT, Type actualT) -> void {
                if (actualT == Type::Unknown) return;
                if (auto it = deduced.find(paramT); it != deduced.end()) {
                    if (it->second == Type::Unknown) it->second = actualT;
                    return;
                }
                if (types.program.isArray(paramT) && types.program.isArray(actualT)) {
                    self(self, types.program.elemOf(paramT), types.program.elemOf(actualT));
                    return;
                }
                if (const auto* pApp = types.program.typeAppOf(paramT)) {
                    if (const auto* aApp = types.program.typeAppOf(actualT);
                        aApp && aApp->baseName == pApp->baseName && aApp->args.size() == pApp->args.size()) {
                        for (std::size_t k = 0; k < pApp->args.size(); ++k)
                            self(self, pApp->args[k], aApp->args[k]);
                    }
                }
            };
            std::vector<Type> actual(expr.args.size(), Type::Unknown);
            for (std::size_t i = 0; i < expr.args.size(); ++i) {
                if (expr.args[i].kind != ExprKind::Closure) {
                    actual[i] = typeOf(expr.args[i]);
                    partialUnify(partialUnify, tmpl.params[i].type, actual[i]);
                }
            }
            for (std::size_t i = 0; i < expr.args.size(); ++i) {
                if (expr.args[i].kind == ExprKind::Closure) {
                    const Type hint = substituteTypeMap(tmpl.params[i].type, deduced, types.program);
                    actual[i] = typeOf(expr.args[i], ArrayUse::Ordinary, hint);
                    partialUnify(partialUnify, tmpl.params[i].type, actual[i]);
                }
            }
            const auto& callee = generics.resolve(expr, actual,
                [&](Type iface, Type target, Span at) { return interfaces.ensureInterfaceImpl(iface, target, at); });
            checkCallArity(expr, callee.params.size(), callee.span);
            std::vector<std::string> mutPaths, allPaths;
            std::vector<Span> mutSpans, allSpans;
            for (std::size_t i = 0; i < actual.size(); ++i) {
                if (callee.params[i].byRef) {
                    if (!expr.args[i].isMutArg) {
                        Diagnostic d("E_MUT_REQUIRED", "Falta mut delante del argumento: " + callee.params[i].name, expr.args[i].span);
                        d.related.push_back(callee.params[i].span);
                        d.fix = Edit{{expr.args[i].span.start, expr.args[i].span.start}, "mut "};
                        throw d;
                    }
                    auto [v, path] = resolveMutPath(expr.args[i], "swap(mut a, mut b)", callee.params[i].span, true);
                    (void)v;
                    if (auto idx = overlapsIndex(mutPaths, path))
                        failAliasMut(path, mutPaths[*idx], expr.args[i].span, mutSpans[*idx]);
                    if (auto idx = overlapsIndex(allPaths, path))
                        failAliasMut(path, allPaths[*idx], expr.args[i].span, allSpans[*idx]);
                    mutPaths.push_back(path);
                    mutSpans.push_back(expr.args[i].span);
                    allPaths.push_back(std::move(path));
                    allSpans.push_back(expr.args[i].span);
                } else {
                    if (expr.args[i].isMutArg) fail("E_MUT_ARG", "El parámetro no admite mut", expr.args[i].span, "id(x)");
                    coerceIfDyn(expr.args[i], actual[i], callee.params[i].type, callee.params[i].span);
                    if (!isTrivialCopyType(types.program, actual[i])) {
                        if (auto lp = lvaluePath(expr.args[i])) {
                            if (auto idx = overlapsIndex(mutPaths, lp->second))
                                failAliasMut(lp->second, mutPaths[*idx], expr.args[i].span, mutSpans[*idx]);
                            allPaths.push_back(std::move(lp->second));
                            allSpans.push_back(expr.args[i].span);
                        }
                    }
                }
                requireType(actual[i], callee.params[i].type, expr.args[i].span, callee.params[i].span);
            }
            types.ensureInstantiated(callee.result, expr.span);
            interfaces.checkDynInType(callee.result, current.module, expr.span, false);
            return callee.result;
        }
        const auto it = functions.find(reference);
        if (it == functions.end()) {
            if (sourceName.find('.') == std::string::npos) {
                const Type builtinRes = inferBuiltinCall(expr, expected);
                if (builtinRes != Type::Unknown) return builtinRes;
            }
            if (sourceName.find('.') == std::string::npos && !current.methodConstraints.empty())
                return inferInterface(expr);
            if (const auto* cd = types.program.constant(reference)) {
                Diagnostic d("E_CALL_TYPE", "No se puede llamar una constante de tipo " + types.program.nameOf(cd->type), expr.span);
                d.actual = types.program.nameOf(cd->type);
                d.expected = "fn(...)";
                throw d;
            }
            fail("E_FUNCTION", "Función desconocida: " + sourceName, expr.span, "fn one() -> int { return 1 }");
        }
        visible(reference, expr.span);
        if (!expr.typeArgs.empty())
            fail("E_GENERIC", "La función " + sourceName + " no acepta argumentos de tipo <...>",
                 expr.span, sourceName + "(...)");
        expr.text = reference;
        const Function& callee = *it->second;
        if (callee.safety == SafetyTier::Raw)
            requireUnsafe(expr.span, "Llamar a '" + callee.sourceName + "' (raw / extern \"C\")");
        checkCallArguments(expr, callee.params);
        types.ensureInstantiated(callee.result, expr.span);
        interfaces.checkDynInType(callee.result, current.module, expr.span, false);
        return callee.result;
    }

    Type inferAssocCall(Expr& expr, Type expected) {
        (void)expected;
        if (!expr.typeArgs.empty())
            fail("E_GENERIC", "El método asociado " + expr.text + " no acepta argumentos de tipo <...>",
                 expr.span, expr.text + "(...)");
        const std::string rawTarget = types.program.nameOf(expr.assocType);
        if (types.genericDecls.contains(rawTarget))
            fail("E_GENERIC", "El tipo genérico " + rawTarget + " requiere argumentos entre <...>",
                 expr.span, rawTarget + "<int>." + expr.text + "()");
        known(expr.assocType, expr.span, false);
        auto fnOpt = interfaces.ensureInherentMethod(expr.assocType, expr.text, expr.span);
        if (!fnOpt) {
            const Function* matched = nullptr;
            Type matchedIface = Type::Unknown;
            for (const auto& [constrained, candidate] : current.methodConstraints) {
                if (constrained != expr.assocType) continue;
                const auto& decl = *interfaces.declarations.at(candidate);
                for (const auto& cm : decl.methods) {
                    if (cm.sourceName != expr.text) continue;
                    if (matched)
                        fail("E_OVERLOAD", "La llamada al método asociado de interfaz es ambigua: " + expr.text,
                             expr.span, expr.text);
                    matched = &cm;
                    matchedIface = candidate;
                }
            }
            if (matched) {
                checkCallArguments(expr, matched->params, true, expr.assocType);
                const Type result = interfaces.substituteSelf(matched->result, expr.assocType);
                if (mentionsOpaqueType(types.program, expr.assocType)) {
                    expr.kind = ExprKind::Call;
                    expr.text = "@abstract." + expr.text;
                    return result;
                }
                const auto key = std::make_pair(matchedIface, expr.assocType);
                if (!interfaces.ensureInterfaceImpl(matchedIface, expr.assocType, expr.span))
                    fail("E_IMPL_MISSING", "Falta impl " + types.program.nameOf(matchedIface) + " for " +
                         types.program.nameOf(expr.assocType), expr.span, "impl ...");
                expr.kind = ExprKind::Call;
                expr.text = interfaces.dispatch.at(key).at(matched->sourceName);
                types.ensureInstantiated(result, expr.span);
                interfaces.checkDynInType(result, current.module, expr.span, false);
                return result;
            }
            if (const Function* ifaceFn = interfaces.findInterfaceAssocMethod(expr.assocType, expr.text, current.module, expr.span)) {
                checkCallArguments(expr, ifaceFn->params);
                types.ensureInstantiated(ifaceFn->result, expr.span);
                interfaces.checkDynInType(ifaceFn->result, current.module, expr.span, false);
                expr.kind = ExprKind::Call;
                expr.text = ifaceFn->name;
                return ifaceFn->result;
            }
            fail("E_METHOD_UNKNOWN", "El tipo " + types.program.nameOf(expr.assocType) +
                 " no tiene método asociado llamado " + expr.text, expr.span,
                 "impl " + types.program.nameOf(expr.assocType) + " { fn " + expr.text + "() { ... } }");
        }
        const Function& callee = *functions.at(*fnOpt);
        if (callee.safety == SafetyTier::Raw)
            requireUnsafe(expr.span, "Llamar al método raw '" + callee.sourceName + "'");
        checkCallArguments(expr, callee.params);
        types.ensureInstantiated(callee.result, expr.span);
        interfaces.checkDynInType(callee.result, current.module, expr.span, false);
        expr.kind = ExprKind::Call;
        expr.text = callee.name;
        return callee.result;
    }

    void checkReceiverAndArgs(Expr& expr, const std::vector<Param>& params, bool isInterfaceDispatch, Type selfTarget) {
        std::vector<std::string> mutPaths, allPaths;
        std::vector<Span> mutSpans, allSpans;
        if (params[0].byRef) {
            auto [v, path] = resolveMutPath(expr.args[0], "var x = ...\nx." + expr.text + "(...)", params[0].span, true);
            (void)v;
            expr.args[0].isMutArg = true;
            mutPaths.push_back(path);
            mutSpans.push_back(expr.args[0].span);
            allPaths.push_back(std::move(path));
            allSpans.push_back(expr.args[0].span);
        } else {
            if (!isTrivialCopyType(types.program, expr.args[0].type)) {
                if (auto lp = lvaluePath(expr.args[0])) {
                    allPaths.push_back(std::move(lp->second));
                    allSpans.push_back(expr.args[0].span);
                }
            }
        }
        checkCallArguments(expr, params, isInterfaceDispatch, selfTarget, 1, mutPaths, allPaths, mutSpans, allSpans);
    }

    Type inferMethodCall(Expr& expr, Type expected) {
        // 1. Disambiguate module-qualified calls (mod.fn(...)) when not shadowed by a local or constant.
        if (expr.args[0].kind == ExprKind::Name) {
            const std::string& modName = expr.args[0].text;
            const std::string localConst = current.module.empty() ? modName : current.module + "." + modName;
            if (!locals.find(modName) && !types.program.constant(localConst)) {
                const std::string qname = modName + "." + expr.text;
                auto isKnownModule = [&]() {
                    if (std::find(types.program.rootModules.begin(), types.program.rootModules.end(), modName) !=
                        types.program.rootModules.end())
                        return true;
                    for (const auto& m : types.program.modules) if (m.name == modName) return true;
                    return false;
                };
                if (generics.contains(qname) || functions.contains(qname) || isKnownModule()) {
                    expr.kind = ExprKind::Call;
                    expr.text = qname;
                    expr.args.erase(expr.args.begin());
                    return inferCall(expr, expected);
                }
            }
        }
        // 2. Evaluate receiver type.
        if (!expr.typeArgs.empty())
            fail("E_GENERIC", "El método " + expr.text + " no acepta argumentos de tipo <...>",
                 expr.span, expr.text + "(...)");
        const Type recvType = derefCell(expr.args[0], typeOf(expr.args[0], ArrayUse::Ordinary));
        requireValue(recvType, expr.args[0].span);

        // 3a. Dynamic dispatch receiver: dyn Interfaz
        if (types.program.isDyn(recvType)) {
            const Type iface = types.program.dynInterface(recvType);
            interfaces.checkDynSafety(iface, expr.span);
            const InterfaceDecl& ifaceDecl = *interfaces.declarations.at(iface);
            for (std::size_t mi = 0; mi < ifaceDecl.methods.size(); ++mi) {
                if (ifaceDecl.methods[mi].sourceName != expr.text) continue;
                const Function& m = ifaceDecl.methods[mi];
                checkReceiverAndArgs(expr, m.params, false, Type::Unknown);
                types.ensureInstantiated(m.result, expr.span);
                expr.kind = ExprKind::DynCall;
                expr.assocType = iface;
                expr.fieldIndices = {mi};
                return m.result;
            }
            fail("E_METHOD_UNKNOWN", "La interfaz dyn " + types.program.nameOf(iface) +
                 " no tiene el método " + expr.text, expr.span,
                 ifaceDecl.methods.empty() ? "dyn " + types.program.nameOf(iface)
                                           : "x." + ifaceDecl.methods.front().sourceName + "(...)");
        }

        // 3b. Constrained generic receiver / Self inside default method
        {
            const Function* matched = nullptr;
            Type matchedIface = Type::Unknown;
            for (const auto& [constrained, candidate] : current.methodConstraints) {
                if (constrained != recvType) continue;
                const auto& decl = *interfaces.declarations.at(candidate);
                for (const auto& cm : decl.methods) {
                    if (cm.sourceName != expr.text || cm.params.empty() || !isSelf(types.program, cm.params[0].type))
                        continue;
                    if (matched)
                        fail("E_OVERLOAD", "La llamada al método de interfaz es ambigua: " + expr.text,
                             expr.span, expr.text);
                    matched = &cm;
                    matchedIface = candidate;
                }
            }
            if (matched) {
                checkReceiverAndArgs(expr, matched->params, true, recvType);
                const Type result = interfaces.substituteSelf(matched->result, recvType);
                if (mentionsOpaqueType(types.program, recvType)) {
                    expr.kind = ExprKind::Call;
                    expr.text = "@abstract." + expr.text;
                    return result;
                }
                const auto key = std::make_pair(matchedIface, recvType);
                if (!interfaces.ensureInterfaceImpl(matchedIface, recvType, expr.span))
                    fail("E_IMPL_MISSING", "Falta impl " + types.program.nameOf(matchedIface) + " for " +
                         types.program.nameOf(recvType), expr.span, "impl ...");
                expr.kind = ExprKind::Call;
                expr.text = interfaces.dispatch.at(key).at(matched->sourceName);
                types.ensureInstantiated(result, expr.span);
                interfaces.checkDynInType(result, current.module, expr.span, false);
                return result;
            }
            if (isOpaqueType(types.program, recvType))
                fail("E_METHOD_UNKNOWN", "El tipo genérico no declara el método " + expr.text,
                     expr.span, "fn f<T: Interfaz>(x: T) { ... }");
        }

        // 3c. Concrete receiver: inherent method first, then visible interface impls.
        if (auto fnOpt = interfaces.ensureInherentMethod(recvType, expr.text, expr.span)) {
            const Function& callee = *functions.at(*fnOpt);
            if (callee.params.empty() || callee.params[0].type != recvType) {
                Diagnostic d("E_METHOD_STATIC", "El método " + expr.text + " es asociado a " +
                             types.program.nameOf(recvType) + ", no recibe self", expr.span);
                d.related.push_back(callee.span);
                d.example = types.program.nameOf(recvType) + "." + expr.text + "(...)";
                throw d;
            }
            if (callee.safety == SafetyTier::Raw)
                requireUnsafe(expr.span, "Llamar al método raw '" + callee.sourceName + "'");
            checkReceiverAndArgs(expr, callee.params, false, Type::Unknown);
            types.ensureInstantiated(callee.result, expr.span);
            interfaces.checkDynInType(callee.result, current.module, expr.span, false);
            expr.kind = ExprKind::Call;
            expr.text = callee.name;
            return callee.result;
        }
        if (const Function* ifaceFn = interfaces.findInterfaceMethod(recvType, expr.text, current.module, expr.span)) {
            if (ifaceFn->safety == SafetyTier::Raw)
                requireUnsafe(expr.span, "Llamar al método raw '" + ifaceFn->sourceName + "'");
            checkReceiverAndArgs(expr, ifaceFn->params, false, Type::Unknown);
            types.ensureInstantiated(ifaceFn->result, expr.span);
            interfaces.checkDynInType(ifaceFn->result, current.module, expr.span, false);
            expr.kind = ExprKind::Call;
            expr.text = ifaceFn->name;
            return ifaceFn->result;
        }
        if (types.hasDecl(recvType) && !types.decl(recvType).isEnum) {
            const auto& fields = types.decl(recvType).variants.at(0).fields;
            for (std::size_t fi = 0; fi < fields.size(); ++fi) {
                if (fields[fi].name == expr.text && types.program.isFn(fields[fi].type)) {
                    if (expr.args[0].isMutArg)
                        fail("E_MUT_ARG", "El receptor de una closure en campo no admite mut", expr.args[0].span, "s." + expr.text + "(...)");
                    Expr fExpr{ExprKind::Field, expr.args[0].span, expr.text, {std::move(expr.args[0])}};
                    fExpr.type = fields[fi].type;
                    fExpr.fieldIndices = {fi};
                    expr.args[0] = std::move(fExpr);
                    expr.kind = ExprKind::ClosureCall;
                    expr.text.clear();
                    return inferClosureCall(expr);
                }
            }
        }

        // 3d. Uniform Function Call Syntax (UFCS) on built-in types and scalars.
        {
            const std::string& mName = expr.text;
            std::string ufcsTarget;
            bool ufcsMut = false;
            bool ufcsIsEmpty = false;
            if (types.program.isArray(recvType)) {
                if (mName == "len" || mName == "cap") ufcsTarget = mName;
                else if (mName == "isEmpty") { ufcsTarget = "len"; ufcsIsEmpty = true; }
                else if (mName == "push" || mName == "pop" || mName == "clear" || mName == "truncate" ||
                         mName == "reserve" || mName == "sort" || mName == "sortStable") {
                    ufcsTarget = mName; ufcsMut = true;
                }
            } else if (recvType == Type::Str) {
                if (mName == "len") ufcsTarget = "len";
                else if (mName == "isEmpty") { ufcsTarget = "len"; ufcsIsEmpty = true; }
                else if (mName == "contains") ufcsTarget = "strContains";
                else if (mName == "startsWith") ufcsTarget = "strStartsWith";
                else if (mName == "endsWith") ufcsTarget = "strEndsWith";
                else if (mName == "indexOf") ufcsTarget = "strIndexOf";
            } else if (types.program.isMap(recvType)) {
                if (mName == "len") ufcsTarget = "len";
                else if (mName == "isEmpty") { ufcsTarget = "len"; ufcsIsEmpty = true; }
                else if (mName == "get") ufcsTarget = "mapGet";
                else if (mName == "has") ufcsTarget = "mapHas";
                else if (mName == "keys") ufcsTarget = "mapKeys";
                else if (mName == "values") ufcsTarget = "mapValues";
                else if (mName == "set") { ufcsTarget = "mapSet"; ufcsMut = true; }
                else if (mName == "del") { ufcsTarget = "mapDel"; ufcsMut = true; }
                else if (mName == "clear" || mName == "reserve") { ufcsTarget = mName; ufcsMut = true; }
            } else if (types.program.isSet(recvType)) {
                if (mName == "len") ufcsTarget = "len";
                else if (mName == "isEmpty") { ufcsTarget = "len"; ufcsIsEmpty = true; }
                else if (mName == "has") ufcsTarget = "setHas";
                else if (mName == "items") ufcsTarget = "setItems";
                else if (mName == "add") { ufcsTarget = "setAdd"; ufcsMut = true; }
                else if (mName == "del") { ufcsTarget = "setDel"; ufcsMut = true; }
                else if (mName == "clear" || mName == "reserve") { ufcsTarget = mName; ufcsMut = true; }
            } else if (types.program.isPool(recvType)) {
                if (mName == "len") ufcsTarget = "len";
                else if (mName == "isEmpty") { ufcsTarget = "len"; ufcsIsEmpty = true; }
                else if (mName == "get") ufcsTarget = "poolGet";
                else if (mName == "has") ufcsTarget = "poolHas";
                else if (mName == "handles") ufcsTarget = "poolHandles";
                else if (mName == "add") { ufcsTarget = "poolAdd"; ufcsMut = true; }
                else if (mName == "set") { ufcsTarget = "poolSet"; ufcsMut = true; }
                else if (mName == "remove") { ufcsTarget = "poolRemove"; ufcsMut = true; }
                else if (mName == "clear" || mName == "reserve") { ufcsTarget = mName; ufcsMut = true; }
            } else if (recvType == Type::StringBuilder) {
                if (mName == "len" || mName == "cap") ufcsTarget = mName;
                else if (mName == "isEmpty") { ufcsTarget = "len"; ufcsIsEmpty = true; }
                else if (mName == "str") ufcsTarget = "sbStr";
                else if (mName == "push") { ufcsTarget = "sbPush"; ufcsMut = true; }
                else if (mName == "pushByte") { ufcsTarget = "sbPushByte"; ufcsMut = true; }
                else if (mName == "pushInt") { ufcsTarget = "sbPushInt"; ufcsMut = true; }
                else if (mName == "pushFloat") { ufcsTarget = "sbPushFloat"; ufcsMut = true; }
                else if (mName == "clear") { ufcsTarget = "sbClear"; ufcsMut = true; }
                else if (mName == "take") { ufcsTarget = "sbTake"; ufcsMut = true; }
            } else if (types.program.isOption(recvType)) {
                if (mName == "isSome" || mName == "isNone" || mName == "unwrapOr") ufcsTarget = mName;
            } else if (types.program.isResult(recvType)) {
                if (mName == "isOk" || mName == "isErr" || mName == "unwrapOr") ufcsTarget = mName;
            } else if (Program::isNumericType(recvType)) {
                if (mName == "abs" || mName == "min" || mName == "max") ufcsTarget = mName;
                else if (Program::isFloatType(recvType) &&
                         (mName == "sqrt" || mName == "floor" || mName == "ceil" ||
                          mName == "round" || mName == "pow" || mName == "isNaN" || mName == "isInf"))
                    ufcsTarget = mName;
            }
            if (!ufcsTarget.empty()) {
                if (expr.args[0].isMutArg)
                    fail("E_MUT_ARG", "No antepongas mut al receptor de una llamada con punto.", expr.args[0].span, "x." + mName + "(...)");
                struct PreTypedGuard {
                    Expr*& slot; Expr* prev;
                    PreTypedGuard(Expr*& s, Expr* v) : slot(s), prev(s) { slot = v; }
                    ~PreTypedGuard() { slot = prev; }
                };
                if (ufcsIsEmpty) {
                    checkCallArity(expr, 1);
                    Expr lenCall{ExprKind::Call, expr.span, ufcsTarget, {std::move(expr.args[0])}};
                    PreTypedGuard ptg(preTypedArg0, &lenCall.args[0]);
                    lenCall.type = inferCall(lenCall, Type::Int);
                    Expr zero{ExprKind::Integer, expr.span, "0", {}, Type::Int};
                    expr = Expr{ExprKind::Binary, expr.span, "==", {std::move(lenCall), std::move(zero)}, Type::Bool};
                    return Type::Bool;
                }
                expr.kind = ExprKind::Call;
                expr.text = std::move(ufcsTarget);
                if (ufcsMut) expr.args[0].isMutArg = true;
                PreTypedGuard ptg(preTypedArg0, &expr.args[0]);
                return inferCall(expr, expected);
            }
        }

        // 3e. UFCS general: `x.f(args)` es `f(x, args)` cuando existe una función libre `f`, propia o
        // de la biblioteca base. Antes se traducen los nombres que el modelo trae de otros lenguajes.
        if (!strictSyntax()) {
            const bool isStr = recvType == Type::Str;
            const bool isArr = types.program.isArray(recvType);
            const Type elemT = isArr ? types.program.elemOf(recvType) : Type::Unknown;
            std::string target = expr.text;
            bool wrapContains = false;
            if (isStr || isArr) {
                if (target == "length" || target == "size") target = "len";
                else if (target == "toUpperCase" || target == "toUpper" || target == "to_uppercase" || target == "toUppercase") target = "upper";
                else if (target == "toLowerCase" || target == "toLower" || target == "to_lowercase" || target == "toLowercase") target = "lower";
                else if (target == "strip") target = "trim";
            }
            if (isStr) {
                if (target == "repeat") target = "strRepeat";
                else if (target == "reverse") target = "strReverse";
                else if (target == "count") target = "countOf";
            } else if (isArr) {
                if (target == "append" || target == "add") target = "push";
                else if (target == "reverse") target = "reversed";
                else if (target == "max") target = "arrayMax";
                else if (target == "min") target = "arrayMin";
                else if (target == "count") target = "countIf";
                else if (target == "sum" && Program::isFloatType(elemT)) target = "sumFloat";
                else if (target == "indexOf" || target == "contains" || target == "includes") {
                    wrapContains = target != "indexOf";
                    target = elemT == Type::Str ? "indexOfStr" : "indexOfInt";
                }
            }
            const std::string qualified = canonicalUnqualified(current.module, target);
            auto fit = functions.find(qualified);
            if (fit == functions.end()) fit = functions.find(target);
            const bool generic = generics.contains(qualified) || generics.contains(target);
            const bool builtin = target == "len" || target == "push";
            if (builtin || generic || (fit != functions.end() && !fit->second->params.empty() && !fit->second->isImpl)) {
                // El receptor ya tiene tipo: no se vuelve a inferir al tratarlo como primer argumento.
                struct ReceiverTyped {
                    Expr*& slot; Expr* prev;
                    ReceiverTyped(Expr*& s, Expr* v) : slot(s), prev(s) { slot = v; }
                    ~ReceiverTyped() { slot = prev; }
                };
                const bool byRef = target == "push" ||
                    (fit != functions.end() && !fit->second->params.empty() && fit->second->params[0].byRef);
                expr.kind = ExprKind::Call;
                expr.text = target;
                if (byRef) expr.args[0].isMutArg = true;
                if (wrapContains) {
                    // a.contains(x)  ->  indexOf(a, x) >= 0
                    Expr call = std::move(expr);
                    const Span span = call.span;
                    Expr test{ExprKind::Binary, span, ">=", {}};
                    test.args.push_back(std::move(call));
                    test.args.push_back(Expr{ExprKind::Integer, span, "0", {}});
                    expr = std::move(test);
                    ReceiverTyped receiverTyped(preTypedArg0, &expr.args[0].args[0]);
                    return typeOf(expr, ArrayUse::Ordinary, expected);
                }
                ReceiverTyped receiverTyped(preTypedArg0, &expr.args[0]);
                return inferCall(expr, expected);
            }
        }

        // Helpful false-friend diagnostics on built-in collections and str
        Diagnostic d("E_METHOD_UNKNOWN", "El tipo " + types.program.nameOf(recvType) +
                     " no tiene el método " + expr.text, expr.span);
        const auto src = types.program.sourceOf(expr.span);
        std::string baseText;
        if (expr.args[0].span.end <= src.size())
            baseText = std::string(src.substr(expr.args[0].span.start, expr.args[0].span.end - expr.args[0].span.start));
        if ((recvType == Type::Str || types.program.isArray(recvType) || recvType == Type::StringBuilder ||
             types.program.isMap(recvType) || types.program.isSet(recvType) || types.program.isPool(recvType)) &&
            (expr.text == "len" || expr.text == "length" || expr.text == "size") && expr.args.size() == 1) {
            d.message = "En Turi la longitud se obtiene con la función len(x).";
            if (!baseText.empty() && expr.span.end <= src.size()) d.fix = Edit{expr.span, "len(" + baseText + ")"};
            d.example = "len(x)";
            throw d;
        }
        if (types.program.isArray(recvType)) {
            if (expr.text == "push" || expr.text == "append") {
                d.message = "Para añadir a un array en Turi usa push(mut a, x).";
                d.example = "push(mut " + (baseText.empty() ? std::string("a") : baseText) + ", x)";
                throw d;
            }
            if (expr.text == "pop") {
                d.message = "Para extraer el último elemento de un array usa pop(mut a).";
                if (!baseText.empty() && expr.args.size() == 1) d.fix = Edit{expr.span, "pop(mut " + baseText + ")"};
                d.example = "pop(mut a)";
                throw d;
            }
        }
        if (types.program.isMap(recvType)) {
            if (expr.text == "get") {
                d.message = "Usa mapGet(mapa, clave) para leer Map<K, V>.";
                d.example = "mapGet(m, clave)";
                throw d;
            }
            if (expr.text == "set" || expr.text == "insert" || expr.text == "put") {
                d.message = "Usa mapSet(mut mapa, clave, valor) para escribir en Map<K, V>.";
                d.example = "mapSet(mut m, clave, valor)";
                throw d;
            }
            if (expr.text == "has" || expr.text == "contains" || expr.text == "containsKey") {
                d.message = "Usa mapHas(mapa, clave) para comprobar claves en Map<K, V>.";
                d.example = "mapHas(m, clave)";
                throw d;
            }
        }
        if (types.program.isSet(recvType)) {
            if (expr.text == "add" || expr.text == "insert") {
                d.message = "Usa setAdd(mut conj, elem) para añadir elementos a Set<T>.";
                d.example = "setAdd(mut s, elem)";
                throw d;
            }
            if (expr.text == "has" || expr.text == "contains") {
                d.message = "Usa setHas(conj, elem) para comprobar pertenencia en Set<T>.";
                d.example = "setHas(s, elem)";
                throw d;
            }
        }
        d.example = "impl MiTipo { fn " + expr.text + "(self) { ... } }";
        throw d;
    }

    // Literal del valor cero de un tipo escalar o str; vacío para los demás tipos.
    static std::optional<Expr> zeroLiteral(Type t, Span span) {
        if (Program::isFloatType(t)) return Expr{ExprKind::Float, span, "0.0", {}};
        if (Program::isNumericType(t)) return Expr{ExprKind::Integer, span, "0", {}};
        if (t == Type::Bool) return Expr{ExprKind::Boolean, span, "false", {}};
        if (t == Type::Str) return Expr{ExprKind::String, span, "", {}};
        return {};
    }

    static bool isLiteralHintCandidate(const Expr& e) {
        return e.kind == ExprKind::Integer || e.kind == ExprKind::Float ||
               (e.kind == ExprKind::Unary && e.text == "-" && !e.args.empty() &&
                (e.args[0].kind == ExprKind::Integer || e.args[0].kind == ExprKind::Float));
    }

    Type probeConcreteNumericType(const Expr& e) const {
        if (isLiteralHintCandidate(e)) return Type::Unknown;
        if (e.kind == ExprKind::Binary && e.args.size() == 2) {
            if (e.text == "==" || e.text == "!=" || e.text == "<" || e.text == "<=" ||
                e.text == ">" || e.text == ">=" || e.text == "&&" || e.text == "||")
                return Type::Bool;
            if (const Type lt = probeConcreteNumericType(e.args[0]); lt != Type::Unknown) return lt;
            return probeConcreteNumericType(e.args[1]);
        }
        if (e.kind == ExprKind::Unary && e.text != "!" && !e.args.empty()) {
            return probeConcreteNumericType(e.args[0]);
        }
        const Type t = probeExprType(e, {});
        return Program::isNumericType(t) ? t : Type::Unknown;
    }

    Type inferBinary(Expr& expr, Type expected = Type::Unknown) {
        Expr& left = expr.args[0];
        Expr& right = expr.args[1];
        const bool isRelOrEq = (expr.text == "==" || expr.text == "!=" ||
                                expr.text == "<" || expr.text == "<=" ||
                                expr.text == ">" || expr.text == ">=");
        Type expLeft = Type::Unknown;
        if (!isRelOrEq && (Program::isNumericType(expected) || types.program.isSimd(expected))) {
            expLeft = expected;
        } else if (expr.text != "&&" && expr.text != "||" && isLiteralHintCandidate(left)) {
            const Type rt = probeConcreteNumericType(right);
            if (Program::isNumericType(rt)) expLeft = rt;
        }
        if ((isRelOrEq || expr.text == "+") && isCharLiteral(left) && !isCharLiteral(right) &&
            probeExprType(right, {}) == Type::Str)
            charToStr(left);
        const bool isArith = expr.text == "-" || expr.text == "*" || expr.text == "/" || expr.text == "%";
        if (isArith && !strictSyntax() && isCharLiteral(left) && !isCharLiteral(right) &&
            probeExprType(right, {}) == Type::Str)
            right = charCodeOf(std::move(right));
        auto leftType = typeOf(left, ArrayUse::Ordinary, expLeft);
        if ((isRelOrEq || expr.text == "+") && leftType == Type::Str && isCharLiteral(right)) charToStr(right);
        if (isArith && !strictSyntax() && leftType == Type::Str && isCharLiteral(right)) {
            // `c - 'a'` con `c` un str de un carácter: aritmética sobre el código del carácter.
            left = charCodeOf(std::move(left));
            leftType = typeOf(left, ArrayUse::Ordinary, expLeft);
        }
        if (expr.text == "==" || expr.text == "!=") {
            requireValue(leftType, left.span);
            if ((leftType >= Type::NamedBase && !types.program.isArray(leftType) &&
                 !types.program.isHandle(leftType) && !types.program.isAnyPtr(leftType) &&
                 !types.program.isSimd(leftType) && !isUnitEnum(leftType)) ||
                types.program.isArray(leftType) || leftType == Type::StringBuilder ||
                Program::isPrimitiveResource(leftType))
                fail("E_EQUALITY", "El agregado se compara por sus campos escalares", left.span, "a.x == b.x");
            requireType(typeOf(right, ArrayUse::Ordinary, leftType), leftType, right.span, left.span);
            return Type::Bool;
        }
        if (expr.text == "&&" || expr.text == "||") {
            requireType(leftType, Type::Bool, left.span);
            auto rhsSnap = saveMoveState();
            requireType(typeOf(right), Type::Bool, right.span, left.span);
            auto afterRhs = saveMoveState();
            mergeMoveState(rhsSnap, afterRhs);
            restoreMoveState(rhsSnap);
            return Type::Bool;
        }
        const bool isIntSimd = types.program.isSimd(leftType) && Program::isIntType(types.program.simdElem(leftType));
        if (expr.text == "&" || expr.text == "|" || expr.text == "^" ||
            expr.text == "<<" || expr.text == ">>" || expr.text == ">>>" || expr.text == "%") {
            const bool ok = Program::isIntType(leftType) || (expr.text != "%" && isIntSimd);
            if (!ok)
                fail("E_TYPE", "El operador " + expr.text + " requiere un tipo entero", left.span, "a & b");
            requireType(typeOf(right, ArrayUse::Ordinary, leftType), leftType, right.span, left.span);
            return leftType;
        }
        if (expr.text == "<" || expr.text == "<=" || expr.text == ">" || expr.text == ">=") {
            if (!Program::isNumericType(leftType) && leftType != Type::Str)
                fail("E_TYPE", "La comparación de orden requiere tipo numérico o str", left.span, "a < b");
            requireType(typeOf(right, ArrayUse::Ordinary, leftType), leftType, right.span, left.span);
            return Type::Bool;
        }
        // +, -, *, /
        if (expr.text == "+" && (right.kind == ExprKind::String || right.kind == ExprKind::Interp) &&
            (Program::isNumericType(leftType) || leftType == Type::Bool)) {
            // `n + " elementos"`: el escalar se convierte con str(), como en JS, Java o Kotlin.
            Expr inner = std::move(left);
            Expr call{ExprKind::Call, inner.span, "str", {}};
            call.args.push_back(std::move(inner));
            left = std::move(call);
            (void)typeOf(left);
            (void)typeOf(right);
            return Type::Str;
        }
        if (expr.text == "+" && leftType == Type::Str) {
            const Type rightType = typeOf(right);
            if (Program::isNumericType(rightType) || rightType == Type::Bool) {
                // `"n=" + n`: el escalar se convierte con str(), como en JS, Java o Kotlin.
                Expr inner = std::move(right);
                Expr call{ExprKind::Call, inner.span, "str", {}};
                call.args.push_back(std::move(inner));
                right = std::move(call);
                (void)typeOf(right);
                return Type::Str;
            }
            if (rightType != Type::Str) {
                Diagnostic d("E_TYPE", "No se puede concatenar str con " + types.program.nameOf(rightType) + "; usa interpolación o str(x)", right.span);
                d.expected = "str";
                d.actual = types.program.nameOf(rightType);
                d.example = R"("valor = {x}")";
                throw d;
            }
            return Type::Str;
        }
        if (types.program.isSimd(leftType)) {
            if (expr.text == "/" && !Program::isFloatType(types.program.simdElem(leftType)))
                fail("E_TYPE", "La división / en Simd solo admite carriles flotantes (f32 o float)", left.span, "a / b");
            requireType(typeOf(right, ArrayUse::Ordinary, leftType), leftType, right.span, left.span);
            return leftType;
        }
        if (!Program::isNumericType(leftType))
            fail("E_TYPE", "El operador " + expr.text + " requiere un tipo numérico", left.span, "a + b");
        requireType(typeOf(right, ArrayUse::Ordinary, leftType), leftType, right.span, left.span);
        return leftType;
    }

    Type construct(Expr& expr, Type expected = Type::Unknown) {
        if (expr.text == "None") {
            checkCallArity(expr, 0);
            if (expected == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir el tipo de T? para None; anota el tipo esperado",
                     expr.span, "let x: int? = None");
            if (!types.program.isOption(expected)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para None", expr.span);
                d.expected = types.program.nameOf(expected);
                d.actual = "T?";
                d.example = "let x: int? = None";
                throw d;
            }
            types.ensureInstantiated(expected, expr.span);
            expr.text = types.program.nameOf(expected) + ".None";
            expr.fieldIndices.clear();
            return expected;
        }
        if (expr.text == "Some") {
            checkCallArity(expr, 1);
            if (expr.args[0].isMutArg) fail("E_MUT_ARG", "Some no admite mut", expr.args[0].span, "Some(1)");
            const Type innerExp = (expected != Type::Unknown && types.program.isOption(expected))
                ? types.program.optionInner(expected) : Type::Unknown;
            Type argT = typeOf(expr.args[0], ArrayUse::Copy, innerExp);
            coerceIfDyn(expr.args[0], argT, innerExp, expr.span);
            requireValue(argT, expr.args[0].span);
            const Type optT = types.program.optionOf(argT);
            types.ensureInstantiated(optT, expr.span);
            expr.text = types.program.nameOf(optT) + ".Some";
            expr.fieldIndices = {0};
            return optT;
        }
        if (expr.text == "Ok" || expr.text == "Err") {
            checkCallArity(expr, 1);
            if (expr.args[0].isMutArg) fail("E_MUT_ARG", expr.text + " no admite mut", expr.args[0].span, expr.text + "(1)");
            if (expected == Type::Unknown)
                fail("E_INFER_TYPE", "No se puede inferir el tipo completo de Result<T, E> para " + expr.text,
                     expr.span, "let r: Result<int, str> = " + expr.text + "(...)");
            if (!types.program.isResult(expected)) {
                Diagnostic d("E_TYPE", "Tipo incompatible para " + expr.text, expr.span);
                d.expected = types.program.nameOf(expected);
                d.actual = "Result<T, E>";
                d.example = "let r: Result<int, str> = " + expr.text + "(...)";
                throw d;
            }
            types.ensureInstantiated(expected, expr.span);
            const Type payloadExp = expr.text == "Ok"
                ? types.program.resultOk(expected) : types.program.resultErr(expected);
            Type payloadAct = typeOf(expr.args[0], ArrayUse::Copy, payloadExp);
            coerceIfDyn(expr.args[0], payloadAct, payloadExp, expr.span);
            requireType(payloadAct, payloadExp, expr.args[0].span);
            expr.text = types.program.nameOf(expected) + "." + expr.text;
            expr.fieldIndices = {0};
            return expected;
        }

        bool needsVisibleCheck = true;
        if (expr.text.starts_with("@Option.") || expr.text.starts_with("@Result.")) {
            needsVisibleCheck = false;
            const bool isOpt = expr.text.starts_with("@Option.");
            const std::string base = isOpt ? "@Option" : "@Result";
            const std::string varName = expr.text.substr(base.size() + 1);
            const std::size_t reqArity = isOpt ? 1 : 2;
            Type appT = Type::Unknown;
            if (expr.typeArgs.empty()) {
                if (expected == Type::Unknown ||
                    (isOpt ? !types.program.isOption(expected) : !types.program.isResult(expected)))
                    fail("E_INFER_TYPE", "Faltan argumentos de tipo en " + expr.text.substr(1),
                         expr.span, isOpt ? "Option<int>.Some(1)" : "Result<int, str>.Ok(1)");
                appT = expected;
            } else {
                if (expr.typeArgs.size() != reqArity)
                    fail("E_GENERIC", "Cantidad incorrecta de argumentos de tipo para " + base.substr(1),
                         expr.span, isOpt ? "Option<int>.None" : "Result<int, str>.Ok(1)");
                for (Type ta : expr.typeArgs) known(ta, expr.span, false);
                appT = types.program.typeApp(base, expr.typeArgs);
            }
            types.ensureInstantiated(appT, expr.span);
            expr.text = types.program.nameOf(appT) + "." + varName;
        } else {
            const auto dotPos = expr.isEnumConstruct ? expr.text.rfind('.') : std::string::npos;
            const std::string baseName = dotPos == std::string::npos ? expr.text : expr.text.substr(0, dotPos);
            const std::string varName = dotPos == std::string::npos ? "" : expr.text.substr(dotPos + 1);
            if (!expr.typeArgs.empty()) {
                visible(baseName, expr.span);
                auto tmplIt = types.genericDecls.find(baseName);
                if (tmplIt == types.genericDecls.end()) {
                    if (types.constructors.contains(expr.text))
                        fail("E_GENERIC", "El tipo no admite argumentos genéricos: " + baseName, expr.span, expr.text);
                    fail("E_CONSTRUCTOR", "Constructor desconocido: " + expr.text, expr.span, "struct Point { x: int }");
                }
                const DataDecl& tmpl = tmplIt->second;
                if (tmpl.isEnum != expr.isEnumConstruct)
                    fail("E_CONSTRUCTOR", "Forma de constructor incorrecta para " + baseName, expr.span, expr.text);
                if (expr.typeArgs.size() != tmpl.generics.size())
                    fail("E_GENERIC", "Cantidad incorrecta de argumentos de tipo para " + baseName,
                         expr.span, baseName + "<int>", tmpl.span);
                for (Type ta : expr.typeArgs) known(ta, expr.span, false);
                const Type appT = types.program.typeApp(baseName, expr.typeArgs);
                types.ensureInstantiated(appT, expr.span);
                expr.text = expr.isEnumConstruct
                    ? (types.program.nameOf(appT) + "." + varName)
                    : types.program.nameOf(appT);
                needsVisibleCheck = false;
            } else if (auto tmplIt = types.genericDecls.find(baseName); tmplIt != types.genericDecls.end()) {
                visible(baseName, expr.span);
                const DataDecl& tmpl = tmplIt->second;
                if (tmpl.isEnum != expr.isEnumConstruct)
                    fail("E_CONSTRUCTOR", "Constructor desconocido: " + expr.text, expr.span, "struct Point { x: int }");
                const Variant* tmplVar = nullptr;
                for (const auto& candV : tmpl.variants) {
                    if (!tmpl.isEnum || candV.name == varName) { tmplVar = &candV; break; }
                }
                if (!tmplVar) fail("E_CONSTRUCTOR", "Constructor desconocido: " + expr.text, expr.span, "enum State { Ready }");
                checkCallArity(expr, tmplVar->fields.size(), tmplVar->span);
                std::unordered_set<std::size_t> used;
                expr.fieldIndices.clear();
                for (std::size_t i = 0; i < expr.args.size(); ++i) {
                    const auto idx = tmpl.isEnum ? i : fieldIndex(*tmplVar, expr.labels.at(i), expr.args[i].span);
                    if (!used.insert(idx).second)
                        fail("E_DUP_FIELD", "El constructor repite el campo " + tmplVar->fields[idx].name, expr.args[i].span,
                             "Point { x: 1 }", tmplVar->fields[idx].span);
                    expr.fieldIndices.push_back(idx);
                }
                std::map<Type, Type> deduced;
                for (Type g : tmpl.generics) deduced.emplace(g, Type::Unknown);
                if (expected != Type::Unknown) {
                    if (const auto* expApp = types.program.typeAppOf(expected)) {
                        if (expApp->baseName == baseName && expApp->args.size() == tmpl.generics.size()) {
                            for (std::size_t k = 0; k < tmpl.generics.size(); ++k)
                                deduced[tmpl.generics[k]] = expApp->args[k];
                        }
                    }
                }
                auto hasUnresolved = [&](auto& self, Type t) -> bool {
                    if (auto it = deduced.find(t); it != deduced.end()) return it->second == Type::Unknown;
                    if (types.program.isArray(t)) return self(self, types.program.elemOf(t));
                    if (const auto* app = types.program.typeAppOf(t)) {
                        for (Type a : app->args) if (self(self, a)) return true;
                    }
                    return false;
                };
                auto unifyTmpl = [&](auto& self, Type pt, Type at) -> void {
                    if (auto it = deduced.find(pt); it != deduced.end()) {
                        if (it->second == Type::Unknown) it->second = at;
                        return;
                    }
                    if (types.program.isArray(pt) && types.program.isArray(at)) {
                        self(self, types.program.elemOf(pt), types.program.elemOf(at));
                        return;
                    }
                    const auto* pa = types.program.typeAppOf(pt);
                    const auto* aa = types.program.typeAppOf(at);
                    if (pa && aa && pa->baseName == aa->baseName && pa->args.size() == aa->args.size()) {
                        for (std::size_t k = 0; k < pa->args.size(); ++k) self(self, pa->args[k], aa->args[k]);
                    }
                };
                auto needsExpected = [](const Expr& ex) {
                    return (ex.kind == ExprKind::ArrayLit && ex.args.empty()) ||
                           (ex.kind == ExprKind::Construct && ex.typeArgs.empty() &&
                            (ex.text == "None" || ex.text == "Ok" || ex.text == "Err")) ||
                           (ex.kind == ExprKind::Call && ex.args.empty() &&
                            (ex.text == "mapNew" || ex.text == "mapSecure" ||
                             ex.text == "setNew" || ex.text == "setSecure" || ex.text == "poolNew")) ||
                           (ex.kind == ExprKind::Call && ex.args.size() == 1 &&
                            (ex.text == "mapWithCap" || ex.text == "setWithCap" || ex.text == "poolWithCap"));
                };
                std::vector<bool> checked(expr.args.size(), false);
                for (std::size_t i = 0; i < expr.args.size(); ++i) {
                    const std::size_t fIdx = expr.fieldIndices[i];
                    const Type ft = tmplVar->fields[fIdx].type;
                    if (hasUnresolved(hasUnresolved, ft) && needsExpected(expr.args[i])) continue;
                    Type expF = Type::Unknown;
                    if (!hasUnresolved(hasUnresolved, ft)) {
                        expF = substituteTypeMap(ft, deduced, types.program);
                        types.ensureInstantiated(expF, expr.args[i].span);
                    }
                    DestGuard dg(activeMoveDest, "el campo '" + tmplVar->fields[fIdx].name + "' de " + baseName);
                    const Type actF = typeOf(expr.args[i], ArrayUse::Copy, expF);
                    checked[i] = true;
                    unifyTmpl(unifyTmpl, ft, actF);
                }
                std::vector<Type> concreteArgs;
                concreteArgs.reserve(tmpl.generics.size());
                for (Type g : tmpl.generics) {
                    Type resolved = deduced.at(g);
                    if (resolved == Type::Unknown)
                        fail("E_INFER_TYPE", "No se pueden inferir los argumentos de tipo de " + baseName +
                             "; especifícalos entre <...>", expr.span, baseName + "<int>");
                    known(resolved, expr.span, false);
                    concreteArgs.push_back(resolved);
                }
                const Type appT = types.program.typeApp(baseName, concreteArgs);
                types.ensureInstantiated(appT, expr.span);
                expr.text = expr.isEnumConstruct
                    ? (types.program.nameOf(appT) + "." + varName)
                    : types.program.nameOf(appT);
                const auto& c = types.constructor(expr.text, expr.span);
                const auto fields = types.program.data[c.declIndex].variants[c.variantIndex].fields;
                for (std::size_t i = 0; i < expr.args.size(); ++i) {
                    const std::size_t fIdx = expr.fieldIndices[i];
                    DestGuard dg(activeMoveDest, "el campo '" + fields[fIdx].name + "' de " + types.program.nameOf(appT));
                    Type fAct = checked[i] ? expr.args[i].type : typeOf(expr.args[i], ArrayUse::Copy, fields[fIdx].type);
                    coerceIfDyn(expr.args[i], fAct, fields[fIdx].type, fields[fIdx].span);
                    requireType(fAct, fields[fIdx].type, expr.args[i].span, fields[fIdx].span);
                }
                return appT;
            }
        }

        if (needsVisibleCheck) visible(expr.text, expr.span);
        const auto& c = types.constructor(expr.text, expr.span);
        const auto variantCopy = types.program.data[c.declIndex].variants[c.variantIndex];
        const auto& fields = variantCopy.fields;
        const Span varSpan = variantCopy.span;
        checkCallArity(expr, fields.size(), varSpan);
        std::unordered_set<std::size_t> used;
        expr.fieldIndices.clear();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto index = c.isEnum ? i : fieldIndex(variantCopy, expr.labels.at(i), expr.args[i].span);
            if (!used.insert(index).second)
                fail("E_DUP_FIELD", "El constructor repite el campo " + fields[index].name, expr.args[i].span,
                     "Point { x: 1 }", fields[index].span);
            DestGuard dg(activeMoveDest, "el campo '" + fields[index].name + "' de " + types.program.nameOf(c.type));
            Type fAct = typeOf(expr.args[i], ArrayUse::Copy, fields[index].type);
            coerceIfDyn(expr.args[i], fAct, fields[index].type, fields[index].span);
            requireType(fAct, fields[index].type, expr.args[i].span, fields[index].span);
            expr.fieldIndices.push_back(index);
        }
        return c.type;
    }

    Type field(Expr& expr, ArrayUse use = ArrayUse::Ordinary) {
        auto& base = expr.args[0];
        if (base.kind == ExprKind::Name && !lookupOptional(base.text, base.span)) {
            const std::string qname = base.text + "." + expr.text;
            if (const auto* cd = types.program.constant(qname)) {
                visible(qname, expr.span);
                foldConstExpr(expr, *cd);
                return expr.type;
            }
            if (generics.contains(qname))
                fail("E_GENERIC", "No se puede usar una función genérica como valor sin envolverla en closure: " + qname,
                     expr.span, "|x| " + qname + "(x)");
            if (functions.contains(qname)) return synthesizeFnRef(expr, qname);
        }
        const auto type = derefCell(base, typeOf(base));
        types.ensureInstantiated(type, expr.span);
        if ((type == Type::Str || types.program.isArray(type) || type == Type::StringBuilder ||
             types.program.isMap(type) || types.program.isSet(type) || types.program.isPool(type)) && expr.text == "length") {
            Diagnostic e("E_LENGTH", "La longitud se obtiene con len", expr.span);
            e.example = R"(len("hola"))";
            const auto source = types.program.sourceOf(base.span);
            if (base.span.end <= source.size()) {
                auto text = source.substr(base.span.start, base.span.end - base.span.start);
                e.fix = Edit{expr.span, "len(" + std::string(text) + ")"};
            }
            throw e;
        }
        if (!types.hasDecl(type) || types.decl(type).isEnum)
            fail("E_FIELD", "El acceso a campos requiere un struct", expr.span, "struct Point { x: int }");
        const auto& v = types.decl(type).variants.at(0);
        const auto index = fieldIndex(v, expr.text, expr.span);
        expr.fieldIndices = {index};
        const Type ft = v.fields[index].type;
        if (use == ArrayUse::Copy && types.program.isResource(ft))
            failResourceCopy("No se puede mover o copiar el campo de recurso '" + expr.text + "' (" +
                             types.program.nameOf(ft) + ") fuera de su struct dueño.",
                             expr.span, v.fields[index].span);
        return ft;
    }

    // 'a' es el int 97, pero junto a un str vale como el str "a" (así `c == 'a'` funciona tanto si
    // `c` es un byte de `s[i]` como si es un carácter de `s.chars()` o de `for c in s`).
    static bool isCharLiteral(const Expr& e) {
        return e.kind == ExprKind::Integer && e.labels.size() == 1 && e.labels[0].size() == 1;
    }
    static void charToStr(Expr& e) {
        Expr text{ExprKind::String, e.span, e.labels[0], {}};
        e = std::move(text);
    }
    // El código del primer byte de un str: `s[0]`.
    static Expr charCodeOf(Expr text) {
        const Span span = text.span;
        Expr index{ExprKind::Index, span, {}, {}};
        index.args.push_back(std::move(text));
        index.args.push_back(Expr{ExprKind::Integer, span, "0", {}});
        return index;
    }

    Type typeOf(Expr& expr, ArrayUse use = ArrayUse::Ordinary, Type expected = Type::Unknown) {
        if (expected == Type::Str && isCharLiteral(expr)) charToStr(expr);
        if (&expr == preTypedArg0) {
            preTypedArg0 = nullptr;
            if (use == ArrayUse::Copy && types.program.isResource(expr.type)) {
                if (expr.kind == ExprKind::Name) {
                    if (auto* varPtr = lookupOptional(expr.text, expr.span)) {
                        if (inContract)
                            fail("E_CONTRACT", "Los contratos requires/ensures no pueden mover ni consumir recursos.",
                                 expr.span, "fn f(a: [int]) -> int ensures len(result) > 0", varPtr->declared);
                        if (!varPtr->ownedLocal)
                            failResourceCopy("El parámetro prestado '" + expr.text + "' (" +
                                             types.program.nameOf(expr.type) + ") no puede moverse ni copiarse.",
                                             expr.span, varPtr->declared);
                        varPtr->moved = true;
                        varPtr->movedAt = expr.span;
                        varPtr->movedTo = activeMoveDest.empty() ? "un movimiento" : activeMoveDest;
                        expr.isResourceMove = true;
                    }
                } else if (expr.kind == ExprKind::Field) {
                    failResourceCopy("No se puede mover o copiar un campo de recurso fuera de su struct dueño.", expr.span);
                }
            }
            return expr.type;
        }
        expr.allocation = ArrayAllocation::None;
        switch (expr.kind) {
            case ExprKind::Integer: {
                const Type target = (expected != Type::Unknown && Program::isIntType(expected)) ? expected : Type::Int;
                (void)parseExactIntLiteral(types.program, expr, target, false);
                expr.type = target;
                break;
            }
            case ExprKind::Float: {
                const Type target = (expected == Type::F32) ? Type::F32 : Type::Float;
                (void)parseExactFloatLiteral(expr, target);
                expr.type = target;
                break;
            }
            case ExprKind::Boolean: expr.type = Type::Bool; break;
            case ExprKind::String: expr.type = Type::Str; break;
            case ExprKind::Interp: {
                for (auto& arg : expr.args) {
                    if (arg.kind == ExprKind::String) {
                        arg.type = Type::Str;
                        continue;
                    }
                    const Type t = typeOf(arg);
                    if (!Program::isNumericType(t) && t != Type::Str && t != Type::Bool) {
                        Diagnostic d("E_TYPE", "La interpolación admite tipos numéricos, str o bool", arg.span);
                        d.expected = "str | int | i8..u64 | float | f32 | bool";
                        d.actual = types.program.nameOf(t);
                        d.example = R"("valor = {x}")";
                        throw d;
                    }
                }
                expr.type = Type::Str;
                break;
            }
            case ExprKind::Name: {
                if (auto* varPtr = lookupOptional(expr.text, expr.span)) {
                    if (varPtr->moved)
                        failUseAfterMove(expr.text, varPtr->type, expr.span,
                                         varPtr->movedAt, varPtr->movedTo, varPtr->declared);
                    expr.type = varPtr->type;
                    if (use == ArrayUse::Copy && types.program.isResource(varPtr->type)) {
                        if (inContract)
                            fail("E_CONTRACT", "Los contratos requires/ensures no pueden mover ni consumir recursos.",
                                 expr.span, "fn f(a: [int]) -> int ensures len(result) > 0", varPtr->declared);
                        if (!varPtr->ownedLocal)
                            failResourceCopy("El parámetro prestado '" + expr.text + "' (" +
                                             types.program.nameOf(varPtr->type) + ") no puede moverse ni copiarse.",
                                             expr.span, varPtr->declared);
                        varPtr->moved = true;
                        varPtr->movedAt = expr.span;
                        varPtr->movedTo = activeMoveDest;
                        expr.isResourceMove = true;
                    }
                    if (varPtr->regionSite && use != ArrayUse::Inspect)
                        varPtr->regionSite->allocation = ArrayAllocation::Owner;
                    if (types.program.isArray(varPtr->type) && use == ArrayUse::Copy)
                        expr.allocation = ArrayAllocation::Owner;
                    break;
                }
                const std::string qname = current.module.empty() ? expr.text : current.module + "." + expr.text;
                if (const auto* cd = types.program.constant(qname)) {
                    foldConstExpr(expr, *cd);
                    break;
                }
                if (generics.contains(qname))
                    fail("E_GENERIC", "No se puede usar una función genérica como valor sin envolverla en closure: " + expr.text,
                         expr.span, "|x| " + expr.text + "(x)");
                if (functions.contains(qname)) {
                    expr.type = synthesizeFnRef(expr, qname);
                    break;
                }
                if (inContract && expr.text == "result") {
                    if (inEnsures && current.result == Type::Void)
                        fail("E_CONTRACT", "'result' no está disponible en 'ensures' para funciones void.",
                             expr.span, "fn f(x: int) -> int ensures result > 0 { return x }");
                    if (!inEnsures)
                        fail("E_CONTRACT", "'result' solo está disponible en cláusulas 'ensures', no en 'requires'.",
                             expr.span, "fn f(x: int) -> int requires x > 0 ensures result > 0 { return x }");
                }
                lookup(expr.text, expr.span); // throws E_NAME
                break;
            }
            case ExprKind::ArrayLit: {
                if (expr.args.empty()) {
                    if (expected == Type::Unknown || !types.program.isArray(expected))
                        fail("E_EMPTY_ARRAY", "Especifica el tipo del array vacío", expr.span, "let a: [int] = []");
                    expr.type = expected;
                    expr.allocation = ArrayAllocation::Owner;
                    break;
                }
                const Type elemExpected = (expected != Type::Unknown && types.program.isArray(expected))
                    ? types.program.elemOf(expected) : Type::Unknown;
                DestGuard dg(activeMoveDest, "un literal de arreglo");
                Type elemT = typeOf(expr.args[0], ArrayUse::Copy, elemExpected);
                coerceIfDyn(expr.args[0], elemT, elemExpected, expr.span);
                requireValue(elemT, expr.args[0].span);
                for (std::size_t i = 1; i < expr.args.size(); ++i) {
                    Type itemT = typeOf(expr.args[i], ArrayUse::Copy, elemT);
                    coerceIfDyn(expr.args[i], itemT, elemT, expr.args[0].span);
                    requireType(itemT, elemT, expr.args[i].span, expr.args[0].span);
                }
                expr.type = types.program.arrayOf(elemT);
                expr.allocation = ArrayAllocation::Owner;
                break;
            }
            case ExprKind::Index: {
                const Type baseT = typeOf(expr.args[0], ArrayUse::Inspect);
                if (baseT == Type::Str) {
                    requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
                    expr.type = Type::Int;
                    break;
                }
                if (const auto* tupleApp = types.program.typeAppOf(baseT);
                    tupleApp && tupleApp->baseName.starts_with("Tuple") && expr.args[1].kind == ExprKind::Integer) {
                    // t[0] sobre una tupla es su primer elemento (como en Python).
                    Expr field{ExprKind::Field, expr.span, "v" + expr.args[1].text, {}};
                    field.args.push_back(std::move(expr.args[0]));
                    expr = std::move(field);
                    return typeOf(expr, use, expected);
                }
                if (types.program.isMap(baseT) && !strictSyntax()) {
                    // `m[k]` es el valor de la clave o el valor cero de V si no está (como en Go).
                    // `m.get(k)` sigue devolviendo V? cuando hace falta distinguir.
                    if (auto zero = zeroLiteral(types.program.mapVal(baseT), expr.span)) {
                        Expr get{ExprKind::Call, expr.span, "mapGet", {}};
                        get.args.push_back(std::move(expr.args[0]));
                        get.args.push_back(std::move(expr.args[1]));
                        Expr value{ExprKind::Call, expr.span, "unwrapOr", {}};
                        value.args.push_back(std::move(get));
                        value.args.push_back(std::move(*zero));
                        expr = std::move(value);
                        return typeOf(expr, use, expected);
                    }
                }
                if (types.program.isMap(baseT) || types.program.isPool(baseT)) {
                    const bool isP = types.program.isPool(baseT);
                    Diagnostic d(isP ? "E_POOL_INDEX" : "E_MAP_INDEX",
                                 isP ? "Usa poolGet(pool, asa) para leer un Pool<T> (devuelve T?)."
                                     : "Usa mapGet(mapa, clave) para leer un Map<K, V> (devuelve V?).",
                                 expr.span);
                    const auto src = types.program.sourceOf(expr.span);
                    if (expr.span.end <= src.size() &&
                        expr.args[0].span.end <= expr.args[1].span.start &&
                        expr.args[1].span.end <= expr.span.end) {
                        const std::string baseText(src.substr(expr.args[0].span.start, expr.args[0].span.end - expr.args[0].span.start));
                        const std::string keyText(src.substr(expr.args[1].span.start, expr.args[1].span.end - expr.args[1].span.start));
                        d.fix = Edit{expr.span, std::string(isP ? "poolGet(" : "mapGet(") + baseText + ", " + keyText + ")"};
                    }
                    d.expected = "[T] | str";
                    d.actual = types.program.nameOf(baseT);
                    d.example = isP ? "let v = poolGet(p, h)" : "let v = mapGet(m, clave)";
                    throw d;
                }
                if (!types.program.isArray(baseT)) {
                    Diagnostic d("E_TYPE", "Se esperaba un array o str al indexar", expr.args[0].span);
                    d.expected = "[T] | str";
                    d.actual = types.program.nameOf(baseT);
                    throw d;
                }
                requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
                expr.type = types.program.elemOf(baseT);
                if (types.program.isResource(expr.type))
                    failResourceCopy("No se puede leer un recurso no copiable mediante a[i]; usa pop(mut a) para extraer su dueño.", expr.span);
                break;
            }
            case ExprKind::Slice: {
                const Type baseT = typeOf(expr.args[0], ArrayUse::Inspect);
                if (baseT != Type::Str && !types.program.isArray(baseT)) {
                    Diagnostic d("E_TYPE", "El corte [lo..hi] requiere un array o str", expr.args[0].span);
                    d.expected = "[T] | str";
                    d.actual = types.program.nameOf(baseT);
                    throw d;
                }
                if (types.program.isArray(baseT) && types.program.isResource(types.program.elemOf(baseT)))
                    failResourceCopy("No se puede cortar un arreglo de recursos no copiables.", expr.span);
                requireType(typeOf(expr.args[1]), Type::Int, expr.args[1].span);
                requireType(typeOf(expr.args[2]), Type::Int, expr.args[2].span);
                if (types.program.isArray(baseT)) expr.allocation = ArrayAllocation::Owner;
                expr.type = baseT;
                break;
            }
            case ExprKind::Call: expr.type = inferCall(expr, expected); break;
            case ExprKind::MethodCall: expr.type = inferMethodCall(expr, expected); break;
            case ExprKind::AssocCall: expr.type = inferAssocCall(expr, expected); break;
            case ExprKind::BoxDyn:
            case ExprKind::DynCall: break;
            case ExprKind::Closure: expr.type = inferClosure(expr, expected); break;
            case ExprKind::ClosureCall: expr.type = inferClosureCall(expr); break;
            case ExprKind::Match: expr.type = matchExpr(expr, expected); break;
            case ExprKind::If: expr.type = ifExpr(expr, expected); break;
            case ExprKind::Unary: {
                if (expr.text == "!") {
                    requireType(typeOf(expr.args[0]), Type::Bool, expr.args[0].span);
                    expr.type = Type::Bool;
                } else if (expr.text == "~") {
                    const bool expOk = (expected != Type::Unknown &&
                        (Program::isIntType(expected) ||
                         (types.program.isSimd(expected) && Program::isIntType(types.program.simdElem(expected)))));
                    const Type expT = expOk ? expected : Type::Unknown;
                    const Type inner = typeOf(expr.args[0], ArrayUse::Ordinary, expT);
                    const bool innerOk = Program::isIntType(inner) ||
                        (types.program.isSimd(inner) && Program::isIntType(types.program.simdElem(inner)));
                    if (!innerOk)
                        fail("E_TYPE", "Complemento ~ requiere un tipo entero", expr.args[0].span, "~x");
                    expr.type = inner;
                } else {
                    if (expr.args[0].kind == ExprKind::Integer && isDecimalDigits(expr.args[0].text)) {
                        if (expected != Type::Unknown && Program::isUnsignedInt(expected))
                            fail("E_TYPE", "Menos unario no aplica a enteros sin signo", expr.span, "0");
                        if (expected == Type::I8 || expected == Type::I16 || expected == Type::I32) {
                            const std::uint64_t bits = parseExactIntLiteral(types.program, expr.args[0], expected, true);
                            const Integer val = signExtendWidth(bits, Program::intBitWidth(expected));
                            expr.kind = ExprKind::Integer;
                            expr.text = std::to_string(val);
                            expr.args.clear();
                            expr.type = expected;
                            break;
                        }
                    }
                    const Type expT = (expected != Type::Unknown &&
                                       (Program::isSignedInt(expected) || Program::isFloatType(expected) ||
                                        types.program.isSimd(expected)))
                        ? expected : Type::Unknown;
                    const auto inner = typeOf(expr.args[0], ArrayUse::Ordinary, expT);
                    if (!Program::isSignedInt(inner) && !Program::isFloatType(inner) && !types.program.isSimd(inner))
                        fail("E_TYPE", "Menos unario requiere un entero con signo o flotante", expr.args[0].span, "-x");
                    expr.type = inner;
                }
                break;
            }
            case ExprKind::Binary: expr.type = inferBinary(expr, expected); break;
            case ExprKind::Field: expr.type = field(expr, use); break;
            case ExprKind::Construct: expr.type = construct(expr, expected); break;
            case ExprKind::Try: {
                if (inContract)
                    fail("E_CONTRACT", "El operador '?' no puede usarse dentro de contratos requires/ensures.",
                         expr.span, "fn f(x: int?) -> int requires isSome(x)");
                DestGuard dg(activeMoveDest, activeMoveDest.empty() ? "el operador '?'" : activeMoveDest);
                const Type innerT = typeOf(expr.args[0], ArrayUse::Copy);
                const Type effectiveRet = current.result != Type::Unknown ? current.result
                                        : (inferredReturn ? *inferredReturn : Type::Unknown);
                if (types.program.isOption(innerT)) {
                    if (!current.isTest && effectiveRet == Type::Unknown && inferredReturn) {
                        *inferredReturn = types.program.optionOf(types.program.optionInner(innerT));
                    }
                    const Type checkRet = current.result != Type::Unknown ? current.result
                                        : (inferredReturn ? *inferredReturn : Type::Unknown);
                    if (!current.isTest && !types.program.isOption(checkRet)) {
                        Diagnostic d("E_TRY_RETURN", "El operador ? sobre T? requiere una función que devuelva T?.", expr.span);
                        d.expected = types.program.nameOf(innerT);
                        d.actual = types.program.nameOf(checkRet);
                        d.related.push_back(current.span);
                        d.example = "fn f() -> int? { let v = lookup()?\nreturn Some(v) }";
                        throw d;
                    }
                    expr.type = types.program.optionInner(innerT);
                } else if (types.program.isResult(innerT)) {
                    const Type retErr = types.program.resultErr(innerT);
                    if (!current.isTest && effectiveRet == Type::Unknown && inferredReturn) {
                        *inferredReturn = types.program.resultOf(types.program.resultOk(innerT), retErr);
                    }
                    const Type checkRet = current.result != Type::Unknown ? current.result
                                        : (inferredReturn ? *inferredReturn : Type::Unknown);
                    if (!current.isTest && !types.program.isResult(checkRet)) {
                        Diagnostic d("E_TRY_RETURN", "El operador ? sobre Result requiere una función que devuelva Result<_, E>.", expr.span);
                        d.expected = "Result<..., " + types.program.nameOf(retErr) + ">";
                        d.actual = types.program.nameOf(checkRet);
                        d.related.push_back(current.span);
                        d.example = "fn f() -> Result<int, str> { let v = parse()?\nreturn Ok(v) }";
                        throw d;
                    }
                    if (!current.isTest && retErr != types.program.resultErr(checkRet)) {
                        Diagnostic d("E_TRY_ERR", "El tipo de error en ? no coincide con el Err del retorno de la función.", expr.span);
                        d.expected = types.program.nameOf(types.program.resultErr(checkRet));
                        d.actual = types.program.nameOf(retErr);
                        d.related.push_back(current.span);
                        d.example = "fn f() -> Result<int, str> { return parse() }";
                        throw d;
                    }
                    expr.type = types.program.resultOk(innerT);
                } else {
                    Diagnostic d("E_TRY_TYPE", "El operador ? requiere T? o Result<T, E> (para condicional usa if/else).", expr.span);
                    d.expected = "T? | Result<T, E>";
                    d.actual = types.program.nameOf(innerT);
                    d.example = "let x = parse(s)?";
                    throw d;
                }
                types.ensureInstantiated(expr.type, expr.span);
                break;
            }
        }
        reserveValue(expr.type, expr.span);
        return expr.type;
    }

    static std::string canonicalUnqualified(std::string_view mod, const std::string& name) {
        if (name.find('.') == std::string::npos && !mod.empty()) return std::string(mod) + "." + name;
        return name;
    }

    Type probeExprType(const Expr& e, const std::unordered_map<std::string, Type>& futureLocals) const {
        switch (e.kind) {
            case ExprKind::Integer: return Type::Int;
            case ExprKind::Float: return Type::Float;
            case ExprKind::Boolean: return Type::Bool;
            case ExprKind::String:
            case ExprKind::Interp: return Type::Str;
            case ExprKind::Name: {
                if (const auto* v = locals.find(e.text)) return v->type;
                if (auto it = futureLocals.find(e.text); it != futureLocals.end()) return it->second;
                const std::string qConst = canonicalUnqualified(current.module, e.text);
                if (const auto* cd = types.program.constant(qConst)) return cd->type;
                if (const auto* cd = types.program.constant(e.text)) return cd->type;
                return Type::Unknown;
            }
            case ExprKind::ArrayLit: {
                if (!e.args.empty()) {
                    const Type et = probeExprType(e.args[0], futureLocals);
                    if (et != Type::Unknown) return types.program.arrayOf(et);
                }
                return Type::Unknown;
            }
            case ExprKind::Unary:
                if (e.text == "!") return Type::Bool;
                if (e.text == "~") {
                    const Type it = probeExprType(e.args[0], futureLocals);
                    return Program::isIntType(it) ? it : Type::Int;
                }
                return probeExprType(e.args[0], futureLocals);
            case ExprKind::Binary:
                if (e.text == "==" || e.text == "!=" || e.text == "<" || e.text == "<=" ||
                    e.text == ">" || e.text == ">=" || e.text == "&&" || e.text == "||")
                    return Type::Bool;
                if (const Type lt = probeExprType(e.args[0], futureLocals); lt != Type::Unknown) return lt;
                return probeExprType(e.args[1], futureLocals);
            case ExprKind::Index: {
                const Type bt = probeExprType(e.args[0], futureLocals);
                if (bt == Type::Str) return Type::Int;
                if (types.program.isArray(bt)) return types.program.elemOf(bt);
                return Type::Unknown;
            }
            case ExprKind::Slice: {
                const Type bt = probeExprType(e.args[0], futureLocals);
                if (bt == Type::Str || types.program.isArray(bt)) return bt;
                return Type::Unknown;
            }
            case ExprKind::Field: {
                if (e.args[0].kind == ExprKind::Name && !locals.find(e.args[0].text) &&
                    !futureLocals.contains(e.args[0].text)) {
                    const std::string qname = e.args[0].text + "." + e.text;
                    if (const auto* cd = types.program.constant(qname)) return cd->type;
                    if (auto cit = types.constructors.find(qname); cit != types.constructors.end())
                        return cit->second.type;
                }
                const Type bt = probeExprType(e.args[0], futureLocals);
                if (bt != Type::Unknown && types.hasDecl(bt) && !types.decl(bt).isEnum) {
                    for (const auto& f : types.decl(bt).variants.at(0).fields)
                        if (f.name == e.text) return f.type;
                }
                return Type::Unknown;
            }
            case ExprKind::Construct: {
                if (e.text == "Some" && e.args.size() == 1) {
                    const Type et = probeExprType(e.args[0], futureLocals);
                    if (et != Type::Unknown) return types.program.optionOf(et);
                }
                if (auto cit = types.constructors.find(e.text); cit != types.constructors.end())
                    return cit->second.type;
                if (!e.typeArgs.empty()) {
                    if (e.text.starts_with("@Option.") && e.typeArgs.size() == 1)
                        return types.program.optionOf(e.typeArgs[0]);
                    if (e.text.starts_with("@Result.") && e.typeArgs.size() == 2)
                        return types.program.resultOf(e.typeArgs[0], e.typeArgs[1]);
                    const auto dotPos = e.isEnumConstruct ? e.text.rfind('.') : std::string::npos;
                    const std::string baseName = dotPos == std::string::npos ? e.text : e.text.substr(0, dotPos);
                    if (types.genericDecls.contains(baseName))
                        return types.program.typeApp(baseName, e.typeArgs);
                }
                return Type::Unknown;
            }
            case ExprKind::Call: {
                if (auto nt = numericBuiltinType(e.text)) return *nt;
                if (e.text == "sizeOf" || e.text == "alignOf") return Type::Int;
                if (e.text == "bitcast" && e.typeArgs.size() == 1) return e.typeArgs[0];
                if ((e.text == "ptrNull" || e.text == "ptrCast" || e.text == "ptrFromAddr") && e.typeArgs.size() == 1)
                    return types.program.ptrOf(e.typeArgs[0]);
                if ((e.text == "mutPtrNull" || e.text == "mutPtrCast" || e.text == "mutPtrFromAddr" ||
                     e.text == "ptrAlloc" || e.text == "ptrAllocAligned") && e.typeArgs.size() == 1)
                    return types.program.mutPtrOf(e.typeArgs[0]);
                if (e.text == "ptrIsNull") return Type::Bool;
                if (e.text == "ptrAddr") return Type::U64;
                if (e.text == "fnPtr") return types.program.ptrOf(Type::Void);
                if (e.text == "cstr") return types.program.arrayOf(Type::U8);
                if (e.text == "strFromCStr" || e.text == "strFromPtr") return Type::Str;
                if (e.text == "ptrOf" && !e.args.empty()) {
                    const Type pt = probeExprType(e.args[0], futureLocals);
                    if (pt != Type::Unknown) return types.program.ptrOf(pt);
                }
                if (e.text == "mutPtrOf" && !e.args.empty()) {
                    const Type pt = probeExprType(e.args[0], futureLocals);
                    if (pt != Type::Unknown) return types.program.mutPtrOf(pt);
                }
                if (e.text == "slicePtr" && !e.args.empty()) {
                    const Type at = probeExprType(e.args[0], futureLocals);
                    if (types.program.isArray(at)) return types.program.ptrOf(types.program.elemOf(at));
                }
                if (e.text == "mutSlicePtr" && !e.args.empty()) {
                    const Type at = probeExprType(e.args[0], futureLocals);
                    if (types.program.isArray(at)) return types.program.mutPtrOf(types.program.elemOf(at));
                }
                if (e.text == "ptrConst" && !e.args.empty()) {
                    const Type pt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isMutPtr(pt)) return types.program.ptrOf(types.program.ptrElem(pt));
                }
                if (e.text == "ptrMut" && !e.args.empty()) {
                    const Type pt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isPtr(pt)) return types.program.mutPtrOf(types.program.ptrElem(pt));
                }
                if (e.text == "ptrAdd" && !e.args.empty()) {
                    const Type pt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isAnyPtr(pt)) return pt;
                }
                if (e.text == "ptrRead" && !e.args.empty()) {
                    const Type pt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isAnyPtr(pt)) return types.program.ptrElem(pt);
                }
                if (e.text == "simdLanes") return Type::Int;
                if ((e.text == "simdSplat" || e.text == "simdFromArray" ||
                     e.text == "simdLoad" || e.text == "simdLoadAligned") && e.typeArgs.size() == 1)
                    return e.typeArgs[0];
                if ((e.text == "simdGet" || e.text == "simdSum" ||
                     e.text == "simdReduceMin" || e.text == "simdReduceMax") && !e.args.empty()) {
                    const Type vt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isSimd(vt)) return types.program.simdElem(vt);
                }
                if ((e.text == "simdCmpEq" || e.text == "simdCmpLt" || e.text == "simdCmpLe" ||
                     e.text == "simdCmpGt" || e.text == "simdCmpGe") && !e.args.empty()) {
                    const Type vt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isSimd(vt))
                        return simdMaskType(types.program, types.program.simdElem(vt), types.program.simdLanes(vt));
                }
                if (e.text == "simdSelect" && e.args.size() >= 2) {
                    const Type vt = probeExprType(e.args[1], futureLocals);
                    if (vt != Type::Unknown) return vt;
                }
                if (e.text == "wrappingAdd" || e.text == "wrappingSub" || e.text == "wrappingMul" ||
                    e.text == "saturatingAdd" || e.text == "saturatingSub" || e.text == "saturatingMul" ||
                    e.text == "simdSet" || e.text == "simdFma" || e.text == "simdMin" ||
                    e.text == "simdMax" || e.text == "simdAbs" || e.text == "simdSqrt" ||
                    e.text == "clz" || e.text == "ctz" || e.text == "popcnt" || e.text == "bswap" ||
                    e.text == "rotl" || e.text == "rotr" ||
                    e.text == "abs" || e.text == "min" || e.text == "max" ||
                    e.text == "sqrt" || e.text == "floor" || e.text == "ceil" || e.text == "round" ||
                    e.text == "sin" || e.text == "cos" || e.text == "pow") {
                    if (!e.args.empty()) {
                        const Type pt = probeExprType(e.args[0], futureLocals);
                        if (pt != Type::Unknown) return pt;
                    }
                    return Type::Int;
                }
                if (e.text == "len" || e.text == "argsCount" ||
                    e.text == "clockNs" || e.text == "execStatus" || e.text == "readByte" ||
                    e.text == "parseInt" || e.text == "tryParseInt" || e.text == "strIndexOf")
                    return Type::Int;
                if (e.text == "parseFloat" || e.text == "tryParseFloat") return Type::Float;
                if (e.text == "isNaN" || e.text == "isInf") return Type::Bool;
                if (e.text == "str" || e.text == "readFile" || e.text == "envGet" ||
                    e.text == "execCapture" || e.text == "argAt")
                    return Type::Str;
                if (e.text == "fileExists" || e.text == "removeFile" || e.text == "makeDir" ||
                    e.text == "writeFile" || e.text == "appendFile" || e.text == "isSome" ||
                    e.text == "isNone" || e.text == "isOk" || e.text == "isErr")
                    return Type::Bool;
                if (e.text == "sbNew" || e.text == "sbWithCap") return Type::StringBuilder;
                if (e.text == "repeat" && e.args.size() == 2) {
                    const Type et = probeExprType(e.args[0], futureLocals);
                    if (et != Type::Unknown) return types.program.arrayOf(et);
                }
                if ((e.text == "mapNew" || e.text == "mapSecure" || e.text == "mapWithCap") && e.typeArgs.size() == 2)
                    return types.program.mapOf(e.typeArgs[0], e.typeArgs[1]);
                if ((e.text == "setNew" || e.text == "setSecure" || e.text == "setWithCap") && e.typeArgs.size() == 1)
                    return types.program.setOf(e.typeArgs[0]);
                if ((e.text == "poolNew" || e.text == "poolWithCap") && e.typeArgs.size() == 1)
                    return types.program.poolOf(e.typeArgs[0]);
                if (e.text == "atomicNew") {
                    if (e.typeArgs.size() == 1) return types.program.atomicOf(e.typeArgs[0]);
                    if (!e.args.empty()) {
                        const Type et = probeExprType(e.args[0], futureLocals);
                        if (et != Type::Unknown) return types.program.atomicOf(et);
                    }
                }
                if ((e.text == "atomicLoad" || e.text == "atomicLoadOrd" ||
                     e.text == "atomicSwap" || e.text == "atomicSwapOrd" ||
                     e.text == "atomicAdd" || e.text == "atomicAddOrd" ||
                     e.text == "atomicSub" || e.text == "atomicSubOrd" ||
                     e.text == "atomicAnd" || e.text == "atomicAndOrd" ||
                     e.text == "atomicOr" || e.text == "atomicOrOrd" ||
                     e.text == "atomicXor" || e.text == "atomicXorOrd") && !e.args.empty()) {
                    const Type at = probeExprType(e.args[0], futureLocals);
                    if (types.program.isAtomic(at)) return types.program.atomicElem(at);
                }
                if (e.text == "atomicCas" || e.text == "atomicCasOrd" || e.text == "ringPush") return Type::Bool;
                if (e.text == "ringNew" && e.typeArgs.size() == 1) return types.program.ringQueueOf(e.typeArgs[0]);
                if (e.text == "ringPop" && !e.args.empty()) {
                    const Type qt = probeExprType(e.args[0], futureLocals);
                    if (types.program.isRingQueue(qt)) return types.program.optionOf(types.program.ringQueueElem(qt));
                }
                if (e.text == "ringLen" || e.text == "ringCap" ||
                    e.text == "regionUsed" || e.text == "regionCapacity")
                    return Type::Int;
                if (e.text == "regionNew") return Type::Region;
                if ((e.text == "regionAlloc" || e.text == "regionAllocAligned") && e.typeArgs.size() == 1)
                    return types.program.mutPtrOf(e.typeArgs[0]);
                const std::string qfn = canonicalUnqualified(current.module, e.text);
                if (auto it = functions.find(qfn); it != functions.end()) return it->second->result;
                if (auto it = functions.find(e.text); it != functions.end()) return it->second->result;
                return Type::Unknown;
            }
            case ExprKind::MethodCall: {
                if (e.args.size() >= 1 && e.args[0].kind == ExprKind::Name &&
                    !locals.find(e.args[0].text) && !futureLocals.contains(e.args[0].text)) {
                    const std::string qfn = e.args[0].text + "." + e.text;
                    if (auto it = functions.find(qfn); it != functions.end()) return it->second->result;
                }
                return Type::Unknown;
            }
            case ExprKind::Match: {
                if (e.matchExpr) {
                    for (const auto& arm : e.matchExpr->arms) {
                        if (arm.resultExpr) {
                            const Type t = probeExprType(*arm.resultExpr, futureLocals);
                            if (t != Type::Unknown) return t;
                        }
                    }
                }
                return Type::Unknown;
            }
            case ExprKind::If: {
                if (e.ifExpr) {
                    if (e.ifExpr->thenResult) {
                        const Type t = probeExprType(*e.ifExpr->thenResult, futureLocals);
                        if (t != Type::Unknown) return t;
                    }
                    if (e.ifExpr->elseResult) {
                        const Type t = probeExprType(*e.ifExpr->elseResult, futureLocals);
                        if (t != Type::Unknown) return t;
                    }
                }
                return Type::Unknown;
            }
            default:
                return Type::Unknown;
        }
    }

    Type inferContainerFromUse(const Expr& initExpr, const std::string& varName,
                               const std::vector<Stmt>& stmts, std::size_t startIdx) const {
        if (initExpr.kind != ExprKind::Call || !initExpr.typeArgs.empty()) return Type::Unknown;
        enum class CKind { None, Map, Set, Pool };
        CKind kind = CKind::None;
        if (initExpr.text == "mapNew" || initExpr.text == "mapSecure" || initExpr.text == "mapWithCap")
            kind = CKind::Map;
        else if (initExpr.text == "setNew" || initExpr.text == "setSecure" || initExpr.text == "setWithCap")
            kind = CKind::Set;
        else if (initExpr.text == "poolNew" || initExpr.text == "poolWithCap")
            kind = CKind::Pool;
        if (kind == CKind::None) return Type::Unknown;

        Type keyT = Type::Unknown, valT = Type::Unknown, elemT = Type::Unknown;
        auto done = [&]() {
            if (kind == CKind::Map) return keyT != Type::Unknown && valT != Type::Unknown;
            return elemT != Type::Unknown;
        };
        auto acceptHint = [&](Type hint) {
            if (hint == Type::Unknown) return;
            if (kind == CKind::Map && types.program.isMap(hint)) {
                if (keyT == Type::Unknown) keyT = types.program.mapKey(hint);
                if (valT == Type::Unknown) valT = types.program.mapVal(hint);
            } else if (kind == CKind::Set && types.program.isSet(hint)) {
                if (elemT == Type::Unknown) elemT = types.program.setElem(hint);
            } else if (kind == CKind::Pool && types.program.isPool(hint)) {
                if (elemT == Type::Unknown) elemT = types.program.poolElem(hint);
            }
        };
        auto isVar = [&](const Expr& e) { return e.kind == ExprKind::Name && e.text == varName; };

        std::unordered_map<std::string, Type> futureLocals;
        auto scanExpr = [&](auto& selfE, auto& selfS, const Expr& e, Type expHint) -> void {
            if (done()) return;
            if (isVar(e)) {
                acceptHint(expHint);
                return;
            }
            if (e.kind == ExprKind::Call) {
                if (kind == CKind::Map && !e.args.empty() && isVar(e.args[0])) {
                    if (e.text == "mapSet" && e.args.size() == 3) {
                        if (keyT == Type::Unknown) keyT = probeExprType(e.args[1], futureLocals);
                        if (valT == Type::Unknown) valT = probeExprType(e.args[2], futureLocals);
                    } else if ((e.text == "mapGet" || e.text == "mapHas" || e.text == "mapDel") && e.args.size() == 2) {
                        if (keyT == Type::Unknown) keyT = probeExprType(e.args[1], futureLocals);
                        if (e.text == "mapGet" && valT == Type::Unknown && expHint != Type::Unknown &&
                            types.program.isOption(expHint)) {
                            valT = types.program.optionInner(expHint);
                        }
                    }
                } else if (kind == CKind::Set && !e.args.empty() && isVar(e.args[0])) {
                    if ((e.text == "setAdd" || e.text == "setDel" || e.text == "setHas") && e.args.size() == 2) {
                        if (elemT == Type::Unknown) elemT = probeExprType(e.args[1], futureLocals);
                    }
                } else if (kind == CKind::Pool && !e.args.empty() && isVar(e.args[0])) {
                    if (e.text == "poolAdd" && e.args.size() == 2) {
                        if (elemT == Type::Unknown) elemT = probeExprType(e.args[1], futureLocals);
                    } else if (e.text == "poolSet" && e.args.size() == 3) {
                        if (elemT == Type::Unknown) elemT = probeExprType(e.args[2], futureLocals);
                    }
                } else if (kind == CKind::Map && e.text == "unwrapOr" && e.args.size() == 2 &&
                           e.args[0].kind == ExprKind::Call && e.args[0].text == "mapGet" &&
                           e.args[0].args.size() == 2 && isVar(e.args[0].args[0])) {
                    if (valT == Type::Unknown) valT = probeExprType(e.args[1], futureLocals);
                }

                const std::string qfn = canonicalUnqualified(current.module, e.text);
                const Function* fnPtr = nullptr;
                if (auto it = functions.find(qfn); it != functions.end()) fnPtr = it->second;
                else if (auto it = functions.find(e.text); it != functions.end()) fnPtr = it->second;
                for (std::size_t k = 0; k < e.args.size(); ++k) {
                    const Type pHint = (fnPtr && k < fnPtr->params.size()) ? fnPtr->params[k].type : Type::Unknown;
                    selfE(selfE, selfS, e.args[k], pHint);
                }
                return;
            }
            if (e.kind == ExprKind::MethodCall && e.args.size() >= 1) {
                if (isVar(e.args[0])) {
                    if (kind == CKind::Map) {
                        if (e.text == "set" && e.args.size() == 3) {
                            if (keyT == Type::Unknown) keyT = probeExprType(e.args[1], futureLocals);
                            if (valT == Type::Unknown) valT = probeExprType(e.args[2], futureLocals);
                        } else if ((e.text == "get" || e.text == "has" || e.text == "del") && e.args.size() == 2) {
                            if (keyT == Type::Unknown) keyT = probeExprType(e.args[1], futureLocals);
                            if (e.text == "get" && valT == Type::Unknown && expHint != Type::Unknown &&
                                types.program.isOption(expHint)) {
                                valT = types.program.optionInner(expHint);
                            }
                        }
                    } else if (kind == CKind::Set) {
                        if ((e.text == "add" || e.text == "del" || e.text == "has") && e.args.size() == 2) {
                            if (elemT == Type::Unknown) elemT = probeExprType(e.args[1], futureLocals);
                        }
                    } else if (kind == CKind::Pool) {
                        if ((e.text == "add" && e.args.size() == 2) || (e.text == "set" && e.args.size() == 3)) {
                            const std::size_t vi = e.text == "add" ? 1 : 2;
                            if (elemT == Type::Unknown) elemT = probeExprType(e.args[vi], futureLocals);
                        }
                    }
                }
                if (e.args[0].kind == ExprKind::Name &&
                    !locals.find(e.args[0].text) && !futureLocals.contains(e.args[0].text)) {
                    const std::string qfn = e.args[0].text + "." + e.text;
                    const Function* fnPtr = nullptr;
                    if (auto it = functions.find(qfn); it != functions.end()) fnPtr = it->second;
                    for (std::size_t k = 1; k < e.args.size(); ++k) {
                        const Type pHint = (fnPtr && (k - 1) < fnPtr->params.size()) ? fnPtr->params[k - 1].type : Type::Unknown;
                        selfE(selfE, selfS, e.args[k], pHint);
                    }
                    return;
                }
            }
            if (e.kind == ExprKind::Construct) {
                if (auto cit = types.constructors.find(e.text); cit != types.constructors.end()) {
                    const auto& var = types.program.data[cit->second.declIndex].variants[cit->second.variantIndex];
                    for (std::size_t k = 0; k < e.args.size(); ++k) {
                        Type fHint = Type::Unknown;
                        if (k < e.labels.size() && !e.labels[k].empty()) {
                            for (const auto& f : var.fields) if (f.name == e.labels[k]) { fHint = f.type; break; }
                        } else if (k < var.fields.size()) {
                            fHint = var.fields[k].type;
                        }
                        selfE(selfE, selfS, e.args[k], fHint);
                    }
                    return;
                }
            }
            if (e.matchExpr) {
                selfE(selfE, selfS, e.matchExpr->scrutinee, Type::Unknown);
                for (const auto& arm : e.matchExpr->arms) {
                    bool shadowed = false;
                    for (const auto& n : arm.names) if (n == varName) shadowed = true;
                    if (shadowed) continue;
                    selfS(selfS, selfE, arm.body, 0);
                    if (arm.resultExpr) selfE(selfE, selfS, *arm.resultExpr, expHint);
                }
                return;
            }
            if (e.ifExpr) {
                selfE(selfE, selfS, e.ifExpr->condition, Type::Bool);
                selfS(selfS, selfE, e.ifExpr->thenBody, 0);
                if (e.ifExpr->thenResult) selfE(selfE, selfS, *e.ifExpr->thenResult, expHint);
                selfS(selfS, selfE, e.ifExpr->elseBody, 0);
                if (e.ifExpr->elseResult) selfE(selfE, selfS, *e.ifExpr->elseResult, expHint);
                return;
            }
            for (const auto& a : e.args) selfE(selfE, selfS, a, Type::Unknown);
        };

        auto scanBlock = [&](auto& selfS, auto& selfE, const std::vector<Stmt>& body, std::size_t fromIdx) -> void {
            std::vector<std::string> pushedKeys;
            for (std::size_t i = fromIdx; i < body.size() && !done(); ++i) {
                const auto& st = body[i];
                switch (st.kind) {
                    case StmtKind::Let:
                    case StmtKind::Var: {
                        selfE(selfE, selfS, st.exprs[0], st.annotation);
                        if (st.name == varName) {
                            for (const auto& k : pushedKeys) futureLocals.erase(k);
                            return;
                        }
                        if (st.name != "_") {
                            Type lt = st.annotation != Type::Unknown ? st.annotation : probeExprType(st.exprs[0], futureLocals);
                            if (lt != Type::Unknown && !futureLocals.contains(st.name)) {
                                futureLocals.emplace(st.name, lt);
                                pushedKeys.push_back(st.name);
                            }
                        }
                        break;
                    }
                    case StmtKind::Return:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, current.result);
                        break;
                    case StmtKind::If:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, Type::Unknown);
                        selfS(selfS, selfE, st.body, 0);
                        selfS(selfS, selfE, st.otherwise, 0);
                        break;
                    case StmtKind::SafetyBlock:
                        selfS(selfS, selfE, st.body, 0);
                        break;
                    case StmtKind::While:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, Type::Unknown);
                        selfS(selfS, selfE, st.body, 0);
                        break;
                    case StmtKind::For:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, Type::Unknown);
                        if (st.name != varName) {
                            const bool added = (st.name != "_" && !futureLocals.contains(st.name));
                            if (added) futureLocals.emplace(st.name, Type::Int);
                            selfS(selfS, selfE, st.body, 0);
                            if (added) futureLocals.erase(st.name);
                        }
                        break;
                    case StmtKind::ForEach:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, Type::Unknown);
                        if (st.name != varName) {
                            bool added = false;
                            if (st.name != "_" && !futureLocals.contains(st.name)) {
                                const Type arrT = probeExprType(st.exprs[0], futureLocals);
                                if (types.program.isArray(arrT)) {
                                    futureLocals.emplace(st.name, types.program.elemOf(arrT));
                                    added = true;
                                }
                            }
                            selfS(selfS, selfE, st.body, 0);
                            if (added) futureLocals.erase(st.name);
                        }
                        break;
                    case StmtKind::Match:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, Type::Unknown);
                        for (const auto& arm : st.arms) {
                            bool shadowed = false;
                            for (const auto& n : arm.names) if (n == varName) shadowed = true;
                            if (!shadowed) selfS(selfS, selfE, arm.body, 0);
                        }
                        break;
                    default:
                        for (const auto& e : st.exprs) selfE(selfE, selfS, e, Type::Unknown);
                        break;
                }
            }
            for (const auto& k : pushedKeys) futureLocals.erase(k);
        };

        scanBlock(scanBlock, scanExpr, stmts, startIdx);
        if (!done()) return Type::Unknown;
        if (kind == CKind::Map) return types.program.mapOf(keyT, valT);
        if (kind == CKind::Set) return types.program.setOf(elemT);
        return types.program.poolOf(elemT);
    }

    bool block(std::vector<Stmt>& statements, bool nested = true) {
        if (nested) locals.levels.emplace_back();
        bool returns = false;
        const bool savedTerm = pathTerminated;
        pathTerminated = false;
        for (std::size_t idx = 0; idx < statements.size(); ++idx) {
            auto& statement = statements[idx];
            Type initHint = Type::Unknown;
            if ((statement.kind == StmtKind::Let || statement.kind == StmtKind::Var) &&
                statement.annotation == Type::Unknown && statement.name != "_") {
                initHint = inferContainerFromUse(statement.exprs[0], statement.name, statements, idx + 1);
            }
            if (checkStatement(statement, initHint)) returns = true;
        }
        const bool ended = pathTerminated;
        if (nested) locals.levels.pop_back();
        pathTerminated = savedTerm || ended;
        return returns;
    }

    bool matchImpl(Expr& scrutinee, std::vector<MatchArm>& arms, Span matchSpan,
                   bool isExpr, Type expected, Type* outType = nullptr) {
        const auto type = derefCell(scrutinee, typeOf(scrutinee));
        types.ensureInstantiated(type, scrutinee.span);
        if (!types.hasDecl(type) || !types.decl(type).isEnum)
            fail("E_MATCH_TYPE", "match requiere un enum", scrutinee.span, "enum State { Ready }");
        if (types.program.isResource(type)) {
            if (scrutinee.kind == ExprKind::Name) {
                if (auto* varPtr = lookupOptional(scrutinee.text, scrutinee.span)) {
                    if (!varPtr->ownedLocal)
                        failResourceCopy("El parámetro prestado '" + scrutinee.text + "' (" +
                                         types.program.nameOf(type) + ") no puede moverse en match.",
                                         scrutinee.span, varPtr->declared);
                    varPtr->moved = true;
                    varPtr->movedAt = scrutinee.span;
                    varPtr->movedTo = "match";
                    scrutinee.isResourceMove = true;
                }
            } else if (scrutinee.kind == ExprKind::Field) {
                failResourceCopy("No se puede mover parcialmente un campo de recurso en match.", scrutinee.span);
            }
        }
        const std::size_t declIdx = types.declIndices.at(type);
        const std::string concName = types.program.nameOf(type);
        const auto* app = types.program.typeAppOf(type);
        std::unordered_map<std::string, Span> seen;
        std::optional<Span> seenWildcard;
        bool returns = true;
        const auto beforeSnap = saveMoveState();
        bool allArmsEnded = true;
        bool anyFallsThrough = false;
        MoveSnapshot mergedSnap;
        Type unifiedType = Type::Unknown;
        Span firstValSpan{};
        for (auto& arm : arms) {
            restoreMoveState(beforeSnap);
            if (seenWildcard)
                fail("E_DUP_ARM", "El brazo '_' debe ir al final de match", arm.span,
                     "match s { _ => ... }", *seenWildcard);
            locals.levels.emplace_back();
            if (arm.variant == "_") {
                if (!arm.typeArgs.empty())
                    fail("E_GENERIC", "El patrón '_' no admite argumentos de tipo", arm.span, "_ => ...");
                if (!arm.names.empty())
                    fail("E_PATTERN_ARITY", "El patrón '_' no lleva paréntesis", arm.span, "_ => ...");
                seenWildcard = arm.span;
            } else {
                if (arm.variant == "Some" || arm.variant == "None") {
                    if (!types.program.isOption(type))
                        fail("E_MATCH_TYPE", "El patrón " + arm.variant + " requiere un tipo opcional T?", arm.span,
                             "match opt { Some(v) => ..., None => ... }", scrutinee.span);
                    arm.variant = concName + "." + arm.variant;
                } else if (arm.variant == "Ok" || arm.variant == "Err") {
                    if (!types.program.isResult(type))
                        fail("E_MATCH_TYPE", "El patrón " + arm.variant + " requiere Result<T, E>", arm.span,
                             "match res { Ok(v) => ..., Err(e) => ... }", scrutinee.span);
                    arm.variant = concName + "." + arm.variant;
                } else if (!arm.typeArgs.empty() || app) {
                    const auto dot = arm.variant.rfind('.');
                    const std::string armBase = dot == std::string::npos ? arm.variant : arm.variant.substr(0, dot);
                    const std::string armVar = dot == std::string::npos ? "" : arm.variant.substr(dot + 1);
                    if (armBase != "@Option" && armBase != "@Result") visible(armBase, arm.span);
                    if (!arm.typeArgs.empty()) {
                        for (Type ta : arm.typeArgs) known(ta, arm.span, false);
                        const Type armType = types.program.typeApp(armBase, arm.typeArgs);
                        requireType(armType, type, arm.span, scrutinee.span);
                        arm.variant = concName + "." + armVar;
                    } else if (app && armBase == app->baseName) {
                        arm.variant = concName + "." + armVar;
                    } else {
                        visible(arm.variant, arm.span);
                    }
                } else {
                    visible(arm.variant, arm.span);
                }
                const auto& c = types.constructor(arm.variant, arm.span);
                requireType(c.type, type, arm.span, scrutinee.span);
                const auto [prior, fresh] = seen.emplace(arm.variant, arm.span);
                if (!fresh) fail("E_DUP_ARM", "Brazo match repetido: " + arm.variant, arm.span,
                                 "match s { State.Ready => return }", prior->second);
                const auto& cVar = types.program.data[c.declIndex].variants[c.variantIndex];
                const auto& fields = cVar.fields;
                if (fields.size() != arm.names.size())
                    fail("E_PATTERN_ARITY", "Cantidad incorrecta de nombres en el patrón", arm.span,
                         "match s { State.Value(n) => return n }", cVar.span);
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    if (arm.names[i] != "_")
                        declare(arm.names[i], fields[i].type, false, arm.span, true);
                }
            }
            const bool savedTerm = pathTerminated;
            pathTerminated = false;
            if (!block(arm.body, false)) returns = false;
            if (isExpr) {
                if (arm.resultExpr) {
                    const Type expArm = expected != Type::Unknown ? expected : unifiedType;
                    Type armT = typeOf(*arm.resultExpr, ArrayUse::Copy, expArm);
                    if (!pathTerminated) {
                        requireValue(armT, arm.resultExpr->span);
                        if (expected != Type::Unknown) {
                            coerceIfDyn(*arm.resultExpr, armT, expected, arm.span);
                            requireType(armT, expected, arm.resultExpr->span, arm.span);
                        }
                        if (unifiedType == Type::Unknown) {
                            unifiedType = armT;
                            firstValSpan = arm.span;
                        } else {
                            coerceIfDyn(*arm.resultExpr, armT, unifiedType, arm.span);
                            requireType(armT, unifiedType, arm.resultExpr->span, firstValSpan);
                        }
                    }
                } else if (!pathTerminated) {
                    fail("E_MATCH_EXPR", "La rama de match como expresión debe devolver un valor o terminar en return/break/continue",
                         arm.span, "match x { A => 1, _ => 0 }");
                }
            }
            const bool armEnded = pathTerminated;
            pathTerminated = savedTerm;
            locals.levels.pop_back();
            if (!armEnded) {
                allArmsEnded = false;
                auto armSnap = saveMoveState();
                if (!anyFallsThrough) {
                    mergedSnap = std::move(armSnap);
                    anyFallsThrough = true;
                } else {
                    mergeMoveState(mergedSnap, armSnap);
                }
            }
        }
        if (!seenWildcard) {
            const auto prefix = concName + ".";
            for (const auto& variant : types.program.data.at(declIdx).variants) {
                if (!seen.contains(prefix + variant.name))
                    fail("E_EXHAUSTIVE", "Falta la variante " + prefix + variant.name, matchSpan,
                         "match s { State.Ready => return }", variant.span);
            }
        }
        if (anyFallsThrough) restoreMoveState(mergedSnap);
        else restoreMoveState(beforeSnap);
        if (allArmsEnded) pathTerminated = true;
        if (isExpr && outType) {
            if (unifiedType == Type::Unknown) {
                if (expected != Type::Unknown) unifiedType = expected;
                else fail("E_INFER_TYPE", "No se puede inferir el tipo de la expresión match porque todas sus ramas divergen; anota el tipo",
                          matchSpan, "let x: int = match ...");
            }
            *outType = unifiedType;
        }
        return returns;
    }

    bool match(Stmt& s) {
        return matchImpl(s.exprs[0], s.arms, s.span, false, Type::Unknown);
    }

    Type matchExpr(Expr& expr, Type expected) {
        Type out = Type::Unknown;
        (void)matchImpl(expr.matchExpr->scrutinee, expr.matchExpr->arms, expr.span, true, expected, &out);
        types.ensureInstantiated(out, expr.span);
        return out;
    }

    Type ifExpr(Expr& expr, Type expected) {
        IfExpr& node = *expr.ifExpr;
        requireType(typeOf(node.condition), Type::Bool, node.condition.span);
        const auto beforeSnap = saveMoveState();
        const bool savedTerm = pathTerminated;
        Type unifiedType = Type::Unknown;
        Span firstValSpan{};

        auto checkArm = [&](std::vector<Stmt>& body, std::optional<Expr>& resExpr, Span armSpan) -> std::pair<bool, MoveSnapshot> {
            locals.levels.emplace_back();
            pathTerminated = false;
            (void)block(body, false);
            if (resExpr) {
                const Type expArm = expected != Type::Unknown ? expected : unifiedType;
                Type armT = typeOf(*resExpr, ArrayUse::Copy, expArm);
                if (!pathTerminated) {
                    requireValue(armT, resExpr->span);
                    if (expected != Type::Unknown) {
                        coerceIfDyn(*resExpr, armT, expected, armSpan);
                        requireType(armT, expected, resExpr->span, armSpan);
                    }
                    if (unifiedType == Type::Unknown) {
                        unifiedType = armT;
                        firstValSpan = armSpan;
                    } else {
                        coerceIfDyn(*resExpr, armT, unifiedType, armSpan);
                        requireType(armT, unifiedType, resExpr->span, firstValSpan);
                    }
                }
            } else if (!pathTerminated) {
                fail("E_IF_EXPR", "La rama de if como expresión debe devolver un valor o terminar en return/break/continue",
                     armSpan, "let x = if cond { 1 } else { 2 }");
            }
            const bool armEnded = pathTerminated;
            locals.levels.pop_back();
            auto snap = saveMoveState();
            return {armEnded, std::move(snap)};
        };

        auto [thenEnded, thenSnap] = checkArm(node.thenBody, node.thenResult, expr.span);
        restoreMoveState(beforeSnap);
        auto [elseEnded, elseSnap] = checkArm(node.elseBody, node.elseResult, expr.span);
        pathTerminated = savedTerm || (thenEnded && elseEnded);

        if (!thenEnded && !elseEnded) {
            mergeMoveState(thenSnap, elseSnap);
            restoreMoveState(thenSnap);
        } else if (!thenEnded) {
            restoreMoveState(thenSnap);
        } else if (!elseEnded) {
            restoreMoveState(elseSnap);
        } else {
            restoreMoveState(beforeSnap);
        }

        if (unifiedType == Type::Unknown) {
            if (expected != Type::Unknown) unifiedType = expected;
            else fail("E_INFER_TYPE", "No se puede inferir el tipo de la expresión if porque ambas ramas divergen; anota el tipo",
                      expr.span, "let x: int = if cond { return 0 } else { return 1 }");
        }
        types.ensureInstantiated(unifiedType, expr.span);
        return unifiedType;
    }

    bool checkStatement(Stmt& s, Type initHint = Type::Unknown) {
        switch (s.kind) {
            case StmtKind::Let: case StmtKind::Var: {
                auto& init = s.exprs[0];
                if (s.annotation != Type::Unknown) known(s.annotation, s.span);
                const Type expectedInit = s.annotation != Type::Unknown ? s.annotation : initHint;
                DestGuard dg(activeMoveDest, s.name == "_" ? std::string("'_'") : ("'" + s.name + "'"));
                auto type = typeOf(init, ArrayUse::Copy, expectedInit);
                coerceIfDyn(init, type, expectedInit, s.span);
                requireValue(type, init.span);
                if (s.annotation != Type::Unknown) requireType(type, s.annotation, init.span, s.span);
                if (s.name != "_") {
                    auto& var = declare(s.name, type, s.kind == StmtKind::Var, s.span, true);
                    if (type == Type::IntArray && init.kind == ExprKind::Call && init.text == "repeat" &&
                        !types.program.isResource(type)) {
                        // Every use resolves its actual binding, including inside nested scopes.
                        // Any use beyond len/index invalidates the provisional region proof.
                        init.allocation = ArrayAllocation::Region;
                        var.regionSite = &init;
                    }
                }
                break;
            }
            case StmtKind::Assign: {
                const auto& curVar = lookup(s.name, s.span);
                if (curVar.isCaptured)
                    fail("E_CLOSURE_CAPTURE_MUT", "Las closures capturan '" + s.name +
                         "' por valor inmutable; crea 'var copia = " + s.name + "' dentro si necesitas mutarlo.",
                         s.span, "var copia = " + s.name, curVar.declared);
                if (!curVar.writable) {
                    Diagnostic d("E_IMMUTABLE", "El nombre es de solo lectura: " + s.name, s.span);
                    d.related.push_back(curVar.declared);
                    d.example = "var total = 0\ntotal = 1";
                    if (curVar.scalarParam) {
                        d.fix = Edit{{curVar.declared.start, curVar.declared.start}, "var "};
                        d.example = "fn next(var n: int) -> int { n += 1\nreturn n }";
                    }
                    throw d;
                }
                if (curVar.regionSite) curVar.regionSite->allocation = ArrayAllocation::Owner;
                const Type targetType = curVar.type;
                const Span declSpan = curVar.declared;
                Type rhsT = Type::Unknown;
                {
                    DestGuard dg(activeMoveDest, "'" + s.name + "'");
                    rhsT = typeOf(s.exprs[0], ArrayUse::Copy, targetType);
                }
                coerceIfDyn(s.exprs[0], rhsT, targetType, declSpan);
                requireType(rhsT, targetType, s.exprs[0].span, declSpan);
                auto& var = lookup(s.name, s.span);
                if (types.program.isResource(var.type)) {
                    var.moved = false;
                    var.movedAt = {};
                    var.movedTo.clear();
                }
                break;
            }
            case StmtKind::IndexSet: {
                auto& var = lookup(s.name, s.span);
                if (types.program.isMap(var.type) && !strictSyntax()) {
                    // `m[k] = v` es mapSet(mut m, k, v). `m[k] += d` llega aquí como `m[k] = m[k] + d`.
                    Expr target{ExprKind::Name, s.span, s.name, {}};
                    target.isMutArg = true;
                    Expr call{ExprKind::Call, s.span, "mapSet", {}};
                    call.args.push_back(std::move(target));
                    call.args.push_back(std::move(s.exprs[0]));
                    call.args.push_back(std::move(s.exprs[1]));
                    s.kind = StmtKind::Call;
                    s.exprs.clear();
                    s.exprs.push_back(std::move(call));
                    typeOf(s.exprs[0]);
                    break;
                }
                if (var.moved)
                    failUseAfterMove(s.name, var.type, s.span, var.movedAt, var.movedTo, var.declared);
                if (var.isCaptured)
                    fail("E_CLOSURE_CAPTURE_MUT", "Las closures capturan '" + s.name +
                         "' por valor inmutable; crea 'var copia = " + s.name + "' dentro si necesitas mutarlo.",
                         s.span, "var copia = " + s.name, var.declared);
                if (!var.writable)
                    fail("E_IMMUTABLE", "El contenedor es de solo lectura: " + s.name, s.span,
                         "var a = repeat(0, 4)\na[0] = 1", var.declared);
                if (types.program.isMap(var.type) || types.program.isPool(var.type)) {
                    const bool isP = types.program.isPool(var.type);
                    Diagnostic d(isP ? "E_POOL_INDEX" : "E_MAP_INDEX",
                                 isP ? "Usa poolSet(mut pool, asa, valor) para escribir en Pool<T>."
                                     : "Usa mapSet(mut mapa, clave, valor) para escribir en Map<K, V>.",
                                 s.span);
                    const auto src = types.program.sourceOf(s.span);
                    if (s.span.end <= src.size() &&
                        s.span.start <= s.exprs[0].span.start &&
                        s.exprs[0].span.end <= s.exprs[1].span.start &&
                        s.exprs[1].span.end <= s.span.end) {
                        const std::string keyText(src.substr(s.exprs[0].span.start, s.exprs[0].span.end - s.exprs[0].span.start));
                        const std::string valText(src.substr(s.exprs[1].span.start, s.exprs[1].span.end - s.exprs[1].span.start));
                        d.fix = Edit{s.span, std::string(isP ? "poolSet(mut " : "mapSet(mut ") + s.name + ", " + keyText + ", " + valText + ")"};
                    }
                    d.expected = "[T]";
                    d.actual = types.program.nameOf(var.type);
                    d.example = isP ? "poolSet(mut p, h, valor)" : "mapSet(mut m, clave, valor)";
                    throw d;
                }
                if (!types.program.isArray(var.type)) {
                    Diagnostic d("E_TYPE", "Se esperaba un array para asignar por índice", s.span);
                    d.expected = "[T]";
                    d.actual = types.program.nameOf(var.type);
                    d.related.push_back(var.declared);
                    throw d;
                }
                const Type elemT = types.program.elemOf(var.type);
                const Span declSpan = var.declared;
                requireType(typeOf(s.exprs[0]), Type::Int, s.exprs[0].span);
                DestGuard dg(activeMoveDest, "'" + s.name + "[...]'");
                Type rhsT = typeOf(s.exprs[1], ArrayUse::Copy, elemT);
                coerceIfDyn(s.exprs[1], rhsT, elemT, declSpan);
                requireType(rhsT, elemT, s.exprs[1].span, declSpan);
                break;
            }
            case StmtKind::FieldSet: {
                auto& var = lookup(s.name, s.span);
                if (var.moved)
                    failUseAfterMove(s.name, var.type, s.span, var.movedAt, var.movedTo, var.declared);
                if (var.isCaptured)
                    fail("E_CLOSURE_CAPTURE_MUT", "Las closures capturan '" + s.name +
                         "' por valor inmutable; crea 'var copia = " + s.name + "' dentro si necesitas mutarlo.",
                         s.span, "var copia = " + s.name, var.declared);
                if (!var.writable)
                    fail("E_IMMUTABLE", "El struct es de solo lectura: " + s.name, s.span,
                         "var p = Point { x: 1, y: 2 }\np.x = 3", var.declared);
                if (var.regionSite) var.regionSite->allocation = ArrayAllocation::Owner;
                Type cur = var.type;
                const Span declSpan = var.declared;
                s.fieldIndices.clear();
                s.fieldTypes.clear();
                for (const auto& fname : s.labels) {
                    if (!types.hasDecl(cur) || types.decl(cur).isEnum)
                        fail("E_FIELD", "La asignación por campo requiere un struct", s.span, "struct Point { x: int }");
                    const auto& variant = types.decl(cur).variants.at(0);
                    const auto idx = fieldIndex(variant, fname, s.span);
                    s.fieldIndices.push_back(idx);
                    cur = variant.fields[idx].type;
                    s.fieldTypes.push_back(cur);
                }
                if (s.exprs.size() == 2) {
                    if (!types.program.isArray(cur)) {
                        Diagnostic d("E_TYPE", "Se esperaba un array para asignación indexada", s.span);
                        d.expected = "[T]";
                        d.actual = types.program.nameOf(cur);
                        throw d;
                    }
                    requireType(typeOf(s.exprs[0]), Type::Int, s.exprs[0].span);
                    const Type elemT = types.program.elemOf(cur);
                    DestGuard dg(activeMoveDest, "'" + s.name + "." + s.labels.back() + "[...]'");
                    Type rhsT = typeOf(s.exprs[1], ArrayUse::Copy, elemT);
                    coerceIfDyn(s.exprs[1], rhsT, elemT, declSpan);
                    requireType(rhsT, elemT, s.exprs[1].span, declSpan);
                } else {
                    DestGuard dg(activeMoveDest, "'" + s.name + "." + s.labels.back() + "'");
                    Type rhsT = typeOf(s.exprs[0], ArrayUse::Copy, cur);
                    coerceIfDyn(s.exprs[0], rhsT, cur, declSpan);
                    requireType(rhsT, cur, s.exprs[0].span, declSpan);
                }
                break;
            }
            case StmtKind::PlaceSet: {
                auto& var = lookup(s.name, s.span);
                if (var.moved)
                    failUseAfterMove(s.name, var.type, s.span, var.movedAt, var.movedTo, var.declared);
                if (var.isCaptured)
                    fail("E_CLOSURE_CAPTURE_MUT", "Las closures capturan '" + s.name +
                         "' por valor inmutable; crea 'var copia = " + s.name + "' dentro si necesitas mutarlo.",
                         s.span, "var copia = " + s.name, var.declared);
                if (!var.writable)
                    fail("E_IMMUTABLE", "El valor es de solo lectura: " + s.name, s.span,
                         "var a = ...\na[0] = ...", var.declared);
                if (var.regionSite) var.regionSite->allocation = ArrayAllocation::Owner;
                Type cur = var.type;
                const Span declSpan = var.declared;
                std::size_t exprIdx = 0;
                for (auto& step : s.placeSteps) {
                    if (step.isIndex) {
                        if (types.program.isMap(cur) || types.program.isPool(cur)) {
                            const bool isP = types.program.isPool(cur);
                            fail(isP ? "E_POOL_INDEX" : "E_MAP_INDEX",
                                 isP ? "Usa poolSet(mut pool, asa, valor) para escribir en Pool<T>."
                                     : "Usa mapSet(mut mapa, clave, valor) para escribir en Map<K, V>.",
                                 s.span, isP ? "poolSet(mut p, h, valor)" : "mapSet(mut m, clave, valor)");
                        }
                        if (!types.program.isArray(cur)) {
                            Diagnostic d("E_TYPE", "Se esperaba un array para asignar por índice", s.span);
                            d.expected = "[T]";
                            d.actual = types.program.nameOf(cur);
                            d.related.push_back(declSpan);
                            throw d;
                        }
                        requireType(typeOf(s.exprs[exprIdx]), Type::Int, s.exprs[exprIdx].span);
                        ++exprIdx;
                        cur = types.program.elemOf(cur);
                        step.resultType = cur;
                    } else {
                        if (!types.hasDecl(cur) || types.decl(cur).isEnum)
                            fail("E_FIELD", "La asignación por campo requiere un struct", s.span, "struct Point { x: int }");
                        const auto& variant = types.decl(cur).variants.at(0);
                        const auto idx = fieldIndex(variant, step.field, s.span);
                        step.fieldIdx = idx;
                        cur = variant.fields[idx].type;
                        step.resultType = cur;
                    }
                }
                DestGuard dg(activeMoveDest, "'" + s.name + "...'");
                Type rhsT = typeOf(s.exprs[exprIdx], ArrayUse::Copy, cur);
                coerceIfDyn(s.exprs[exprIdx], rhsT, cur, declSpan);
                requireType(rhsT, cur, s.exprs[exprIdx].span, declSpan);
                break;
            }
            case StmtKind::Call: typeOf(s.exprs[0]); break;
            case StmtKind::If: {
                requireType(typeOf(s.exprs[0]), Type::Bool, s.exprs[0].span);
                const auto beforeSnap = saveMoveState();
                const bool savedTerm = pathTerminated;
                pathTerminated = false;
                const bool thenReturns = block(s.body);
                const bool thenEnded = pathTerminated;
                auto thenSnap = saveMoveState();
                restoreMoveState(beforeSnap);
                pathTerminated = false;
                const bool elseReturns = block(s.otherwise);
                const bool elseEnded = pathTerminated;
                auto elseSnap = saveMoveState();
                if (!thenEnded && !elseEnded) {
                    mergeMoveState(thenSnap, elseSnap);
                    restoreMoveState(thenSnap);
                } else if (!thenEnded) {
                    restoreMoveState(thenSnap);
                } else if (!elseEnded) {
                    restoreMoveState(elseSnap);
                } else {
                    restoreMoveState(beforeSnap);
                }
                pathTerminated = savedTerm || (thenEnded && elseEnded);
                return thenReturns && elseReturns;
            }
            case StmtKind::SafetyBlock: {
                const SafetyTier savedSafety = activeSafety;
                activeSafety = s.safety;
                bool ret = false;
                try {
                    ret = block(s.body);
                } catch (...) {
                    activeSafety = savedSafety;
                    throw;
                }
                activeSafety = savedSafety;
                return ret;
            }
            case StmtKind::While: {
                const auto beforeCond = saveMoveState();
                requireType(typeOf(s.exprs[0]), Type::Bool, s.exprs[0].span);
                checkLoopBackEdgeMoves(beforeCond, s.exprs[0].span);
                if (!s.label.empty()) {
                    for (const auto& lc : loopStack)
                        if (lc.label == s.label) fail("E_DUP_LABEL", "Etiqueta de bucle duplicada: '" + s.label, s.span, "'outer: while ...");
                }
                loopStack.push_back({s.label, false});
                loopMoveFrames.push_back({s.label, beforeCond, {}});
                const bool savedTerm = pathTerminated;
                pathTerminated = false;
                block(s.body);
                if (!pathTerminated) checkLoopBackEdgeMoves(beforeCond, s.span);
                auto exitSnap = beforeCond;
                for (const auto& brk : loopMoveFrames.back().breaks) mergeMoveState(exitSnap, brk);
                restoreMoveState(exitSnap);
                loopMoveFrames.pop_back();
                loopStack.pop_back();
                pathTerminated = savedTerm;
                break;
            }
            case StmtKind::For: {
                for (auto& bound : s.exprs) requireType(typeOf(bound), Type::Int, bound.span);
                if (!s.label.empty()) {
                    for (const auto& lc : loopStack)
                        if (lc.label == s.label) fail("E_DUP_LABEL", "Etiqueta de bucle duplicada: '" + s.label, s.span, "'outer: for ...");
                }
                const auto beforeLoop = saveMoveState();
                locals.levels.emplace_back();
                if (s.name != "_") declare(s.name, Type::Int, false, s.span, true);
                loopStack.push_back({s.label, true});
                loopMoveFrames.push_back({s.label, beforeLoop, {}});
                const bool savedTerm = pathTerminated;
                pathTerminated = false;
                block(s.body, false);
                if (!pathTerminated) checkLoopBackEdgeMoves(beforeLoop, s.span);
                auto exitSnap = beforeLoop;
                for (const auto& brk : loopMoveFrames.back().breaks) mergeMoveState(exitSnap, brk);
                locals.levels.pop_back();
                restoreMoveState(exitSnap);
                loopMoveFrames.pop_back();
                loopStack.pop_back();
                pathTerminated = savedTerm;
                break;
            }
            case StmtKind::ForEach: {
                const Type collT = typeOf(s.exprs[0], ArrayUse::Inspect);
                if (collT == Type::Str && !strictSyntax() && s.exprs[0].kind == ExprKind::Name && s.name != "_") {
                    // `for c in s` recorre los caracteres de un str (cada `c` es un str de un carácter):
                    //   for c__i in 0..len(s) { let c = s[c__i..c__i + 1]; ... }
                    const Span span = s.exprs[0].span;
                    const std::string index = s.name + "__i";
                    Expr text = s.exprs[0];
                    Expr length{ExprKind::Call, span, "len", {}};
                    length.args.push_back(text);
                    Expr upper{ExprKind::Binary, span, "+", {}};
                    upper.args.push_back(Expr{ExprKind::Name, span, index, {}});
                    upper.args.push_back(Expr{ExprKind::Integer, span, "1", {}});
                    Expr slice{ExprKind::Slice, span, {}, {}};
                    slice.args.push_back(std::move(text));
                    slice.args.push_back(Expr{ExprKind::Name, span, index, {}});
                    slice.args.push_back(std::move(upper));
                    Stmt bind{};
                    bind.kind = StmtKind::Let;
                    bind.span = span;
                    bind.name = s.name;
                    bind.exprs.push_back(std::move(slice));
                    s.kind = StmtKind::For;
                    s.name = index;
                    s.exprs.clear();
                    s.exprs.push_back(Expr{ExprKind::Integer, span, "0", {}});
                    s.exprs.push_back(std::move(length));
                    s.body.insert(s.body.begin(), std::move(bind));
                    return checkStatement(s, initHint);
                }
                if (!types.program.isArray(collT)) {
                    Diagnostic d("E_TYPE", "for x in ... requiere un array", s.exprs[0].span);
                    d.expected = "[T]";
                    d.actual = types.program.nameOf(collT);
                    throw d;
                }
                const Type elemT = types.program.elemOf(collT);
                if (types.program.isResource(elemT))
                    failResourceCopy("No se puede iterar con 'for x in ...' sobre un arreglo de recursos no copiables ('" +
                                     types.program.nameOf(elemT) + "'); usa un bucle por índice.",
                                     s.exprs[0].span);
                if (!s.label.empty()) {
                    for (const auto& lc : loopStack)
                        if (lc.label == s.label) fail("E_DUP_LABEL", "Etiqueta de bucle duplicada: '" + s.label, s.span, "'outer: for ...");
                }
                const auto beforeLoop = saveMoveState();
                locals.levels.emplace_back();
                if (s.name != "_") declare(s.name, elemT, false, s.span, true);
                loopStack.push_back({s.label, true});
                loopMoveFrames.push_back({s.label, beforeLoop, {}});
                const bool savedTerm = pathTerminated;
                pathTerminated = false;
                block(s.body, false);
                if (!pathTerminated) checkLoopBackEdgeMoves(beforeLoop, s.span);
                auto exitSnap = beforeLoop;
                for (const auto& brk : loopMoveFrames.back().breaks) mergeMoveState(exitSnap, brk);
                locals.levels.pop_back();
                restoreMoveState(exitSnap);
                loopMoveFrames.pop_back();
                loopStack.pop_back();
                pathTerminated = savedTerm;
                break;
            }
            case StmtKind::Break: {
                if (loopStack.empty())
                    fail("E_LOOP_CONTROL", "break/continue solo puede usarse dentro de un bucle", s.span, "while cond { break }");
                if (!s.label.empty()) {
                    const bool found = std::any_of(loopStack.begin(), loopStack.end(),
                                                   [&](const LoopCtx& lc) { return lc.label == s.label; });
                    if (!found)
                        fail("E_LOOP_CONTROL", "Etiqueta de bucle desconocida: '" + s.label, s.span, "'outer: for i in 0..n { break 'outer }");
                }
                findLoopMoveFrame(s.label).breaks.push_back(saveMoveState());
                pathTerminated = true;
                break;
            }
            case StmtKind::Continue: {
                if (loopStack.empty())
                    fail("E_LOOP_CONTROL", "break/continue solo puede usarse dentro de un bucle", s.span, "while cond { break }");
                if (!s.label.empty()) {
                    const bool found = std::any_of(loopStack.begin(), loopStack.end(),
                                                   [&](const LoopCtx& lc) { return lc.label == s.label; });
                    if (!found)
                        fail("E_LOOP_CONTROL", "Etiqueta de bucle desconocida: '" + s.label, s.span, "'outer: for i in 0..n { break 'outer }");
                }
                checkLoopBackEdgeMoves(findLoopMoveFrame(s.label).entry, s.span);
                pathTerminated = true;
                break;
            }
            case StmtKind::Return:
                if (inferredReturn && current.result == Type::Unknown) {
                    if (s.exprs.empty()) {
                        if (*inferredReturn == Type::Unknown) *inferredReturn = Type::Void;
                        else requireType(Type::Void, *inferredReturn, s.span, current.span);
                    } else {
                        DestGuard dg(activeMoveDest, "return");
                        auto type = typeOf(s.exprs[0], ArrayUse::Copy, *inferredReturn);
                        if (*inferredReturn != Type::Unknown) coerceIfDyn(s.exprs[0], type, *inferredReturn, current.span);
                        requireValue(type, s.exprs[0].span);
                        if (*inferredReturn == Type::Unknown) *inferredReturn = type;
                        else requireType(type, *inferredReturn, s.exprs[0].span, current.span);
                    }
                    pathTerminated = true;
                    return true;
                }
                if (s.exprs.empty()) requireType(Type::Void, current.result, s.span, current.span);
                else {
                    DestGuard dg(activeMoveDest, "return");
                    auto type = typeOf(s.exprs[0], ArrayUse::Copy, current.result);
                    coerceIfDyn(s.exprs[0], type, current.result, current.span);
                    requireValue(type, s.exprs[0].span);
                    requireType(type, current.result, s.exprs[0].span, current.span);
                }
                pathTerminated = true;
                return true;
            case StmtKind::Assert: requireType(typeOf(s.exprs[0]), Type::Bool, s.exprs[0].span); break;
            case StmtKind::Print: {
                const auto type = typeOf(s.exprs[0]);
                requireValue(type, s.exprs[0].span);
                if (type >= Type::NamedBase || types.program.isArray(type) ||
                    type == Type::StringBuilder || Program::isPrimitiveResource(type))
                    fail("E_PRINT", "print admite int, float, bool o str", s.span, "print(42)");
                break;
            }
            case StmtKind::Match: return match(s);
        }
        return false;
    }
};

// --- Mutabilidad inferida -------------------------------------------------------------------
// `let` frente a `var` y los parámetros de solo lectura son la segunda causa de error de los modelos
// (JS/TS reasignan `let`; casi todos los lenguajes dejan reasignar un parámetro). El compilador
// puede deducirlo: si el programa modifica un `let` o un parámetro por valor, esa declaración pasa
// a ser `var` (copia local mutable, la semántica de valor de siempre) y se vuelve a comprobar.
bool sameSpan(const Span& a, const Span& b) {
    return a.start == b.start && a.end == b.end && a.file == b.file;
}
bool promoteInStmts(std::vector<Stmt>& stmts, const Span& declared);
bool promoteParams(std::vector<Param>& params, const Span& declared) {
    for (auto& p : params) {
        if (sameSpan(p.span, declared) && !p.byRef && !p.writable && p.name != "self") {
            p.writable = true;
            return true;
        }
    }
    return false;
}
bool promoteInExpr(Expr& e, const Span& declared) {
    for (auto& a : e.args)
        if (promoteInExpr(a, declared)) return true;
    if (e.closure) {
        if (promoteParams(e.closure->params, declared)) return true;
        if (promoteInStmts(e.closure->body, declared)) return true;
    }
    if (e.matchExpr) {
        if (promoteInExpr(e.matchExpr->scrutinee, declared)) return true;
        for (auto& arm : e.matchExpr->arms) {
            if (promoteInStmts(arm.body, declared)) return true;
            if (arm.resultExpr && promoteInExpr(*arm.resultExpr, declared)) return true;
        }
    }
    if (e.ifExpr) {
        if (promoteInExpr(e.ifExpr->condition, declared)) return true;
        if (promoteInStmts(e.ifExpr->thenBody, declared) || promoteInStmts(e.ifExpr->elseBody, declared)) return true;
        if (e.ifExpr->thenResult && promoteInExpr(*e.ifExpr->thenResult, declared)) return true;
        if (e.ifExpr->elseResult && promoteInExpr(*e.ifExpr->elseResult, declared)) return true;
    }
    return false;
}
bool promoteInStmts(std::vector<Stmt>& stmts, const Span& declared) {
    for (auto& st : stmts) {
        if (st.kind == StmtKind::Let && st.name != "_" && sameSpan(st.span, declared)) {
            st.kind = StmtKind::Var;
            return true;
        }
        for (auto& e : st.exprs)
            if (promoteInExpr(e, declared)) return true;
        if (promoteInStmts(st.body, declared) || promoteInStmts(st.otherwise, declared)) return true;
        for (auto& arm : st.arms) {
            if (promoteInStmts(arm.body, declared)) return true;
            if (arm.resultExpr && promoteInExpr(*arm.resultExpr, declared)) return true;
        }
    }
    return false;
}
void renameInStmts(std::vector<Stmt>& stmts, const std::string& from, const std::string& to);
void renameInExpr(Expr& e, const std::string& from, const std::string& to) {
    if (e.kind == ExprKind::Name && e.text == from) e.text = to;
    for (auto& a : e.args) renameInExpr(a, from, to);
    if (e.matchExpr) {
        renameInExpr(e.matchExpr->scrutinee, from, to);
        for (auto& arm : e.matchExpr->arms) {
            renameInStmts(arm.body, from, to);
            if (arm.resultExpr) renameInExpr(*arm.resultExpr, from, to);
        }
    }
    if (e.ifExpr) {
        renameInExpr(e.ifExpr->condition, from, to);
        renameInStmts(e.ifExpr->thenBody, from, to);
        renameInStmts(e.ifExpr->elseBody, from, to);
        if (e.ifExpr->thenResult) renameInExpr(*e.ifExpr->thenResult, from, to);
        if (e.ifExpr->elseResult) renameInExpr(*e.ifExpr->elseResult, from, to);
    }
}
void renameInStmts(std::vector<Stmt>& stmts, const std::string& from, const std::string& to) {
    for (auto& st : stmts) {
        for (auto& e : st.exprs) renameInExpr(e, from, to);
        renameInStmts(st.body, from, to);
        renameInStmts(st.otherwise, from, to);
        for (auto& arm : st.arms) {
            renameInStmts(arm.body, from, to);
            if (arm.resultExpr) renameInExpr(*arm.resultExpr, from, to);
        }
    }
}
constexpr std::string_view kParamCopySuffix = "__in";
bool promoteInFunction(Function& fn, const Span& declared) {
    if (fn.isExternDecl) return false;
    for (auto& p : fn.params) {
        if (!sameSpan(p.span, declared) || p.byRef || p.writable || p.name.ends_with(kParamCopySuffix)) continue;
        if (p.name == "self") {
            // Un método que modifica `self` quiere modificar al receptor: equivale a `mut self`.
            p.writable = true;
            p.byRef = true;
            return true;
        }
        if (Program::isNumericType(p.type) || p.type == Type::Bool) {
            p.writable = true;
            return true;
        }
        // Agregado por valor (arreglo, str, struct...): copia local mutable con el mismo nombre.
        // La semántica de valor se conserva: el argumento de quien llama no cambia.
        const std::string outer = p.name;
        const std::string inner = outer + std::string(kParamCopySuffix);
        for (auto& clause : fn.requiresClauses) renameInExpr(clause, outer, inner);
        Stmt copy{};
        copy.kind = StmtKind::Var;
        copy.span = p.span;
        copy.name = outer;
        copy.exprs.push_back(Expr{ExprKind::Name, p.span, inner, {}});
        p.name = inner;
        fn.body.insert(fn.body.begin(), std::move(copy));
        return true;
    }
    return promoteInStmts(fn.body, declared);
}
bool promoteToMutable(Program& program, const Span& declared) {
    for (auto& fn : program.functions)
        if (promoteInFunction(fn, declared)) return true;
    for (auto& impl : program.impls)
        for (auto& fn : impl.methods)
            if (promoteInFunction(fn, declared)) return true;
    for (auto& iface : program.interfaces)
        for (auto& fn : iface.methods)
            if (promoteInFunction(fn, declared)) return true;
    return false;
}

void checkOnce(Program& program);
std::size_t boxRecursiveFields(Program& program, std::vector<std::string>* targets = nullptr);

} // namespace

std::vector<std::string> recursiveTypeNames(const Program& program) {
    std::vector<std::string> names;
    if (strictSyntax()) return names;
    Program copy = program;
    (void)boxRecursiveFields(copy, &names);
    return names;
}

void check(Program& program) {
    program.cellRegionCache.reset(); // el análisis de regiones se recalcula sobre el programa comprobado
    if (strictSyntax()) {
        checkOnce(program);
        return;
    }
    for (unsigned round = 0;; ++round) {
        Program attempt = program;
        // El diagnóstico se captura y se vuelve a lanzar por copia (como en el resto del compilador):
        // un `throw;` desde el `catch` falla con ASan en Windows.
        auto failure = captureDiagnostic([&] { checkOnce(attempt); });
        if (!failure) {
            program = std::move(attempt);
            return;
        }
        if (round >= 1024 || failure->id != "E_IMMUTABLE" || failure->related.empty() ||
            !promoteToMutable(program, failure->related.front()))
            throw *failure;
    }
}

namespace {
// Tipos recursivos directos. `struct Node { left: Node?, right: Node? }` o
// `enum Expr { Num(v: int), Add(l: Expr, r: Expr) }` tendrían tamaño infinito por valor. El campo que
// cierra el ciclo se guarda en una celda del montón (Shared<T>): el programador escribe `Node` y el
// compilador mete y saca el valor de la celda en cada uso. La semántica sigue siendo de valor.
std::size_t boxRecursiveFields(Program& program, std::vector<std::string>* targets) {
    std::size_t boxed = 0;
    const std::size_t n = program.data.size();
    std::map<std::string, std::size_t> byName;
    for (std::size_t i = 0; i < n; ++i) byName.emplace(program.nameOf(program.data[i].type), i);
    const auto targetOf = [&](Type fieldType) -> std::optional<std::size_t> {
        Type t = fieldType;
        if (program.isOption(t)) t = program.optionInner(t);
        std::string name;
        if (const auto* app = program.typeAppOf(t)) {
            if (app->baseName.starts_with("@")) return std::nullopt;
            name = app->baseName;
        } else {
            name = program.nameOf(t);
        }
        if (auto it = byName.find(name); it != byName.end()) return it->second;
        return std::nullopt;
    };
    std::vector<std::vector<std::size_t>> edges(n);
    for (std::size_t i = 0; i < n; ++i)
        for (const auto& v : program.data[i].variants)
            for (const auto& f : v.fields)
                if (auto to = targetOf(f.type)) edges[i].push_back(*to);
    const auto reaches = [&](std::size_t from, std::size_t goal) {
        std::vector<bool> seen(n, false);
        std::vector<std::size_t> stack{from};
        while (!stack.empty()) {
            const std::size_t at = stack.back();
            stack.pop_back();
            if (at == goal) return true;
            if (seen[at]) continue;
            seen[at] = true;
            for (std::size_t next : edges[at]) stack.push_back(next);
        }
        return false;
    };
    // Un tipo que se contiene a sí mismo sin salida (`struct Loop { next: Loop }`) no se puede
    // construir nunca: ese sigue siendo un error (E_TYPE_CYCLE), no se mete en celda. Un tipo se
    // puede construir si alguna variante tiene todos sus campos construibles; `T?` siempre lo es.
    std::vector<bool> buildable(n, false);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (buildable[i]) continue;
            for (const auto& v : program.data[i].variants) {
                bool ok = true;
                for (const auto& f : v.fields) {
                    if (program.isOption(f.type)) continue;
                    const auto to = targetOf(f.type);
                    if (to && !buildable[*to]) { ok = false; break; }
                }
                if (ok) { buildable[i] = true; changed = true; break; }
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!buildable[i]) continue;
        for (auto& v : program.data[i].variants) {
            for (auto& f : v.fields) {
                const auto to = targetOf(f.type);
                if (!to || !reaches(*to, i)) continue;
                if (program.isOption(f.type)) f.type = program.optionOf(program.sharedOf(program.optionInner(f.type)));
                else f.type = program.sharedOf(f.type);
                ++boxed;
                if (targets && program.data[*to].generics.empty()) {
                    const std::string name = program.nameOf(program.data[*to].type);
                    if (std::find(targets->begin(), targets->end(), name) == targets->end()) targets->push_back(name);
                }
            }
        }
    }
    return boxed;
}

void checkOnce(Program& program) {
    if (!strictSyntax()) (void)boxRecursiveFields(program);
    for (const auto& f : program.functions) {
        if (!f.isTest && (f.sourceName == "repeat" || f.sourceName == "readByte"))
            fail("E_RESERVED", "Nombre reservado: " + f.name, f.span, "fn count() -> int { return 3 }");
    }

    // Check and evaluate global constants (allowing cross-module and acyclic forward references)
    std::unordered_map<std::string, std::size_t> constIdx;
    for (std::size_t i = 0; i < program.consts.size(); ++i) {
        const auto& c = program.consts[i];
        const auto [it, added] = constIdx.emplace(c.name, i);
        if (!added) fail("E_DUP_CONST", "Constante duplicada: " + c.name, c.span, "const kVal: int = 1",
                         program.consts[it->second].span);
    }
    for (const auto& f : program.functions) {
        if (!f.isTest && constIdx.contains(f.name))
            fail("E_DUP_CONST", "El nombre ya está usado por una constante: " + f.name, f.span, "fn other() {}",
                 program.consts[constIdx.at(f.name)].span);
    }

    for (const auto& d : program.data) {
        if (!d.isEnum) continue;
        const bool hasPayloads = std::any_of(d.variants.begin(), d.variants.end(),
                                             [](const Variant& v) { return !v.fields.empty(); });
        if (d.hasExplicitEnumValues && hasPayloads) {
            for (const auto& v : d.variants)
                if (v.enumValueExpr)
                    fail("E_ENUM_VALUE", "Los enums con campos no admiten valores enteros en sus variantes", v.span, "enum Kind { A = 1, B = 2 }");
        }
    }

    Types types(program);
    for (std::size_t di = 0; di < program.data.size(); ++di) {
        const std::size_t vCount = program.data[di].variants.size();
        for (std::size_t vi = 0; vi < vCount; ++vi) {
            const std::size_t fCount = program.data[di].variants[vi].fields.size();
            for (std::size_t fi = 0; fi < fCount; ++fi) {
                const Type ft = program.data[di].variants[vi].fields[fi].type;
                const Span fs = program.data[di].variants[vi].fields[fi].span;
                types.known(ft, fs, false);
            }
        }
    }
    std::unordered_map<Type, std::size_t> sizes;
    Layouts layouts{types, {}, {}};
    for (const auto& d : program.data) sizes.emplace(d.type, layouts.measure(d.type, d.span).bytes);

    struct ConstVal {
        Type type;
        Integer i = 0;
        double f = 0.0;
        bool b = false;
        std::string s{};
        std::uint64_t u = 0;
    };
    auto makeConstInt = [](Type t, std::uint64_t rawBits) -> ConstVal {
        const std::uint64_t masked = rawBits & maskForType(t);
        const Integer signedVal = Program::isSignedInt(t)
            ? signExtendWidth(masked, Program::intBitWidth(t))
            : static_cast<Integer>(masked);
        return {t, signedVal, 0.0, false, {}, masked};
    };
    auto evalConstCast = [&](const ConstVal& v, Type targetT, Span span) -> ConstVal {
        if (v.type == targetT) return v;
        if (Program::isFloatType(v.type)) {
            if (targetT == Type::Float) return {Type::Float, 0, v.f};
            if (targetT == Type::F32) return {Type::F32, 0, static_cast<double>(static_cast<float>(v.f))};
            if (Program::isSignedInt(targetT)) {
                const unsigned w = Program::intBitWidth(targetT);
                if (!std::isfinite(v.f)) fail("R_OVERFLOW", "Desbordamiento en constante", span, "1");
                if (w == 64) {
                    if (v.f >= 9223372036854775808.0 || v.f < -9223372036854775808.0)
                        fail("R_OVERFLOW", "Desbordamiento en constante", span, "1");
                    return makeConstInt(targetT, static_cast<std::uint64_t>(static_cast<Integer>(v.f)));
                }
                const double lo = static_cast<double>(minSignedForWidth(w)) - 1.0;
                const double hi = static_cast<double>(maxSignedForWidth(w)) + 1.0;
                if (v.f >= hi || v.f <= lo) fail("R_OVERFLOW", "Desbordamiento en constante", span, "1");
                return makeConstInt(targetT, static_cast<std::uint64_t>(static_cast<Integer>(v.f)));
            }
            const unsigned w = Program::intBitWidth(targetT);
            if (!std::isfinite(v.f) || v.f < 0.0) fail("R_OVERFLOW", "Desbordamiento en constante", span, "1");
            const double hi = (w == 64) ? 18446744073709551616.0 : (static_cast<double>(maskForWidth(w)) + 1.0);
            if (v.f >= hi) fail("R_OVERFLOW", "Desbordamiento en constante", span, "1");
            return makeConstInt(targetT, static_cast<std::uint64_t>(v.f));
        }
        if (Program::isIntType(v.type)) {
            const unsigned fromW = Program::intBitWidth(v.type);
            if (Program::isFloatType(targetT)) {
                const double d = Program::isSignedInt(v.type)
                    ? static_cast<double>(signExtendWidth(v.u, fromW))
                    : static_cast<double>(v.u);
                return {targetT, 0, targetT == Type::F32 ? static_cast<double>(static_cast<float>(d)) : d};
            }
            const unsigned toW = Program::intBitWidth(targetT);
            if (toW <= fromW) return makeConstInt(targetT, v.u);
            if (Program::isSignedInt(v.type))
                return makeConstInt(targetT, static_cast<std::uint64_t>(signExtendWidth(v.u, fromW)));
            return makeConstInt(targetT, v.u);
        }
        fail("E_TYPE", "Tipo no convertible en constante", span, "const kA: int = 1");
    };
    auto isValidConstType = [](Type t) {
        return Program::isNumericType(t) || t == Type::Bool || t == Type::Str;
    };

    std::vector<int> constState(program.consts.size(), 0); // 0=unvisited, 1=visiting, 2=done
    auto evalConst = [&](auto& self, Expr& e, const std::string& inModule, Type expected) -> ConstVal {
        switch (e.kind) {
            case ExprKind::Integer: {
                const Type target = (expected != Type::Unknown && Program::isIntType(expected)) ? expected : Type::Int;
                const std::uint64_t bits = parseExactIntLiteral(program, e, target, false);
                e.type = target;
                return makeConstInt(target, bits);
            }
            case ExprKind::Float: {
                const Type target = (expected == Type::F32) ? Type::F32 : Type::Float;
                const double fv = parseExactFloatLiteral(e, target);
                e.type = target;
                return {target, 0, fv};
            }
            case ExprKind::Boolean: e.type = Type::Bool; return {Type::Bool, 0, 0.0, e.text == "true"};
            case ExprKind::String: e.type = Type::Str; return {Type::Str, 0, 0.0, false, e.text};
            case ExprKind::Name: {
                const std::string qname = inModule.empty() ? e.text : inModule + "." + e.text;
                auto it = constIdx.find(qname);
                if (it == constIdx.end())
                    fail("E_NAME", "Constante desconocida o no constante: " + e.text, e.span, "const kA: int = 1");
                const std::size_t idx = it->second;
                if (constState[idx] == 1)
                    fail("E_CONST_CYCLE", "Dependencia circular en constante: " + program.consts[idx].name, e.span, "const kA: int = 1");
                if (constState[idx] == 0) {
                    constState[idx] = 1;
                    auto& cd = program.consts[idx];
                    if (!isValidConstType(cd.type))
                        fail("E_TYPE", "const admite tipos numéricos, bool o str", cd.span, "const kMax: int = 10");
                    ConstVal v = self(self, cd.init, cd.module, cd.type);
                    checkTypes(program, v.type, cd.type, cd.init.span, cd.span);
                    cd.intVal = v.i; cd.uintVal = v.u; cd.floatVal = v.f; cd.boolVal = v.b; cd.strVal = v.s;
                    constState[idx] = 2;
                }
                const auto& resolved = program.consts[idx];
                e.type = resolved.type;
                return {resolved.type, resolved.intVal, resolved.floatVal, resolved.boolVal, resolved.strVal, resolved.uintVal};
            }
            case ExprKind::Field: {
                if (e.args.size() != 1 || e.args[0].kind != ExprKind::Name)
                    fail("E_VALUE", "Expresión no constante", e.span, "const kA: int = 1");
                const std::string qname = e.args[0].text + "." + e.text;
                if (!program.visible(inModule, qname))
                    fail("E_IMPORT", "Constante de módulo no importado: " + qname, e.span, "import defs");
                auto it = constIdx.find(qname);
                if (it == constIdx.end())
                    fail("E_NAME", "Constante desconocida: " + qname, e.span, "const kA: int = 1");
                const std::size_t idx = it->second;
                if (constState[idx] == 1)
                    fail("E_CONST_CYCLE", "Dependencia circular en constante: " + qname, e.span, "const kA: int = 1");
                if (constState[idx] == 0) {
                    constState[idx] = 1;
                    auto& cd = program.consts[idx];
                    if (!isValidConstType(cd.type))
                        fail("E_TYPE", "const admite tipos numéricos, bool o str", cd.span, "const kMax: int = 10");
                    ConstVal v = self(self, cd.init, cd.module, cd.type);
                    checkTypes(program, v.type, cd.type, cd.init.span, cd.span);
                    cd.intVal = v.i; cd.uintVal = v.u; cd.floatVal = v.f; cd.boolVal = v.b; cd.strVal = v.s;
                    constState[idx] = 2;
                }
                const auto& resolved = program.consts[idx];
                e.type = resolved.type;
                return {resolved.type, resolved.intVal, resolved.floatVal, resolved.boolVal, resolved.strVal, resolved.uintVal};
            }
            case ExprKind::Call: {
                if (e.text == "sizeOf" || e.text == "alignOf") {
                    if (!e.args.empty() || e.typeArgs.size() != 1)
                        fail("E_GENERIC", e.text + " requiere 0 argumentos y <T>", e.span, e.text + "<u32>()");
                    const Type targetT = e.typeArgs[0];
                    checkTypeVisible(program, inModule, targetT, e.span);
                    types.known(targetT, e.span, false);
                    sizes.try_emplace(targetT, layouts.measure(targetT, e.span).bytes);
                    LayoutCalculator lc(program);
                    const auto [sz, al] = lc.sizeAlignOf(targetT);
                    e.type = Type::Int;
                    return makeConstInt(Type::Int, static_cast<std::uint64_t>(e.text == "sizeOf" ? sz : al));
                }
                if (auto targetNum = numericBuiltinType(e.text)) {
                    if (e.args.size() != 1 || !e.typeArgs.empty())
                        fail("E_ARITY", "Cantidad incorrecta de argumentos de " + e.text, e.span, e.text + "(0)");
                    ConstVal argV = self(self, e.args[0], inModule, numericLiteralHint(*targetNum, e.args[0]));
                    e.type = *targetNum;
                    return evalConstCast(argV, *targetNum, e.span);
                }
                fail("E_VALUE", "Una constante requiere una expresión constante", e.span, "const kA: int = 1");
            }
            case ExprKind::Unary: {
                if (e.text == "!") {
                    ConstVal v = self(self, e.args[0], inModule, Type::Bool);
                    checkTypes(program, v.type, Type::Bool, e.args[0].span);
                    e.type = Type::Bool;
                    return {Type::Bool, 0, 0.0, !v.b};
                }
                if (e.text == "~") {
                    const Type expT = (expected != Type::Unknown && Program::isIntType(expected)) ? expected : Type::Unknown;
                    ConstVal v = self(self, e.args[0], inModule, expT);
                    if (!Program::isIntType(v.type)) fail("E_TYPE", "Complemento ~ requiere un tipo entero", e.args[0].span, "~1");
                    e.type = v.type;
                    return makeConstInt(v.type, ~v.u);
                }
                if (e.args[0].kind == ExprKind::Integer && isDecimalDigits(e.args[0].text)) {
                    if (expected != Type::Unknown && Program::isUnsignedInt(expected))
                        fail("E_TYPE", "Menos unario no aplica a enteros sin signo", e.span, "0");
                    if (expected == Type::I8 || expected == Type::I16 || expected == Type::I32) {
                        const std::uint64_t bits = parseExactIntLiteral(program, e.args[0], expected, true);
                        const Integer val = signExtendWidth(bits, Program::intBitWidth(expected));
                        e.kind = ExprKind::Integer;
                        e.text = std::to_string(val);
                        e.args.clear();
                        e.type = expected;
                        return makeConstInt(expected, bits);
                    }
                }
                const Type expT = (expected != Type::Unknown &&
                                   (Program::isSignedInt(expected) || Program::isFloatType(expected)))
                    ? expected : Type::Unknown;
                ConstVal v = self(self, e.args[0], inModule, expT);
                if (Program::isFloatType(v.type)) {
                    e.type = v.type;
                    return {v.type, 0, v.type == Type::F32 ? static_cast<double>(-static_cast<float>(v.f)) : -v.f};
                }
                if (!Program::isSignedInt(v.type))
                    fail("E_TYPE", "Menos unario requiere un entero con signo o flotante", e.args[0].span, "-1");
                if (v.i == minSignedForWidth(Program::intBitWidth(v.type)))
                    fail("R_OVERFLOW", "Desbordamiento en constante", e.span, "const kX: int = -1");
                e.type = v.type;
                return makeConstInt(v.type, static_cast<std::uint64_t>(-v.i));
            }
            case ExprKind::Binary: {
                const auto& op = e.text;
                const bool isRelOrEq = (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=");
                const Type expL = (!isRelOrEq && Program::isNumericType(expected)) ? expected : Type::Unknown;
                ConstVal l = self(self, e.args[0], inModule, expL);
                ConstVal r = self(self, e.args[1], inModule, (op == "&&" || op == "||") ? Type::Bool : l.type);
                if (op == "&&" || op == "||") {
                    checkTypes(program, l.type, Type::Bool, e.args[0].span);
                    checkTypes(program, r.type, Type::Bool, e.args[1].span);
                    e.type = Type::Bool;
                    return {Type::Bool, 0, 0.0, op == "&&" ? (l.b && r.b) : (l.b || r.b)};
                }
                checkTypes(program, r.type, l.type, e.args[1].span, e.args[0].span);
                if (op == "==" || op == "!=") {
                    bool eq = false;
                    if (Program::isIntType(l.type)) eq = (l.u == r.u);
                    else if (Program::isFloatType(l.type)) eq = (l.f == r.f);
                    else if (l.type == Type::Bool) eq = (l.b == r.b);
                    else if (l.type == Type::Str) eq = (l.s == r.s);
                    else fail("E_TYPE", "Tipo no constante", e.span, "1 == 1");
                    e.type = Type::Bool;
                    return {Type::Bool, 0, 0.0, op == "==" ? eq : !eq};
                }
                if (l.type == Type::Str) {
                    if (op == "+" && r.s.size() + l.s.size() <= static_cast<std::size_t>(maxStringLength)) {
                        e.type = Type::Str;
                        return {Type::Str, 0, 0.0, false, l.s + r.s};
                    }
                    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                        e.type = Type::Bool;
                        bool b = op == "<" ? l.s < r.s : op == "<=" ? l.s <= r.s : op == ">" ? l.s > r.s : l.s >= r.s;
                        return {Type::Bool, 0, 0.0, b};
                    }
                    fail("E_TYPE", "Operador inválido para constante str", e.span, R"("a" + "b")");
                }
                if (Program::isFloatType(l.type)) {
                    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                        e.type = Type::Bool;
                        bool b = op == "<" ? l.f < r.f : op == "<=" ? l.f <= r.f : op == ">" ? l.f > r.f : l.f >= r.f;
                        return {Type::Bool, 0, 0.0, b};
                    }
                    double res = 0.0;
                    if (l.type == Type::F32) {
                        const float lf = static_cast<float>(l.f), rf = static_cast<float>(r.f);
                        const float fr = op == "+" ? lf + rf : op == "-" ? lf - rf : op == "*" ? lf * rf :
                                         op == "/" ? lf / rf : (fail("E_TYPE", "Operador inválido para f32", e.span, "a + b"), 0.0f);
                        res = static_cast<double>(fr);
                    } else {
                        res = op == "+" ? l.f + r.f : op == "-" ? l.f - r.f : op == "*" ? l.f * r.f :
                              op == "/" ? l.f / r.f : (fail("E_TYPE", "Operador inválido para float", e.span, "a + b"), 0.0);
                    }
                    e.type = l.type;
                    return {l.type, 0, res};
                }
                if (!Program::isIntType(l.type)) fail("E_TYPE", "Tipo no constante", e.span, "1 + 1");
                const unsigned w = Program::intBitWidth(l.type);
                const bool isSigned = Program::isSignedInt(l.type);
                if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                    e.type = Type::Bool;
                    bool b = isSigned
                        ? (op == "<" ? l.i < r.i : op == "<=" ? l.i <= r.i : op == ">" ? l.i > r.i : l.i >= r.i)
                        : (op == "<" ? l.u < r.u : op == "<=" ? l.u <= r.u : op == ">" ? l.u > r.u : l.u >= r.u);
                    return {Type::Bool, 0, 0.0, b};
                }
                if (op == "&" || op == "|" || op == "^") {
                    e.type = l.type;
                    const std::uint64_t br = op == "&" ? (l.u & r.u) : op == "|" ? (l.u | r.u) : (l.u ^ r.u);
                    return makeConstInt(l.type, br);
                }
                if (op == "<<" || op == ">>" || op == ">>>") {
                    if ((isSigned && r.i < 0) || r.u >= w)
                        fail("R_OVERFLOW", "Desplazamiento fuera de rango en constante", e.span, "1 << 1");
                    const unsigned sc = static_cast<unsigned>(r.u);
                    e.type = l.type;
                    if (op == "<<") return makeConstInt(l.type, l.u << sc);
                    if (op == ">>>") return makeConstInt(l.type, l.u >> sc);
                    if (isSigned) return makeConstInt(l.type, static_cast<std::uint64_t>(l.i >> sc));
                    return makeConstInt(l.type, l.u >> sc);
                }
                e.type = l.type;
                if (op == "/" || op == "%") {
                    if (r.u == 0) fail("R_DIV_ZERO", "Divisor cero en constante", e.span, "4 / 2");
                    if (isSigned) {
                        if (l.i == minSignedForWidth(w) && r.i == -1)
                            fail("R_OVERFLOW", "Desbordamiento en constante", e.span, "1");
                        return makeConstInt(l.type, static_cast<std::uint64_t>(op == "/" ? l.i / r.i : l.i % r.i));
                    }
                    return makeConstInt(l.type, op == "/" ? l.u / r.u : l.u % r.u);
                }
                if (isSigned) {
                    auto ov = (op == "+") ? checkedAddWidth(l.i, r.i, w)
                            : (op == "-") ? checkedSubWidth(l.i, r.i, w)
                                          : checkedMulWidth(l.i, r.i, w);
                    if (!ov) fail("R_OVERFLOW", "Desbordamiento en constante", e.span, "1");
                    return makeConstInt(l.type, static_cast<std::uint64_t>(*ov));
                }
                auto uov = (op == "+") ? checkedUAddWidth(l.u, r.u, w)
                         : (op == "-") ? checkedUSubWidth(l.u, r.u)
                                       : checkedUMulWidth(l.u, r.u, w);
                if (!uov) fail("R_OVERFLOW", "Desbordamiento en constante", e.span, "1");
                return makeConstInt(l.type, *uov);
            }
            case ExprKind::If: {
                if (!e.ifExpr || !e.ifExpr->thenBody.empty() || !e.ifExpr->elseBody.empty() ||
                    !e.ifExpr->thenResult || !e.ifExpr->elseResult) {
                    fail("E_VALUE", "Un if en constante solo admite expresiones puras en ambas ramas", e.span,
                         "const kV: int = if kFlag { 1 } else { 2 }");
                }
                ConstVal cv = self(self, e.ifExpr->condition, inModule, Type::Bool);
                checkTypes(program, cv.type, Type::Bool, e.ifExpr->condition.span);
                ConstVal res = cv.b ? self(self, *e.ifExpr->thenResult, inModule, expected)
                                    : self(self, *e.ifExpr->elseResult, inModule, expected);
                e.type = res.type;
                return res;
            }
            default:
                fail("E_VALUE", "Una constante requiere una expresión constante", e.span, "const kA: int = 1");
        }
    };
    for (std::size_t idx = 0; idx < program.consts.size(); ++idx) {
        if (constState[idx] != 0) continue;
        constState[idx] = 1;
        auto& cd = program.consts[idx];
        if (!isValidConstType(cd.type))
            fail("E_TYPE", "const admite tipos numéricos, bool o str", cd.span, "const kMax: int = 10");
        ConstVal v = evalConst(evalConst, cd.init, cd.module, cd.type);
        checkTypes(program, v.type, cd.type, cd.init.span, cd.span);
        cd.intVal = v.i; cd.uintVal = v.u; cd.floatVal = v.f; cd.boolVal = v.b; cd.strVal = v.s;
        constState[idx] = 2;
    }
    for (auto& d : program.data) {
        if (!d.isEnum) continue;
        std::int64_t nextVal = 0;
        bool nextValid = true;
        std::unordered_set<std::int64_t> usedVals;
        for (auto& v : d.variants) {
            if (v.enumValueExpr) {
                ConstVal cv = evalConst(evalConst, *v.enumValueExpr, d.module, Type::Int);
                checkTypes(program, cv.type, Type::Int, v.enumValueExpr->span, v.span);
                v.enumValue = cv.i;
                if (cv.i == maxInt) { nextValid = false; }
                else { nextVal = cv.i + 1; nextValid = true; }
            } else {
                if (!nextValid) fail("E_INT_RANGE", "El valor implícito del enum desborda int64", v.span, "enum E { A = 1 }");
                v.enumValue = nextVal;
                if (nextVal == maxInt) nextValid = false;
                else ++nextVal;
            }
            if (d.hasExplicitEnumValues && !usedVals.insert(v.enumValue).second)
                fail("E_DUP_ENUM_VALUE", "Valor entero duplicado en enum: " + std::to_string(v.enumValue), v.span, "enum Kind { A = 1, B = 2 }");
        }
    }
    Interfaces interfaces(program, types);
    GenericInstances generics(program);
    types.syncOpaque();
    Functions functions, tests;
    interfaces.fnTable = &functions;
    for (const auto& fn : program.functions) {
        auto& names = fn.isTest ? tests : functions;
        if (!fn.isTest && (fn.sourceName == "len" || fn.sourceName == "print" || fn.sourceName == "assert"))
            fail("E_RESERVED", "Nombre reservado: " + fn.name, fn.span, "fn count() -> int { return 1 }");
        const auto [it, added] = names.emplace(fn.name, &fn);
        if (!added) fail("E_DUP_FUNCTION", "Declaración duplicada: " + fn.name, fn.span,
                         "fn other() {}", it->second->span);
        if (!fn.isTest && fn.name == program.entryMain && (!fn.params.empty() || fn.result != Type::Void))
            fail("E_MAIN", "main debe carecer de parámetros y devolver void", fn.span, "fn main() {}");
    }
    {
        auto isFfiOk = [&](Type t, bool allowVoid) -> bool {
            return (allowVoid && t == Type::Void) || t == Type::Bool ||
                   Program::isNumericType(t) || program.isAnyPtr(t);
        };
        std::unordered_map<std::string, const Function*> externBySym;
        for (const auto& fn : program.functions) {
            if (fn.isTest || !fn.isExternC) continue;
            if (fn.cSymbol == "main") {
                Diagnostic d("E_FFI_ABI", "El símbolo extern \"C\" no puede llamarse 'main'.", fn.span);
                throw d;
            }
            types.known(fn.result, fn.span, true);
            if (!isFfiOk(fn.result, true)) {
                Diagnostic d("E_FFI_TYPE", "Tipo de retorno no compatible con extern \"C\": " + program.nameOf(fn.result), fn.span);
                d.actual = program.nameOf(fn.result);
                d.example = "Usa escalares (i8..u64, f32, f64, bool), Ptr<T>, MutPtr<T> o void.";
                throw d;
            }
            for (const auto& p : fn.params) {
                if (p.byRef || p.writable) {
                    Diagnostic d("E_FFI_TYPE", "Los parámetros extern \"C\" no admiten mut ni var; usa Ptr<T> o MutPtr<T>.", p.span);
                    throw d;
                }
                types.known(p.type, p.span, false);
                if (!isFfiOk(p.type, false)) {
                    Diagnostic d("E_FFI_TYPE", "Tipo de parámetro no compatible con extern \"C\": " + program.nameOf(p.type) +
                                 " (pasa structs @repr(C) mediante Ptr<T> o MutPtr<T>).", p.span);
                    d.actual = program.nameOf(p.type);
                    d.example = "fn init_buf(p: MutPtr<Packet>)";
                    throw d;
                }
            }
            const auto [eit, added] = externBySym.emplace(fn.cSymbol, &fn);
            if (!added) {
                const Function* prev = eit->second;
                if (!fn.isExternDecl && !prev->isExternDecl)
                    fail("E_DUP_FUNCTION", "Símbolo extern \"C\" duplicado: " + fn.cSymbol, fn.span,
                         "extern \"C\" fn otro() {}", prev->span);
                bool sameSig = (prev->result == fn.result && prev->params.size() == fn.params.size());
                for (std::size_t pi = 0; sameSig && pi < fn.params.size(); ++pi)
                    sameSig = (prev->params[pi].type == fn.params[pi].type);
                if (!sameSig || !fn.isExternDecl || !prev->isExternDecl) {
                    Diagnostic d("E_FFI_TYPE", "Declaración extern \"C\" incompatible para símbolo '" + fn.cSymbol + "'.", fn.span);
                    d.related.push_back(prev->span);
                    throw d;
                }
            }
        }
    }
    std::deque<Function> pendingClosures;
    std::unordered_map<std::string, std::size_t> fnRefCache;
    std::size_t nextClosureId = 0;
    auto isAbstractFn = [&](const Function& f) {
        return f.name.starts_with("@abstract.") ||
               std::any_of(f.substitutedTypes.begin(), f.substitutedTypes.end(),
                           [&](Type t) { return isOpaqueType(program, t); });
    };
    auto checkFunction = [&](Function& fn) {
        Checker checker{functions, fn, {}, types, interfaces, generics, sizes, {},
                        &functions, &pendingClosures, &fnRefCache, &nextClosureId,
                        nullptr, nullptr, nullptr, isAbstractFn(fn), {}, {}, {}, false, fn.safety};
        checker.known(fn.result, fn.span, true);
        checker.reserveValue(fn.result, fn.span);
        for (const auto& param : fn.params) {
            checker.known(param.type, param.span);
            const bool scalar = !param.byRef && (Program::isNumericType(param.type) || param.type == Type::Bool);
            if (param.writable && !param.byRef && !scalar)
                fail("E_PARAM_VAR", "Un parámetro var admite escalares numéricos o bool; para otros tipos usa nombre: mut Tipo.",
                     param.span, "fn step(var n: int) {}");
            checker.declare(param.name, param.type, param.writable, param.span, false).scalarParam = scalar;
        }
        auto checkBoolContract = [&](Expr& e, const char* clauseName) {
            const Type ct = checker.typeOf(e, ArrayUse::Inspect, Type::Bool);
            if (ct != Type::Bool) {
                Diagnostic d("E_CONTRACT",
                             std::string("La cláusula ") + clauseName + " debe evaluar a bool.",
                             e.span);
                d.expected = "bool";
                d.actual = types.program.nameOf(ct);
                d.example = std::string(clauseName) + " x > 0";
                throw d;
            }
        };
        checker.inContract = true;
        checker.inEnsures = false;
        for (Expr& req : fn.requiresClauses) checkBoolContract(req, "requires");
        if (!fn.ensuresClauses.empty()) {
            if (fn.result != Type::Void) {
                for (const auto& p : fn.params) {
                    if (p.name == "result") {
                        fail("E_CONTRACT",
                             "Un parámetro no puede llamarse 'result' cuando la función declara 'ensures' con valor de retorno.",
                             p.span, "fn f(x: int) -> int ensures result > 0 { return x }");
                    }
                }
            }
            checker.locals.levels.emplace_back();
            if (fn.result != Type::Void)
                checker.declare("result", fn.result, false, fn.span, false);
            checker.inEnsures = true;
            for (Expr& ens : fn.ensuresClauses) checkBoolContract(ens, "ensures");
            checker.inEnsures = false;
            checker.locals.levels.pop_back();
        }
        checker.inContract = false;
        if (fn.isExternDecl || (fn.name.starts_with("@abstract.iface.") && !fn.hasDefaultBody)) return;
        if (!checker.block(fn.body, false) && fn.result != Type::Void)
            fail("E_RETURN", "Hay un camino sin valor de retorno en " + fn.name, fn.span,
                 "fn one() -> int { return 1 }");
    };
    for (auto& fn : program.functions) checkFunction(fn);
    for (auto& fn : interfaces.abstractChecks) checkFunction(fn);
    std::size_t genCursor = 0, methCursor = 0;
    while (genCursor < generics.pending.size() || methCursor < interfaces.pendingMethods.size()) {
        while (genCursor < generics.pending.size()) checkFunction(generics.pending[genCursor++]);
        while (methCursor < interfaces.pendingMethods.size()) checkFunction(interfaces.pendingMethods[methCursor++]);
    }
    for (auto& m : interfaces.pendingMethods) program.functions.push_back(std::move(m));
    for (auto& c : pendingClosures) program.functions.push_back(std::move(c));
    generics.finish();
    std::erase_if(program.data, [&](const DataDecl& d) { return mentionsOpaqueType(program, d.type); });
    types.reindexConcrete();
    {
        Layouts layouts{types, {}, {}};
        for (const auto& d : program.data) sizes.emplace(d.type, layouts.measure(d.type, d.span).bytes);
    }
}

// Independent, stack-based evaluation. Calls, loops and returns never use the
// host call stack. Only the bounded syntax tree is visited recursively.
struct Aggregate;
struct ArrayBuffer;
struct StringBuilderBuf;
struct SwissTableBuf;
struct DynObject;
struct ClosureObject;
struct PoolObject;
struct SharedObject;
struct FileObject;
struct TcpListenerObject;
struct SocketObject;
struct MutexObject;
struct LockObject;
struct ScopeObject;
struct TaskObject;
struct FrozenObject;
struct AtomicObject;
struct RingQueueObject;
struct RegionObject;
struct HandleValue {
    std::uint32_t idx = 0;
    std::uint32_t gen = 0;
    bool operator==(const HandleValue&) const = default;
};
struct RawPtrVal {
    Type pointee = Type::Unknown;
    std::uintptr_t addr = 0;
    void* vmSlot = nullptr;
    std::shared_ptr<ArrayBuffer> vmArr = nullptr;
    Type slotType = Type::Unknown;
    std::size_t byteOffset = 0;
    bool operator==(const RawPtrVal&) const = default;
};
struct SimdVal {
    Type elemType = Type::Unknown;
    unsigned lanes = 0;
    // Los tipos SIMD válidos ocupan como máximo 64 bytes. Empaquetar los
    // carriles evita que cada Value escalar reserve espacio para 32 enteros.
    std::array<std::uint64_t, 8> words{};
    bool operator==(const SimdVal&) const = default;

    std::uint64_t lane(unsigned index) const {
        const unsigned width = simdElemBytes(elemType) * 8;
        const unsigned offset = index * width;
        return (words[offset / 64] >> (offset % 64)) & maskForWidth(width);
    }

    void setLane(unsigned index, std::uint64_t value) {
        const unsigned width = simdElemBytes(elemType) * 8;
        const unsigned offset = index * width;
        const unsigned shift = offset % 64;
        const std::uint64_t mask = maskForWidth(width);
        auto& word = words[offset / 64];
        word = (word & ~(mask << shift)) | ((value & mask) << shift);
    }
};
using AggregatePtr = std::shared_ptr<Aggregate>;
using ArrayPtr = std::shared_ptr<ArrayBuffer>;
using SbPtr = std::shared_ptr<StringBuilderBuf>;
using TablePtr = std::shared_ptr<SwissTableBuf>;
using DynPtr = std::shared_ptr<DynObject>;
using ClosurePtr = std::shared_ptr<const ClosureObject>;
using PoolPtr = std::shared_ptr<PoolObject>;
using SharedPtr = std::shared_ptr<SharedObject>;
using FilePtr = std::shared_ptr<FileObject>;
using TcpListenerPtr = std::shared_ptr<TcpListenerObject>;
using SocketPtr = std::shared_ptr<SocketObject>;
using MutexPtr = std::shared_ptr<MutexObject>;
using LockPtr = std::shared_ptr<LockObject>;
using ScopePtr = std::shared_ptr<ScopeObject>;
using TaskPtr = std::shared_ptr<TaskObject>;
using FrozenPtr = std::shared_ptr<const FrozenObject>;
using AtomicPtr = std::shared_ptr<AtomicObject>;
using RingQueuePtr = std::shared_ptr<RingQueueObject>;
using RegionPtr = std::shared_ptr<RegionObject>;
using Value = std::variant<std::monostate, Integer, ExactInt, float, double, bool, std::string_view, SimdVal, AggregatePtr, ArrayPtr, SbPtr, TablePtr, DynPtr, ClosurePtr, HandleValue, RawPtrVal, PoolPtr, SharedPtr, FilePtr, TcpListenerPtr, SocketPtr, MutexPtr, LockPtr, ScopePtr, TaskPtr, FrozenPtr, AtomicPtr, RingQueuePtr, RegionPtr>;

inline void ensureWinsock() {
#ifdef _WIN32
    static bool initialized = []() {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)initialized;
#endif
}

struct FileObject {
    FILE* fp = nullptr;
    void close() {
        if (fp) {
            std::fclose(fp);
            fp = nullptr;
        }
    }
    ~FileObject() { close(); }
};

struct TcpListenerObject {
    std::uintptr_t fd = 0;
    int port = 0;
    bool open = false;
    void close() {
        if (open) {
#ifdef _WIN32
            closesocket(static_cast<SOCKET>(fd));
#else
            ::close(static_cast<int>(fd));
#endif
            open = false;
            fd = 0;
        }
    }
    ~TcpListenerObject() { close(); }
};

struct SocketObject {
    std::uintptr_t fd = 0;
    bool open = false;
    void close() {
        if (open) {
#ifdef _WIN32
            closesocket(static_cast<SOCKET>(fd));
#else
            ::close(static_cast<int>(fd));
#endif
            open = false;
            fd = 0;
        }
    }
    ~SocketObject() { close(); }
};

struct MutexState {
    std::mutex mu;
    std::condition_variable cv;
    bool locked = false;
};

struct MutexObject {
    std::shared_ptr<MutexState> state;
};

struct LockObject {
    std::shared_ptr<MutexState> state;
    void unlock() {
        if (state) {
            {
                std::lock_guard<std::mutex> lk(state->mu);
                state->locked = false;
            }
            state->cv.notify_one();
            state.reset();
        }
    }
    ~LockObject() { unlock(); }
};
struct TaskObject {
    Value result;
    bool consumed = false;
    bool hasResult = false;
};
struct ScopeObject {
    bool open = true;
    std::vector<TaskPtr> tasks;
};
struct FrozenObject {
    Value val;
};
struct AtomicObject {
    Type elemType = Type::Unknown;
    std::atomic<std::uint64_t> bits{0};
    AtomicObject(Type t, std::uint64_t init) : elemType(t), bits(init) {}
};
struct RingQueueObject {
    Type elemType = Type::Unknown;
    std::size_t cap = 0;
    std::mutex mu;
    std::deque<Value> items;
    RingQueueObject(Type t, std::size_t c) : elemType(t), cap(c) {}
};
struct RegionChunk {
    std::uint8_t* data = nullptr;
    std::size_t cap = 0;
    std::size_t used = 0;
    std::size_t align = 64u;
};
struct RegionObject {
    std::vector<RegionChunk> chunks;
    std::size_t initCap = 0;
    std::size_t totalUsed = 0;
    std::size_t totalCap = 0;
    static std::uint8_t* allocAlignedMem(std::size_t bytes, std::size_t align) {
        const std::size_t sz = bytes > 0 ? bytes : 1u;
#ifdef _WIN32
        return static_cast<std::uint8_t*>(_aligned_malloc(sz, align));
#else
        const std::size_t eff = std::max(align, sizeof(void*));
        void* p = nullptr;
        if (posix_memalign(&p, eff, sz) != 0) return nullptr;
        return static_cast<std::uint8_t*>(p);
#endif
    }
    static void freeAlignedMem(std::uint8_t* p) {
        if (!p) return;
#ifdef _WIN32
        _aligned_free(p);
#else
        std::free(p);
#endif
    }
    std::uint8_t* alloc(std::size_t bytes, std::size_t align) {
        const std::size_t effAlign = align > 0 ? align : 1u;
        if (!chunks.empty()) {
            auto& cur = chunks.back();
            if (cur.align >= effAlign) {
                const std::size_t off = (cur.used + effAlign - 1u) & ~(effAlign - 1u);
                if (off >= cur.used && off <= cur.cap && bytes <= cur.cap - off) {
                    std::uint8_t* p = cur.data + off;
                    totalUsed += (off + bytes) - cur.used;
                    cur.used = off + bytes;
                    return p;
                }
            }
        }
        const std::size_t chunkAlign = std::max<std::size_t>(64u, effAlign);
        std::size_t nextCap = initCap > 0 ? initCap : 4096u;
        if (!chunks.empty()) {
            const std::size_t prev = chunks.back().cap;
            nextCap = prev <= (std::numeric_limits<std::size_t>::max() / 2u)
                ? prev * 2u : std::numeric_limits<std::size_t>::max();
        }
        nextCap = std::max<std::size_t>(std::max<std::size_t>(nextCap, 64u), bytes);
        std::uint8_t* mem = allocAlignedMem(nextCap, chunkAlign);
        if (!mem) return nullptr;
        chunks.push_back(RegionChunk{mem, nextCap, bytes, chunkAlign});
        totalCap += nextCap;
        totalUsed += bytes;
        return mem;
    }
    void reset() {
        while (chunks.size() > 1u) {
            freeAlignedMem(chunks.back().data);
            chunks.pop_back();
        }
        totalUsed = 0;
        if (!chunks.empty()) {
            chunks[0].used = 0;
            totalCap = chunks[0].cap;
        } else {
            totalCap = 0;
        }
    }
    void close() {
        for (auto& ch : chunks) freeAlignedMem(ch.data);
        chunks.clear();
        totalUsed = 0;
        totalCap = 0;
    }
    ~RegionObject() { close(); }
};
struct DynObject {
    Type concreteType = Type::Unknown;
    Type iface = Type::Unknown;
    Value inner;
};
struct ClosureObject {
    std::size_t id = 0;
    Value env;
};
struct ArrayBuffer {
    std::vector<Value> items;
    std::size_t cap = 0;
};
struct StringBuilderBuf {
    std::string data;
    std::size_t cap = 0;
};
struct PoolSlot {
    Value val;
    std::uint32_t gen = 1;
    bool alive = false;
};
struct PoolObject {
    std::vector<PoolSlot> slots;
    std::vector<std::uint32_t> freeList;
    std::size_t len = 0;
    std::size_t cap = 0;
    std::size_t used = 0;
};
struct SharedObject {
    Value payload;
    SharedObject() = default;
    explicit SharedObject(Value v) : payload(std::move(v)) {}
    SharedObject(const SharedObject&) = default;
    SharedObject(SharedObject&&) = default;
    SharedObject& operator=(const SharedObject&) = default;
    // Soltar una lista o un árbol largo con destructores anidados desbordaría la pila del
    // intérprete. El contenido de cada celda que muere se aparta y se suelta en un bucle, así la
    // profundidad de pila no depende del tamaño de los datos.
    ~SharedObject() {
        static thread_local std::vector<Value>* pending = new std::vector<Value>();
        static thread_local bool draining = false;
        pending->push_back(std::move(payload));
        if (draining) return;
        draining = true;
        while (!pending->empty()) {
            Value dying = std::move(pending->back());
            pending->pop_back();
        }
        draining = false;
    }
};
enum class TableKeyKind { I64, ExactInt, Bool, Enum, Str, Handle };
struct SwissTableBuf {
    TableKeyKind keyKind = TableKeyKind::I64;
    bool isMap = true;
    bool secure = false;
    std::uint64_t seed = 0x243F6A8885A308D3ULL;
    std::uint32_t len = 0;
    std::uint32_t buckets = 0;
    std::uint32_t growthLeft = 0;
    std::vector<std::uint8_t> ctrl;
    std::vector<Value> keys;
    std::vector<Value> vals;
};
struct Aggregate {
    std::string_view tag;
    std::vector<Value> fields;
};
enum class Op {
    Literal, Read, Call, SharedUpdateEnd, BoxDyn, DynCall, MakeClosure, ClosureCall, ArrayLit, Interp, Slice, Unary, Binary, ShortCircuit, Bind, Write, Discard, DropOperands, Print,
    Assert, ContractCheck, BeginEnsures, EndEnsuresReturn, IfFalse, Jump, Enter, Leave, RangeEnter, RangeBind, RangeNext, RangePop,
    ForEachEnter, ForEachBind, ForEachNext, ForEachPop, Return,
    Construct, Field, FieldSet, MatchArm, Index, IndexSet, PlaceSet, SavePlaceIdx, ReadPlace, Try
};
struct Instruction {
    Op op;
    Span span;
    const Expr* expr = nullptr;
    const Stmt* stmt = nullptr;
    std::size_t target = 0;
    const MatchArm* arm = nullptr;
    bool rawMode = false;
};
using Code = std::vector<Instruction>;

struct EvaluationOrder {
    const Functions& userFunctions;
    const Function* currentFn = nullptr;
    Code code;
    bool inRawBlock = false;
    struct LoopTarget {
        std::string label;
        bool isFor = false;
        std::size_t breakScope = 0;
        std::size_t breakRange = 0;
        std::size_t breakForEach = 0;
        std::size_t contScope = 0;
        std::size_t contRange = 0;
        std::size_t contForEach = 0;
        std::size_t contTarget = 0;
        std::size_t operandDepth = 0;
        std::vector<std::size_t> contPatches;
        std::vector<std::size_t> breakPatches;
    };
    std::vector<LoopTarget> loopTargets;
    std::size_t scopeDepth = 0;
    std::size_t rangeDepth = 0;
    std::size_t forEachDepth = 0;
    std::size_t pendingOperands = 0;

    explicit EvaluationOrder(const Functions& uf) : userFunctions(uf) {}

    bool emitPlaceIndices(const Expr& target) {
        std::vector<const Expr*> idxExprs;
        const Expr* cur = &target;
        while (cur->kind == ExprKind::Field || cur->kind == ExprKind::Index) {
            if (cur->kind == ExprKind::Index) idxExprs.push_back(&cur->args.at(1));
            cur = &cur->args.at(0);
        }
        for (auto it = idxExprs.rbegin(); it != idxExprs.rend(); ++it) {
            expression(**it);
            code.push_back({Op::SavePlaceIdx, (*it)->span});
            --pendingOperands;
        }
        return !idxExprs.empty();
    }

    void expression(const Expr& e) {
        const std::size_t basePending = pendingOperands;
        switch (e.kind) {
            case ExprKind::Name: code.push_back({Op::Read, e.span, &e}); ++pendingOperands; return;
            case ExprKind::Integer: case ExprKind::Float: case ExprKind::Boolean: case ExprKind::String:
                code.push_back({Op::Literal, e.span, &e}); ++pendingOperands; return;
            case ExprKind::ArrayLit:
                for (const auto& elem : e.args) expression(elem);
                code.push_back({Op::ArrayLit, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Interp:
                for (const auto& part : e.args) expression(part);
                code.push_back({Op::Interp, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Slice:
                expression(e.args[0]);
                expression(e.args[1]);
                expression(e.args[2]);
                code.push_back({Op::Slice, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Call: {
                const bool isBuiltin = !userFunctions.contains(e.text);
                const std::size_t skipArg = isBuiltin ? (
                    (e.text == "push" || e.text == "pop" || e.text == "clear" || e.text == "reserve" || e.text == "truncate" ||
                     e.text == "sbPush" || e.text == "sbPushByte" || e.text == "sbPushInt" ||
                     e.text == "sbPushFloat" || e.text == "sbClear" || e.text == "sbTake" ||
                     e.text == "mapSet" || e.text == "mapDel" || e.text == "setAdd" ||
                     e.text == "setDel" || e.text == "sort" || e.text == "sortStable" ||
                     e.text == "poolAdd" || e.text == "poolSet" || e.text == "poolRemove" ||
                     e.text == "sharedSet" || e.text == "sharedUpdate" ||
                     e.text == "fileRead" || e.text == "fileReadAll" || e.text == "fileReadLine" ||
                     e.text == "fileWrite" || e.text == "fileFlush" || e.text == "fileClose" ||
                     e.text == "tcpAccept" || e.text == "socketRead" || e.text == "socketWrite" ||
                     e.text == "socketClose" || e.text == "listenerClose" ||
                     e.text == "mutexLock" || e.text == "mutexTryLock" || e.text == "unlock" ||
                     e.text == "taskSpawn" || e.text == "taskSpawnMove" ||
                     e.text == "ptrOf" || e.text == "mutPtrOf" || e.text == "mutSlicePtr" ||
                     e.text == "regionAlloc" || e.text == "regionAllocAligned" || e.text == "regionReset")
                        ? std::size_t(0)
                        : (e.text == "simdToArray" ? std::size_t(1) : ~std::size_t(0)))
                    : ~std::size_t(0);
                for (std::size_t k = 0; k < e.args.size(); ++k) {
                    if (k == skipArg) {
                        (void)emitPlaceIndices(e.args[k]);
                        continue;
                    }
                    if (e.args[k].isMutArg && emitPlaceIndices(e.args[k])) {
                        code.push_back({Op::ReadPlace, e.args[k].span, &e.args[k]});
                        ++pendingOperands;
                    } else {
                        expression(e.args[k]);
                    }
                }
                code.push_back({Op::Call, e.span, &e, nullptr, 0, nullptr, inRawBlock});
                if (isBuiltin && e.text == "sharedUpdate")
                    code.push_back({Op::SharedUpdateEnd, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            }
            case ExprKind::BoxDyn:
                expression(e.args[0]);
                code.push_back({Op::BoxDyn, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::DynCall:
                for (const auto& a : e.args) {
                    if (a.isMutArg && emitPlaceIndices(a)) {
                        code.push_back({Op::ReadPlace, a.span, &a});
                        ++pendingOperands;
                    } else {
                        expression(a);
                    }
                }
                code.push_back({Op::DynCall, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Closure:
                for (const auto& a : e.args) expression(a);
                code.push_back({Op::MakeClosure, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::ClosureCall:
                for (const auto& a : e.args) expression(a);
                code.push_back({Op::ClosureCall, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Match: {
                const auto& m = *e.matchExpr;
                expression(m.scrutinee);
                std::vector<std::size_t> exits;
                for (const auto& arm : m.arms) {
                    const auto attempt = code.size();
                    code.push_back({Op::MatchArm, arm.span, nullptr, nullptr, 0, &arm});
                    ++scopeDepth;
                    pendingOperands = basePending;
                    block(arm.body);
                    if (arm.resultExpr) expression(*arm.resultExpr);
                    --scopeDepth;
                    code.push_back({Op::Leave, arm.span});
                    exits.push_back(code.size());
                    code.push_back({Op::Jump, arm.span});
                    code[attempt].target = code.size();
                }
                for (auto end : exits) code[end].target = code.size();
                pendingOperands = basePending + 1;
                return;
            }
            case ExprKind::If: {
                const auto& ie = *e.ifExpr;
                expression(ie.condition);
                const auto ifFalsePos = code.size();
                code.push_back({Op::IfFalse, e.span});
                code.push_back({Op::Enter, e.span});
                ++scopeDepth;
                pendingOperands = basePending;
                block(ie.thenBody);
                if (ie.thenResult) expression(*ie.thenResult);
                --scopeDepth;
                code.push_back({Op::Leave, e.span});
                const auto endThen = code.size();
                code.push_back({Op::Jump, e.span});
                code[ifFalsePos].target = code.size();
                code.push_back({Op::Enter, e.span});
                ++scopeDepth;
                pendingOperands = basePending;
                block(ie.elseBody);
                if (ie.elseResult) expression(*ie.elseResult);
                --scopeDepth;
                code.push_back({Op::Leave, e.span});
                code[endThen].target = code.size();
                pendingOperands = basePending + 1;
                return;
            }
            case ExprKind::MethodCall:
            case ExprKind::AssocCall:
                throw std::logic_error("unresolved method/assoc call in interpreter");
            case ExprKind::Unary:
                expression(e.args[0]);
                code.push_back({Op::Unary, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Field:
                expression(e.args[0]);
                code.push_back({Op::Field, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Index:
                expression(e.args[0]);
                expression(e.args[1]);
                code.push_back({Op::Index, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Construct:
                for (const auto& arg : e.args) expression(arg);
                code.push_back({Op::Construct, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Try:
                expression(e.args[0]);
                code.push_back({Op::Try, e.span, &e});
                pendingOperands = basePending + 1;
                return;
            case ExprKind::Binary:
                expression(e.args[0]);
                if (e.text == "&&" || e.text == "||") {
                    const auto jump = code.size();
                    code.push_back({Op::ShortCircuit, e.span, &e});
                    pendingOperands = basePending;
                    expression(e.args[1]);
                    code[jump].target = code.size();
                } else {
                    expression(e.args[1]);
                    code.push_back({Op::Binary, e.span, &e});
                }
                pendingOperands = basePending + 1;
                return;
        }
    }

    void block(const std::vector<Stmt>& body) {
        for (const auto& s : body) statement(s);
    }

    void scoped(const std::vector<Stmt>& body, Span span) {
        code.push_back({Op::Enter, span});
        ++scopeDepth;
        block(body);
        --scopeDepth;
        code.push_back({Op::Leave, span});
    }

    LoopTarget& findLoop(const std::string& label) {
        if (label.empty()) return loopTargets.back();
        for (auto it = loopTargets.rbegin(); it != loopTargets.rend(); ++it)
            if (it->label == label) return *it;
        return loopTargets.back();
    }

    void statement(const Stmt& s) {
        const std::size_t stmtBase = pendingOperands;
        const auto start = code.size();
        for (const auto& expr : s.exprs) expression(expr);
        const auto current = code.size();
        pendingOperands = stmtBase;
        switch (s.kind) {
            case StmtKind::Let: case StmtKind::Var: code.push_back({Op::Bind, s.span, nullptr, &s}); return;
            case StmtKind::Assign: code.push_back({Op::Write, s.span, nullptr, &s}); return;
            case StmtKind::IndexSet: code.push_back({Op::IndexSet, s.span, nullptr, &s}); return;
            case StmtKind::FieldSet: code.push_back({Op::FieldSet, s.span, nullptr, &s}); return;
            case StmtKind::PlaceSet: code.push_back({Op::PlaceSet, s.span, nullptr, &s}); return;
            case StmtKind::Call: code.push_back({Op::Discard, s.span}); return;
            case StmtKind::Print: code.push_back({Op::Print, s.span}); return;
            case StmtKind::Assert: code.push_back({Op::Assert, s.span}); return;
            case StmtKind::Return:
                if (currentFn && !currentFn->ensuresClauses.empty() && currentFn->safety != SafetyTier::Raw) {
                    const std::size_t hasVal = s.exprs.empty() ? 0u : 1u;
                    code.push_back({Op::BeginEnsures, s.span, nullptr, &s, hasVal});
                    pendingOperands = stmtBase;
                    for (const auto& ens : currentFn->ensuresClauses) {
                        expression(ens);
                        code.push_back({Op::ContractCheck, ens.span});
                        --pendingOperands;
                    }
                    code.push_back({Op::EndEnsuresReturn, s.span, nullptr, &s, hasVal});
                    pendingOperands = stmtBase;
                    return;
                }
                code.push_back({Op::Return, s.span, nullptr, &s});
                return;
            case StmtKind::If: {
                code.push_back({Op::IfFalse, s.span});
                scoped(s.body, s.span);
                const auto endThen = code.size();
                code.push_back({Op::Jump, s.span});
                code[current].target = code.size();
                scoped(s.otherwise, s.span);
                code[endThen].target = code.size();
                pendingOperands = stmtBase;
                return;
            }
            case StmtKind::SafetyBlock: {
                const bool savedRaw = inRawBlock;
                inRawBlock = (s.safety == SafetyTier::Raw);
                scoped(s.body, s.span);
                inRawBlock = savedRaw;
                pendingOperands = stmtBase;
                return;
            }
            case StmtKind::While: {
                code.push_back({Op::IfFalse, s.span});
                loopTargets.push_back({s.label, false, scopeDepth, rangeDepth, forEachDepth,
                                       scopeDepth, rangeDepth, forEachDepth, start, stmtBase, {}, {}});
                scoped(s.body, s.span);
                code.push_back({Op::Jump, s.span, nullptr, nullptr, start});
                const auto endIp = code.size();
                code[current].target = endIp;
                for (std::size_t patch : loopTargets.back().breakPatches) code[patch].target = endIp;
                loopTargets.pop_back();
                pendingOperands = stmtBase;
                return;
            }
            case StmtKind::For: {
                code.push_back({Op::RangeEnter, s.span});
                ++rangeDepth;
                code.push_back({Op::RangeBind, s.span, nullptr, &s});
                ++scopeDepth;
                loopTargets.push_back({s.label, true, scopeDepth - 1, rangeDepth - 1, forEachDepth,
                                       scopeDepth, rangeDepth, forEachDepth, 0, stmtBase, {}, {}});
                block(s.body);
                const auto nextIp = code.size();
                code.push_back({Op::RangeNext, s.span, nullptr, nullptr, current + 1});
                --scopeDepth;
                --rangeDepth;
                const auto endIp = code.size();
                code[current].target = endIp;
                for (std::size_t patch : loopTargets.back().contPatches) code[patch].target = nextIp;
                for (std::size_t patch : loopTargets.back().breakPatches) code[patch].target = endIp;
                loopTargets.pop_back();
                pendingOperands = stmtBase;
                return;
            }
            case StmtKind::ForEach: {
                code.push_back({Op::ForEachEnter, s.span});
                ++forEachDepth;
                code.push_back({Op::ForEachBind, s.span, nullptr, &s});
                ++scopeDepth;
                loopTargets.push_back({s.label, true, scopeDepth - 1, rangeDepth, forEachDepth - 1,
                                       scopeDepth, rangeDepth, forEachDepth, 0, stmtBase, {}, {}});
                block(s.body);
                const auto nextIp = code.size();
                code.push_back({Op::ForEachNext, s.span, nullptr, nullptr, current + 1});
                --scopeDepth;
                --forEachDepth;
                const auto endIp = code.size();
                code[current].target = endIp;
                for (std::size_t patch : loopTargets.back().contPatches) code[patch].target = nextIp;
                for (std::size_t patch : loopTargets.back().breakPatches) code[patch].target = endIp;
                loopTargets.pop_back();
                pendingOperands = stmtBase;
                return;
            }
            case StmtKind::Break: {
                auto& lt = findLoop(s.label);
                if (stmtBase > lt.operandDepth)
                    code.push_back({Op::DropOperands, s.span, nullptr, nullptr, stmtBase - lt.operandDepth});
                for (std::size_t i = lt.breakScope; i < scopeDepth; ++i) code.push_back({Op::Leave, s.span});
                for (std::size_t i = lt.breakRange; i < rangeDepth; ++i) code.push_back({Op::RangePop, s.span});
                for (std::size_t i = lt.breakForEach; i < forEachDepth; ++i) code.push_back({Op::ForEachPop, s.span});
                lt.breakPatches.push_back(code.size());
                code.push_back({Op::Jump, s.span});
                return;
            }
            case StmtKind::Continue: {
                auto& lt = findLoop(s.label);
                if (stmtBase > lt.operandDepth)
                    code.push_back({Op::DropOperands, s.span, nullptr, nullptr, stmtBase - lt.operandDepth});
                for (std::size_t i = lt.contScope; i < scopeDepth; ++i) code.push_back({Op::Leave, s.span});
                for (std::size_t i = lt.contRange; i < rangeDepth; ++i) code.push_back({Op::RangePop, s.span});
                for (std::size_t i = lt.contForEach; i < forEachDepth; ++i) code.push_back({Op::ForEachPop, s.span});
                if (lt.isFor) {
                    lt.contPatches.push_back(code.size());
                    code.push_back({Op::Jump, s.span});
                } else {
                    code.push_back({Op::Jump, s.span, nullptr, nullptr, lt.contTarget});
                }
                return;
            }
            case StmtKind::Match: {
                std::vector<std::size_t> exits;
                for (const auto& arm : s.arms) {
                    const auto attempt = code.size();
                    code.push_back({Op::MatchArm, arm.span, nullptr, nullptr, 0, &arm});
                    ++scopeDepth;
                    pendingOperands = stmtBase;
                    block(arm.body);
                    --scopeDepth;
                    code.push_back({Op::Leave, arm.span});
                    exits.push_back(code.size());
                    code.push_back({Op::Jump, arm.span});
                    code[attempt].target = code.size();
                }
                for (auto end : exits) code[end].target = code.size();
                pendingOperands = stmtBase;
                return;
            }
        }
    }
};

[[noreturn]] void overflow(Span span) {
    fail("R_OVERFLOW", "Desbordamiento de int", span, "9223372036854775807 - 1");
}

Value makeIntValue(Type t, std::uint64_t rawBits) {
    if (t == Type::Int) return Value{static_cast<Integer>(rawBits)};
    return Value{ExactInt{t, rawBits & maskForType(t)}};
}

Value makeFloatValue(Type t, double d) {
    if (t == Type::F32) return Value{static_cast<float>(d)};
    return Value{d};
}

Value evalIntLiteralVM(const Program& prog, const Expr& expr) {
    const Type t = (expr.type != Type::Unknown && Program::isIntType(expr.type)) ? expr.type : Type::Int;
    return makeIntValue(t, parseExactIntLiteral(prog, expr, t, false));
}

Value evalFloatLiteralVM(const Expr& expr) {
    if (expr.type == Type::F32) return Value{static_cast<float>(parseExactFloatLiteral(expr, Type::F32))};
    return Value{parseFloat(expr)};
}

std::uintptr_t vmRawPtrAddr(const RawPtrVal& p) {
    if (p.addr != 0) return p.addr;
    if (p.vmSlot != nullptr) return reinterpret_cast<std::uintptr_t>(p.vmSlot) + p.byteOffset;
    if (p.vmArr != nullptr) return reinterpret_cast<std::uintptr_t>(p.vmArr.get()) + p.byteOffset;
    return 0;
}

std::uint64_t encodeSimdLane(Type elemT, const Value& v) {
    if (elemT == Type::Int) return static_cast<std::uint64_t>(std::get<Integer>(v));
    if (Program::isIntType(elemT)) return std::get<ExactInt>(v).bits;
    if (elemT == Type::F32) return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(std::get<float>(v)));
    if (elemT == Type::Float) return std::bit_cast<std::uint64_t>(std::get<double>(v));
    throw std::logic_error("invalid SIMD lane type");
}

Value decodeSimdLane(Type elemT, std::uint64_t bits) {
    if (elemT == Type::Int) return Value{static_cast<Integer>(bits)};
    if (Program::isIntType(elemT)) return makeIntValue(elemT, bits);
    if (elemT == Type::F32) return Value{std::bit_cast<float>(static_cast<std::uint32_t>(bits & 0xFFFFFFFFULL))};
    if (elemT == Type::Float) return Value{std::bit_cast<double>(bits)};
    throw std::logic_error("invalid SIMD lane type");
}

SimdVal evalSimdBinary(std::string_view op, const SimdVal& l, const SimdVal& r) {
    SimdVal out{l.elemType, l.lanes, {}};
    const Type elemT = l.elemType;
    if (elemT == Type::Float) {
        for (unsigned k = 0; k < l.lanes; ++k) {
            const double a = std::bit_cast<double>(l.lane(k));
            const double b = std::bit_cast<double>(r.lane(k));
            double res = 0.0;
            if (op == "+") res = a + b;
            else if (op == "-") res = a - b;
            else if (op == "*") res = a * b;
            else res = a / b;
            out.setLane(k, std::bit_cast<std::uint64_t>(res));
        }
        return out;
    }
    if (elemT == Type::F32) {
        for (unsigned k = 0; k < l.lanes; ++k) {
            const float a = std::bit_cast<float>(static_cast<std::uint32_t>(l.lane(k)));
            const float b = std::bit_cast<float>(static_cast<std::uint32_t>(r.lane(k)));
            float res = 0.0f;
            if (op == "+") res = a + b;
            else if (op == "-") res = a - b;
            else if (op == "*") res = a * b;
            else res = a / b;
            out.setLane(k, std::bit_cast<std::uint32_t>(res));
        }
        return out;
    }
    const unsigned w = Program::intBitWidth(elemT);
    const bool isSigned = Program::isSignedInt(elemT);
    for (unsigned k = 0; k < l.lanes; ++k) {
        const std::uint64_t a = l.lane(k);
        const std::uint64_t b = r.lane(k);
        std::uint64_t res = 0;
        if (op == "+") res = a + b;
        else if (op == "-") res = a - b;
        else if (op == "*") res = a * b;
        else if (op == "&") res = a & b;
        else if (op == "|") res = a | b;
        else if (op == "^") res = a ^ b;
        else if (op == "<<") res = a << static_cast<unsigned>(b & (w - 1u));
        else if (op == ">>" && isSigned) {
            const Integer sa = signExtendWidth(a, w);
            res = static_cast<std::uint64_t>(sa >> static_cast<unsigned>(b & (w - 1u)));
        } else {
            res = a >> static_cast<unsigned>(b & (w - 1u));
        }
        out.setLane(k, res);
    }
    return out;
}

template <class U>
std::uint64_t applyBitIntrinsicTyped(std::string_view fn, std::uint64_t x, std::uint64_t s) {
    constexpr unsigned w = sizeof(U) * 8u;
    const U uv = static_cast<U>(x);
    if (fn == "clz") return static_cast<std::uint64_t>(std::countl_zero(uv));
    if (fn == "ctz") return static_cast<std::uint64_t>(std::countr_zero(uv));
    if (fn == "popcnt") return static_cast<std::uint64_t>(std::popcount(uv));
    if (fn == "bswap") {
        if constexpr (w == 8) return static_cast<std::uint64_t>(uv);
        else return static_cast<std::uint64_t>(std::byteswap(uv));
    }
    const int sc = static_cast<int>(s & (w - 1u));
    if (fn == "rotl") return static_cast<std::uint64_t>(std::rotl(uv, sc));
    return static_cast<std::uint64_t>(std::rotr(uv, sc));
}

Value evalBitIntrinsicVM(Type t, std::string_view fn, std::uint64_t x, std::uint64_t s = 0) {
    const unsigned w = Program::intBitWidth(t);
    std::uint64_t bits = 0;
    switch (w) {
        case 8u: bits = applyBitIntrinsicTyped<std::uint8_t>(fn, x, s); break;
        case 16u: bits = applyBitIntrinsicTyped<std::uint16_t>(fn, x, s); break;
        case 32u: bits = applyBitIntrinsicTyped<std::uint32_t>(fn, x, s); break;
        default: bits = applyBitIntrinsicTyped<std::uint64_t>(fn, x, s); break;
    }
    return decodeSimdLane(t, bits);
}

Value binary(const Expr& expr, Value lhs, Value rhs) {
    const auto& op = expr.text;
    if (op == "==" || op == "!=") {
        bool eq = false;
        if (auto* la = std::get_if<AggregatePtr>(&lhs)) {
            eq = (*la)->tag == std::get<AggregatePtr>(rhs)->tag;
        } else if (auto* lf = std::get_if<double>(&lhs)) {
            eq = (*lf == std::get<double>(rhs));
        } else if (auto* lf32 = std::get_if<float>(&lhs)) {
            eq = (*lf32 == std::get<float>(rhs));
        } else if (auto* lei = std::get_if<ExactInt>(&lhs)) {
            eq = (lei->bits == std::get<ExactInt>(rhs).bits);
        } else if (auto* lp = std::get_if<RawPtrVal>(&lhs)) {
            eq = (vmRawPtrAddr(*lp) == vmRawPtrAddr(std::get<RawPtrVal>(rhs)));
        } else if (auto* lsv = std::get_if<SimdVal>(&lhs)) {
            const auto& rsv = std::get<SimdVal>(rhs);
            eq = (lsv->elemType == rsv.elemType && lsv->lanes == rsv.lanes);
            for (unsigned k = 0; eq && k < lsv->lanes; ++k) {
                if (lsv->elemType == Type::Float)
                    eq = (std::bit_cast<double>(lsv->lane(k)) == std::bit_cast<double>(rsv.lane(k)));
                else if (lsv->elemType == Type::F32)
                    eq = (std::bit_cast<float>(static_cast<std::uint32_t>(lsv->lane(k))) ==
                          std::bit_cast<float>(static_cast<std::uint32_t>(rsv.lane(k))));
                else
                    eq = (lsv->lane(k) == rsv.lane(k));
            }
        } else {
            eq = (lhs == rhs);
        }
        return op == "==" ? eq : !eq;
    }
    if (auto* lsv = std::get_if<SimdVal>(&lhs)) {
        return Value{evalSimdBinary(op, *lsv, std::get<SimdVal>(rhs))};
    }
    if (auto* lf = std::get_if<double>(&lhs)) {
        const double rf = std::get<double>(rhs);
        if (op == "<") return *lf < rf;
        if (op == "<=") return *lf <= rf;
        if (op == ">") return *lf > rf;
        if (op == ">=") return *lf >= rf;
        if (op == "+") return *lf + rf;
        if (op == "-") return *lf - rf;
        if (op == "*") return *lf * rf;
        return *lf / rf;
    }
    if (auto* lf = std::get_if<float>(&lhs)) {
        const float rf = std::get<float>(rhs);
        if (op == "<") return *lf < rf;
        if (op == "<=") return *lf <= rf;
        if (op == ">") return *lf > rf;
        if (op == ">=") return *lf >= rf;
        if (op == "+") return *lf + rf;
        if (op == "-") return *lf - rf;
        if (op == "*") return *lf * rf;
        return *lf / rf;
    }
    if (auto* ls = std::get_if<std::string_view>(&lhs)) {
        const auto rs = std::get<std::string_view>(rhs);
        if (op == "<") return *ls < rs;
        if (op == "<=") return *ls <= rs;
        if (op == ">") return *ls > rs;
        return *ls >= rs;
    }
    if (auto* lei = std::get_if<ExactInt>(&lhs)) {
        const auto rei = std::get<ExactInt>(rhs);
        const Type ty = lei->type;
        const unsigned w = Program::intBitWidth(ty);
        const bool isSigned = Program::isSignedInt(ty);
        if (isSigned) {
            const Integer sa = signExtendWidth(lei->bits, w);
            const Integer sb = signExtendWidth(rei.bits, w);
            if (op == "<") return sa < sb;
            if (op == "<=") return sa <= sb;
            if (op == ">") return sa > sb;
            if (op == ">=") return sa >= sb;
            if (op == "&") return makeIntValue(ty, lei->bits & rei.bits);
            if (op == "|") return makeIntValue(ty, lei->bits | rei.bits);
            if (op == "^") return makeIntValue(ty, lei->bits ^ rei.bits);
            if (op == "<<") return makeIntValue(ty, lei->bits << checkShiftCount(sb, expr.span, w));
            if (op == ">>") return makeIntValue(ty, static_cast<std::uint64_t>(sa >> checkShiftCount(sb, expr.span, w)));
            if (op == ">>>") return makeIntValue(ty, lei->bits >> checkShiftCount(sb, expr.span, w));
            if (op == "/" || op == "%") {
                if (sb == 0) fail("R_DIV_ZERO", "Divisor cero", expr.span, "let half = 4 / 2");
                if (sa == minSignedForWidth(w) && sb == -1) overflow(expr.span);
                return makeIntValue(ty, static_cast<std::uint64_t>(op == "/" ? sa / sb : sa % sb));
            }
            auto res = (op == "+") ? checkedAddWidth(sa, sb, w)
                     : (op == "-") ? checkedSubWidth(sa, sb, w)
                                   : checkedMulWidth(sa, sb, w);
            if (!res) overflow(expr.span);
            return makeIntValue(ty, static_cast<std::uint64_t>(*res));
        }
        const std::uint64_t a = lei->bits, b = rei.bits;
        if (op == "<") return a < b;
        if (op == "<=") return a <= b;
        if (op == ">") return a > b;
        if (op == ">=") return a >= b;
        if (op == "&") return makeIntValue(ty, a & b);
        if (op == "|") return makeIntValue(ty, a | b);
        if (op == "^") return makeIntValue(ty, a ^ b);
        if (op == "<<" || op == ">>" || op == ">>>") {
            if (b >= w)
                fail("R_OVERFLOW", "Desplazamiento fuera de rango [0, " + std::to_string(w - 1) + "]", expr.span, "x << 1");
            const unsigned sc = static_cast<unsigned>(b);
            if (op == "<<") return makeIntValue(ty, a << sc);
            return makeIntValue(ty, a >> sc);
        }
        if (op == "/" || op == "%") {
            if (b == 0) fail("R_DIV_ZERO", "Divisor cero", expr.span, "let half = 4 / 2");
            return makeIntValue(ty, op == "/" ? a / b : a % b);
        }
        auto res = (op == "+") ? checkedUAddWidth(a, b, w)
                 : (op == "-") ? checkedUSubWidth(a, b)
                               : checkedUMulWidth(a, b, w);
        if (!res) overflow(expr.span);
        return makeIntValue(ty, *res);
    }
    const auto a = std::get<Integer>(lhs), b = std::get<Integer>(rhs);
    if (op == "<") return a < b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == ">=") return a >= b;
    if (op == "&") return a & b;
    if (op == "|") return a | b;
    if (op == "^") return a ^ b;
    if (op == "<<") return static_cast<Integer>(static_cast<std::uint64_t>(a) << checkShiftCount(b, expr.span));
    if (op == ">>") return a >> checkShiftCount(b, expr.span);
    if (op == ">>>") return static_cast<Integer>(static_cast<std::uint64_t>(a) >> checkShiftCount(b, expr.span));
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
struct ForEachIter { ArrayPtr array; std::size_t cursor, end; };

[[noreturn]] void allocationFailed(Span span) {
    fail("R_ALLOC", "La reserva del array excede los recursos disponibles", span, "repeat(0, 8)");
}

ArrayPtr repeatArray(Value element, Integer count, Span span) {
    if (count < 0) fail("R_ARRAY_SIZE", "La longitud debe ser no negativa", span, "repeat(0, 8)");
    if (count > maxArrayLength) allocationFailed(span);
    try {
        auto buf = std::make_shared<ArrayBuffer>();
        buf->items.assign(static_cast<std::size_t>(count), std::move(element));
        buf->cap = static_cast<std::size_t>(count);
        return buf;
    } catch (const std::bad_alloc&) { allocationFailed(span); }
}

std::size_t arrayIndex(const ArrayBuffer& array, Integer index, Span where) {
    if (index < 0 || static_cast<std::uint64_t>(index) >= array.items.size())
        fail("R_BOUNDS", "El índice está fuera del array", where, "a[len(a) - 1]");
    return static_cast<std::size_t>(index);
}
struct LocalSlot {
    Value value;
    Value* ref = nullptr;
    bool owned = true;
    Value& target() { return ref ? *ref : value; }
    const Value& target() const { return ref ? *ref : value; }
};
struct PlaceStepVM {
    bool isIndex = false;
    std::size_t fieldIdx = 0;
    Integer indexVal = 0;
};
struct WriteBack {
    Value* rootSlot = nullptr;
    std::vector<PlaceStepVM> placeSteps;
    std::shared_ptr<Value> tempBox;
    DynPtr dynKeepAlive;
    Span span{};
};
struct Frame {
    const Code* code;
    std::size_t ip, base;
    Scopes<LocalSlot> locals;
    std::vector<std::vector<std::string>> declOrder{1};
    std::vector<Range> ranges;
    std::vector<ForEachIter> forEachs;
    const Function* fn = nullptr;
    std::vector<WriteBack> writeBacks{};
    std::vector<Integer> placeIdxStack{};
};

struct Interpreter {
    const Program& programRef;
    Functions functions;
    std::map<std::pair<Type, Type>, std::vector<std::string>> dynDispatch;
    std::unordered_map<const Function*, Code> bodies;
    std::unordered_map<std::string, Integer> enumValues;
    std::unordered_map<std::string, std::size_t> enumVariantIndices;
    std::vector<std::string> programArgs;
    std::deque<std::string> ownedStrings;
    std::deque<Frame> frames;
    std::vector<Value> operands;
    std::ostream& out;
    std::istream* input = nullptr;
    Limits limits;
    LayoutCalculator layoutCalc;
    struct VmAlloc { std::size_t bytes = 0; bool aligned = false; };
    std::unordered_map<std::uintptr_t, VmAlloc> vmAllocs;
    mutable std::unordered_map<std::uintptr_t, RawPtrVal> vmAddrMap;
    std::uintptr_t nextFnSentinel = 0x100000ULL;
    std::unordered_map<std::string, std::uintptr_t> fnSentinels;

    ~Interpreter() {
        for (const auto& [addr, info] : vmAllocs) {
            void* p = reinterpret_cast<void*>(addr);
#ifdef _WIN32
            if (info.aligned) _aligned_free(p);
            else std::free(p);
#else
            (void)info;
            std::free(p);
#endif
        }
    }

    Interpreter(const Program& program, std::ostream& stream, Limits budget)
        : programRef(program), programArgs(program.programArgs), out(stream), limits(budget), layoutCalc(program) {
        for (const auto& d : program.data) {
            if (!d.isEnum) continue;
            const auto dname = program.nameOf(d.type);
            for (std::size_t vi = 0; vi < d.variants.size(); ++vi) {
                enumValues.emplace(dname + "." + d.variants[vi].name, d.variants[vi].enumValue);
                enumVariantIndices.emplace(dname + "." + d.variants[vi].name, vi);
            }
        }
        for (const auto& fn : program.functions) {
            if (!fn.isTest) functions.emplace(fn.name, &fn);
        }
        for (const auto& dv : program.dynVtables) {
            dynDispatch.emplace(std::make_pair(dv.iface, dv.target), dv.methods);
        }
        for (const auto& fn : program.functions) {
            if (fn.isExternDecl) continue;
            EvaluationOrder order(functions);
            order.currentFn = &fn;
            order.inRawBlock = (fn.safety == SafetyTier::Raw);
            if (fn.safety != SafetyTier::Raw) {
                for (const auto& req : fn.requiresClauses) {
                    order.expression(req);
                    order.code.push_back({Op::ContractCheck, req.span});
                    --order.pendingOperands;
                }
            }
            if (!fn.ensuresClauses.empty() && fn.safety != SafetyTier::Raw) {
                order.scoped(fn.body, fn.span);
                order.code.push_back({Op::BeginEnsures, fn.span, nullptr, nullptr, 0u});
                for (const auto& ens : fn.ensuresClauses) {
                    order.expression(ens);
                    order.code.push_back({Op::ContractCheck, ens.span});
                    --order.pendingOperands;
                }
                order.code.push_back({Op::EndEnsuresReturn, fn.span, nullptr, nullptr, 0u});
            } else {
                order.block(fn.body);
                order.code.push_back({Op::Return, fn.span});
            }
            bodies.emplace(&fn, std::move(order.code));
        }
    }

    static constexpr std::uint64_t foldSeed = 0x243F6A8885A308D3ULL;
    static constexpr std::uint64_t foldMult = 0x2D358DCCAA6C78A5ULL;
    static constexpr std::uint64_t foldMult2 = 0x8BB84B93962EACC9ULL;

    static std::uint64_t foldedMul(std::uint64_t a, std::uint64_t b) {
#if defined(__SIZEOF_INT128__)
        const __uint128_t prod = static_cast<__uint128_t>(a) * static_cast<__uint128_t>(b);
        return static_cast<std::uint64_t>(prod) ^ static_cast<std::uint64_t>(prod >> 64);
#elif defined(_MSC_VER) && defined(_M_X64)
        std::uint64_t hi = 0;
        const std::uint64_t lo = _umul128(a, b, &hi);
        return lo ^ hi;
#else
        const std::uint64_t aLo = static_cast<std::uint32_t>(a), aHi = a >> 32;
        const std::uint64_t bLo = static_cast<std::uint32_t>(b), bHi = b >> 32;
        const std::uint64_t p00 = aLo * bLo, p01 = aLo * bHi, p10 = aHi * bLo, p11 = aHi * bHi;
        const std::uint64_t mid = (p00 >> 32) + static_cast<std::uint32_t>(p01) + static_cast<std::uint32_t>(p10);
        const std::uint64_t lo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
        const std::uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        return lo ^ hi;
#endif
    }

    static std::uint64_t hashU64(std::uint64_t x, std::uint64_t seed) {
        return foldedMul(x ^ seed, foldMult);
    }

    static std::uint64_t hashBytes(const char* p, std::size_t n, std::uint64_t seed) {
        std::uint64_t h = foldedMul(seed ^ static_cast<std::uint64_t>(n), foldMult2);
        if (n == 0) return h;
        while (n >= 16) {
            std::uint64_t a = 0, b = 0;
            std::memcpy(&a, p, 8);
            std::memcpy(&b, p + 8, 8);
            h = foldedMul(a ^ h, b ^ foldMult);
            p += 16;
            n -= 16;
        }
        if (n >= 8) {
            std::uint64_t a = 0, b = 0;
            std::memcpy(&a, p, 8);
            std::memcpy(&b, p + (n - 8), 8);
            h = foldedMul(a ^ h, b ^ foldMult2);
        } else if (n >= 4) {
            std::uint32_t a = 0, b = 0;
            std::memcpy(&a, p, 4);
            std::memcpy(&b, p + (n - 4), 4);
            h = foldedMul(static_cast<std::uint64_t>(a) ^ h, static_cast<std::uint64_t>(b) ^ foldMult2);
        } else if (n > 0) {
            const auto* up = reinterpret_cast<const std::uint8_t*>(p);
            const std::uint64_t tail = static_cast<std::uint64_t>(up[0]) |
                                       (static_cast<std::uint64_t>(up[n >> 1]) << 8) |
                                       (static_cast<std::uint64_t>(up[n - 1]) << 16);
            h = foldedMul(tail ^ h, foldMult);
        }
        return h;
    }

    static std::uint64_t sipHash13(const std::uint8_t* p, std::size_t len, std::uint64_t k0, std::uint64_t k1) {
        auto sipRound = [](std::uint64_t& v0, std::uint64_t& v1, std::uint64_t& v2, std::uint64_t& v3) {
            v0 += v1; v1 = std::rotl(v1, 13); v1 ^= v0; v0 = std::rotl(v0, 32);
            v2 += v3; v3 = std::rotl(v3, 16); v3 ^= v2;
            v0 += v3; v3 = std::rotl(v3, 21); v3 ^= v0;
            v2 += v1; v1 = std::rotl(v1, 17); v1 ^= v2; v2 = std::rotl(v2, 32);
        };
        std::uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
        std::uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
        std::uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
        std::uint64_t v3 = 0x7465646279746573ULL ^ k1;
        const std::size_t blocks = len / 8;
        for (std::size_t i = 0; i < blocks; ++i) {
            std::uint64_t m = 0;
            std::memcpy(&m, p + i * 8, 8);
            v3 ^= m;
            sipRound(v0, v1, v2, v3);
            v0 ^= m;
        }
        const std::uint8_t* tail = p + blocks * 8;
        std::uint64_t b = static_cast<std::uint64_t>(len) << 56;
        for (std::size_t k = 0; k < (len & 7u); ++k) b |= static_cast<std::uint64_t>(tail[k]) << (k * 8);
        v3 ^= b;
        sipRound(v0, v1, v2, v3);
        v0 ^= b;
        v2 ^= 0xFFu;
        sipRound(v0, v1, v2, v3);
        sipRound(v0, v1, v2, v3);
        sipRound(v0, v1, v2, v3);
        return v0 ^ v1 ^ v2 ^ v3;
    }

    static std::uint64_t randomTableSeed() {
        std::random_device rd;
        std::uint64_t s = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        if (s == 0) s = foldSeed;
        return s;
    }

    static std::uint64_t floatTotalKey(double d) {
        const std::uint64_t u = std::bit_cast<std::uint64_t>(d);
        const std::uint64_t m = static_cast<std::uint64_t>(-static_cast<std::int64_t>(u >> 63)) | 0x8000000000000000ULL;
        return u ^ m;
    }

    static std::uint32_t float32TotalKey(float f) {
        const std::uint32_t u = std::bit_cast<std::uint32_t>(f);
        const std::uint32_t m = static_cast<std::uint32_t>(-static_cast<std::int32_t>(u >> 31)) | 0x80000000U;
        return u ^ m;
    }

    TableKeyKind tableKeyKind(Type collT) const {
        const Type kt = programRef.isMap(collT) ? programRef.mapKey(collT) : programRef.setElem(collT);
        if (kt == Type::Int) return TableKeyKind::I64;
        if (Program::isIntType(kt)) return TableKeyKind::ExactInt;
        if (kt == Type::Bool) return TableKeyKind::Bool;
        if (kt == Type::Str) return TableKeyKind::Str;
        if (programRef.isHandle(kt)) return TableKeyKind::Handle;
        return TableKeyKind::Enum;
    }

    std::uint64_t hashTableKey(const SwissTableBuf& t, const Value& v) const {
        const std::uint64_t seed = t.secure ? t.seed : foldSeed;
        if (t.keyKind == TableKeyKind::Str) {
            const auto sv = std::get<std::string_view>(v);
            if (t.secure)
                return sipHash13(reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size(),
                                 seed, foldedMul(seed, foldMult));
            return hashBytes(sv.data(), sv.size(), seed);
        }
        std::uint64_t k = 0;
        if (t.keyKind == TableKeyKind::I64) k = static_cast<std::uint64_t>(std::get<Integer>(v));
        else if (t.keyKind == TableKeyKind::ExactInt) k = std::get<ExactInt>(v).bits;
        else if (t.keyKind == TableKeyKind::Bool) k = std::get<bool>(v) ? 1ULL : 0ULL;
        else if (t.keyKind == TableKeyKind::Handle) {
            const auto h = std::get<HandleValue>(v);
            k = (static_cast<std::uint64_t>(h.gen) << 32) | static_cast<std::uint64_t>(h.idx);
        }
        else k = static_cast<std::uint64_t>(enumVariantIndices.at(std::string(std::get<AggregatePtr>(v)->tag)));
        if (t.secure) {
            std::uint8_t buf[8];
            std::memcpy(buf, &k, 8);
            return sipHash13(buf, 8, seed, foldedMul(seed, foldMult));
        }
        return hashU64(k, seed);
    }

    static bool tableKeyEq(const SwissTableBuf& t, const Value& a, const Value& b) {
        if (t.keyKind == TableKeyKind::Str) return std::get<std::string_view>(a) == std::get<std::string_view>(b);
        if (t.keyKind == TableKeyKind::I64) return std::get<Integer>(a) == std::get<Integer>(b);
        if (t.keyKind == TableKeyKind::ExactInt) return std::get<ExactInt>(a).bits == std::get<ExactInt>(b).bits;
        if (t.keyKind == TableKeyKind::Bool) return std::get<bool>(a) == std::get<bool>(b);
        if (t.keyKind == TableKeyKind::Handle) return std::get<HandleValue>(a) == std::get<HandleValue>(b);
        return std::get<AggregatePtr>(a)->tag == std::get<AggregatePtr>(b)->tag;
    }

    static void poolReserve(PoolObject& p, Integer minCap, Span span) {
        if (minCap < 0) fail("R_ARRAY_SIZE", "La longitud debe ser no negativa", span, "reserve(mut p, 8)");
        if (minCap > maxPoolCap) allocationFailed(span);
        if (static_cast<std::size_t>(minCap) <= p.cap) return;
        std::size_t newCap = p.cap == 0 ? 8 : p.cap;
        while (newCap < static_cast<std::size_t>(minCap)) newCap *= 2;
        if (newCap > static_cast<std::size_t>(maxPoolCap)) newCap = static_cast<std::size_t>(maxPoolCap);
        try { p.slots.resize(newCap); }
        catch (const std::bad_alloc&) { allocationFailed(span); }
        p.cap = newCap;
    }

    static std::uint32_t tableMaxLoad(std::uint32_t buckets) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(buckets) * 7ULL) >> 3);
    }

    static std::uint16_t matchByte(const std::vector<std::uint8_t>& ctrl, std::uint32_t pos, std::uint8_t target) {
        std::uint16_t m = 0;
        for (std::uint32_t k = 0; k < 16u; ++k)
            if (ctrl[pos + k] == target) m |= static_cast<std::uint16_t>(1u << k);
        return m;
    }

    static void setCtrl(SwissTableBuf& t, std::uint32_t idx, std::uint8_t c) {
        t.ctrl[idx] = c;
        if (idx < 16u) t.ctrl[t.buckets + idx] = c;
    }

    void tableRehash(SwissTableBuf& t, std::uint32_t newBuckets, Span span) const {
        if (newBuckets < 16u || newBuckets > (1u << 24)) allocationFailed(span);
        std::vector<std::uint8_t> newCtrl;
        std::vector<Value> newKeys, newVals;
        try {
            newCtrl.assign(static_cast<std::size_t>(newBuckets) + 16u, 0xFFu);
            newKeys.resize(newBuckets);
            if (t.isMap) newVals.resize(newBuckets);
        } catch (const std::bad_alloc&) { allocationFailed(span); }
        const std::uint32_t mask = newBuckets - 1u;
        for (std::uint32_t oldIdx = 0; oldIdx < t.buckets; ++oldIdx) {
            if ((t.ctrl[oldIdx] & 0x80u) != 0u) continue;
            const std::uint64_t h = hashTableKey(t, t.keys[oldIdx]);
            const std::uint8_t h2 = static_cast<std::uint8_t>((h >> 57) & 0x7Fu);
            std::uint32_t pos = static_cast<std::uint32_t>(h) & mask;
            std::uint32_t step = 0, dstIdx = 0;
            for (;;) {
                const std::uint16_t em = matchByte(newCtrl, pos, 0xFFu);
                if (em != 0u) {
                    dstIdx = (pos + static_cast<std::uint32_t>(std::countr_zero(em))) & mask;
                    break;
                }
                step += 16u;
                pos = (pos + step) & mask;
            }
            newCtrl[dstIdx] = h2;
            if (dstIdx < 16u) newCtrl[newBuckets + dstIdx] = h2;
            newKeys[dstIdx] = std::move(t.keys[oldIdx]);
            if (t.isMap) newVals[dstIdx] = std::move(t.vals[oldIdx]);
        }
        t.ctrl = std::move(newCtrl);
        t.keys = std::move(newKeys);
        t.vals = std::move(newVals);
        t.buckets = newBuckets;
        t.growthLeft = tableMaxLoad(newBuckets) - t.len;
    }

    void tableReserve(SwissTableBuf& t, Integer minCap, Span span) const {
        if (minCap < 0) fail("R_ARRAY_SIZE", "La longitud debe ser no negativa", span, "reserve(mut m, 8)");
        if (minCap > maxTableCap) allocationFailed(span);
        if (minCap == 0) return;
        std::uint64_t b = 16;
        while (((b * 7ULL) >> 3) < static_cast<std::uint64_t>(minCap)) {
            if (b >= maxTableBuckets) allocationFailed(span);
            b <<= 1;
        }
        const auto needBuckets = static_cast<std::uint32_t>(b);
        if (t.buckets == 0u || t.buckets < needBuckets) {
            tableRehash(t, needBuckets, span);
        } else if (static_cast<std::uint64_t>(minCap) > t.len &&
                   static_cast<std::uint64_t>(t.growthLeft) < static_cast<std::uint64_t>(minCap) - t.len) {
            tableRehash(t, t.buckets, span);
        }
    }

    std::optional<Value> tableFind(const SwissTableBuf& t, const Value& key) const {
        if (t.len == 0u || t.buckets == 0u) return {};
        const std::uint32_t mask = t.buckets - 1u;
        const std::uint64_t h = hashTableKey(t, key);
        const std::uint8_t h2 = static_cast<std::uint8_t>((h >> 57) & 0x7Fu);
        std::uint32_t pos = static_cast<std::uint32_t>(h) & mask;
        std::uint32_t step = 0;
        for (;;) {
            std::uint16_t m = matchByte(t.ctrl, pos, h2);
            while (m != 0u) {
                const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(m));
                const std::uint32_t idx = (pos + bit) & mask;
                if (tableKeyEq(t, t.keys[idx], key)) return t.isMap ? t.vals[idx] : Value{true};
                m &= static_cast<std::uint16_t>(m - 1u);
            }
            if (matchByte(t.ctrl, pos, 0xFFu) != 0u) return {};
            step += 16u;
            pos = (pos + step) & mask;
        }
    }

    bool tableInsert(TablePtr& tbl, const Value& key, Value val, Span span) const {
        if (tbl.use_count() > 1) {
            try { tbl = std::make_shared<SwissTableBuf>(*tbl); }
            catch (const std::bad_alloc&) { allocationFailed(span); }
        }
        SwissTableBuf& t = *tbl;
        if (t.buckets == 0u) tableReserve(t, 1, span);
        std::uint32_t mask = t.buckets - 1u;
        const std::uint64_t h = hashTableKey(t, key);
        const std::uint8_t h2 = static_cast<std::uint8_t>((h >> 57) & 0x7Fu);
        std::uint32_t pos = static_cast<std::uint32_t>(h) & mask;
        std::uint32_t step = 0;
        std::uint32_t firstDel = UINT32_MAX, emptyIdx = UINT32_MAX;
        for (;;) {
            std::uint16_t m = matchByte(t.ctrl, pos, h2);
            while (m != 0u) {
                const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(m));
                const std::uint32_t idx = (pos + bit) & mask;
                if (tableKeyEq(t, t.keys[idx], key)) {
                    if (t.isMap) t.vals[idx] = std::move(val);
                    return false;
                }
                m &= static_cast<std::uint16_t>(m - 1u);
            }
            if (firstDel == UINT32_MAX) {
                const std::uint16_t dm = matchByte(t.ctrl, pos, 0x80u);
                if (dm != 0u) firstDel = (pos + static_cast<std::uint32_t>(std::countr_zero(dm))) & mask;
            }
            const std::uint16_t em = matchByte(t.ctrl, pos, 0xFFu);
            if (em != 0u) {
                emptyIdx = (pos + static_cast<std::uint32_t>(std::countr_zero(em))) & mask;
                break;
            }
            step += 16u;
            pos = (pos + step) & mask;
        }
        std::uint32_t target = firstDel;
        if (target == UINT32_MAX) {
            if (t.growthLeft == 0u) {
                const std::uint32_t newB = (t.len * 2u < t.buckets) ? t.buckets : (t.buckets * 2u);
                tableRehash(t, newB, span);
                mask = t.buckets - 1u;
                pos = static_cast<std::uint32_t>(h) & mask;
                step = 0;
                for (;;) {
                    const std::uint16_t em = matchByte(t.ctrl, pos, 0xFFu);
                    if (em != 0u) {
                        emptyIdx = (pos + static_cast<std::uint32_t>(std::countr_zero(em))) & mask;
                        break;
                    }
                    step += 16u;
                    pos = (pos + step) & mask;
                }
            }
            target = emptyIdx;
        }
        t.keys[target] = key;
        if (t.isMap) t.vals[target] = std::move(val);
        setCtrl(t, target, h2);
        t.len += 1u;
        if (firstDel == UINT32_MAX) t.growthLeft -= 1u;
        return true;
    }

    bool tableDel(TablePtr& tbl, const Value& key, Span span) const {
        if (tbl->len == 0u || tbl->buckets == 0u) return false;
        if (tbl.use_count() > 1) {
            try { tbl = std::make_shared<SwissTableBuf>(*tbl); }
            catch (const std::bad_alloc&) { allocationFailed(span); }
        }
        SwissTableBuf& t = *tbl;
        const std::uint32_t mask = t.buckets - 1u;
        const std::uint64_t h = hashTableKey(t, key);
        const std::uint8_t h2 = static_cast<std::uint8_t>((h >> 57) & 0x7Fu);
        std::uint32_t pos = static_cast<std::uint32_t>(h) & mask;
        std::uint32_t step = 0;
        for (;;) {
            std::uint16_t m = matchByte(t.ctrl, pos, h2);
            while (m != 0u) {
                const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(m));
                const std::uint32_t idx = (pos + bit) & mask;
                if (tableKeyEq(t, t.keys[idx], key)) {
                    t.keys[idx] = Value{};
                    if (t.isMap) t.vals[idx] = Value{};
                    setCtrl(t, idx, 0x80u);
                    t.len -= 1u;
                    return true;
                }
                m &= static_cast<std::uint16_t>(m - 1u);
            }
            if (matchByte(t.ctrl, pos, 0xFFu) != 0u) return false;
            step += 16u;
            pos = (pos + step) & mask;
        }
    }

    std::string_view storeString(std::string s) {
        ownedStrings.push_back(std::move(s));
        return ownedStrings.back();
    }

    std::string formatScalar(const Value& v) const {
        if (auto* s = std::get_if<std::string_view>(&v)) return std::string(*s);
        if (auto* n = std::get_if<Integer>(&v)) return std::to_string(*n);
        if (auto* ei = std::get_if<ExactInt>(&v)) {
            if (Program::isSignedInt(ei->type))
                return std::to_string(signExtendWidth(ei->bits, Program::intBitWidth(ei->type)));
            return std::to_string(ei->bits);
        }
        if (auto* f = std::get_if<float>(&v)) return formatFloat(static_cast<double>(*f));
        if (auto* d = std::get_if<double>(&v)) return formatFloat(*d);
        return std::get<bool>(v) ? "true" : "false";
    }

    Value castNumericVM(Type toT, Type fromT, const Value& v, Span span) const {
        if (toT == fromT) return v;
        if (!Program::isNumericType(fromT)) {
            const auto ag = std::get<AggregatePtr>(v);
            const Integer ev = enumValues.at(std::string(ag->tag));
            return makeIntValue(toT, static_cast<std::uint64_t>(ev));
        }
        if (Program::isFloatType(fromT)) {
            const double d = (fromT == Type::Float) ? std::get<double>(v) : static_cast<double>(std::get<float>(v));
            if (Program::isFloatType(toT)) return makeFloatValue(toT, d);
            const unsigned w = Program::intBitWidth(toT);
            if (!std::isfinite(d)) overflow(span);
            if (Program::isSignedInt(toT)) {
                if (w == 64) {
                    if (d >= 9223372036854775808.0 || d < -9223372036854775808.0) overflow(span);
                    return makeIntValue(toT, static_cast<std::uint64_t>(static_cast<Integer>(d)));
                }
                const double lo = static_cast<double>(minSignedForWidth(w)) - 1.0;
                const double hi = static_cast<double>(maxSignedForWidth(w)) + 1.0;
                if (d >= hi || d <= lo) overflow(span);
                return makeIntValue(toT, static_cast<std::uint64_t>(static_cast<Integer>(d)));
            }
            if (d < 0.0) overflow(span);
            const double hi = (w == 64) ? 18446744073709551616.0 : (static_cast<double>(maskForWidth(w)) + 1.0);
            if (d >= hi) overflow(span);
            return makeIntValue(toT, static_cast<std::uint64_t>(d));
        }
        const std::uint64_t rawBits = (fromT == Type::Int)
            ? static_cast<std::uint64_t>(std::get<Integer>(v))
            : std::get<ExactInt>(v).bits;
        const unsigned fromW = Program::intBitWidth(fromT);
        if (Program::isFloatType(toT)) {
            if (toT == Type::F32) {
                const float f = Program::isSignedInt(fromT)
                    ? static_cast<float>(signExtendWidth(rawBits, fromW))
                    : static_cast<float>(rawBits);
                return Value{f};
            }
            const double d = Program::isSignedInt(fromT)
                ? static_cast<double>(signExtendWidth(rawBits, fromW))
                : static_cast<double>(rawBits);
            return Value{d};
        }
        const unsigned toW = Program::intBitWidth(toT);
        if (toW <= fromW) return makeIntValue(toT, rawBits);
        if (Program::isSignedInt(fromT))
            return makeIntValue(toT, static_cast<std::uint64_t>(signExtendWidth(rawBits, fromW)));
        return makeIntValue(toT, rawBits);
    }

    void packValueToBytes(Type t, const Value& v, std::uint8_t* dst) const {
        switch (t) {
            case Type::I8:
            case Type::U8: {
                const std::uint8_t b = static_cast<std::uint8_t>(std::get<ExactInt>(v).bits & 0xFFULL);
                std::memcpy(dst, &b, 1);
                return;
            }
            case Type::I16:
            case Type::U16: {
                const std::uint16_t u = static_cast<std::uint16_t>(std::get<ExactInt>(v).bits & 0xFFFFULL);
                std::memcpy(dst, &u, 2);
                return;
            }
            case Type::I32:
            case Type::U32: {
                const std::uint32_t u = static_cast<std::uint32_t>(std::get<ExactInt>(v).bits & 0xFFFFFFFFULL);
                std::memcpy(dst, &u, 4);
                return;
            }
            case Type::Int: {
                const std::uint64_t u = static_cast<std::uint64_t>(std::get<Integer>(v));
                std::memcpy(dst, &u, 8);
                return;
            }
            case Type::U64: {
                const std::uint64_t u = std::get<ExactInt>(v).bits;
                std::memcpy(dst, &u, 8);
                return;
            }
            case Type::F32: {
                const float f = std::get<float>(v);
                std::memcpy(dst, &f, 4);
                return;
            }
            case Type::Float: {
                const double d = std::get<double>(v);
                std::memcpy(dst, &d, 8);
                return;
            }
            case Type::Bool: {
                const std::uint8_t b = std::get<bool>(v) ? 1u : 0u;
                std::memcpy(dst, &b, 1);
                return;
            }
            default: {
                if (programRef.isHandle(t)) {
                    const auto hv = std::get<HandleValue>(v);
                    std::memcpy(dst, &hv.idx, 4);
                    std::memcpy(dst + 4, &hv.gen, 4);
                    return;
                }
                if (programRef.isAnyPtr(t)) {
                    const auto& rp = std::get<RawPtrVal>(v);
                    const std::uintptr_t a = vmRawPtrAddr(rp);
                    if (a != 0 && (rp.vmSlot != nullptr || rp.vmArr != nullptr)) vmAddrMap[a] = rp;
                    const std::uint64_t u = static_cast<std::uint64_t>(a);
                    std::memcpy(dst, &u, 8);
                    return;
                }
                if (programRef.isSimd(t)) {
                    const auto& sv = std::get<SimdVal>(v);
                    const Type elemT = programRef.simdElem(t);
                    const std::size_t elemSz = vmElemSize(elemT);
                    for (unsigned k = 0; k < sv.lanes; ++k) {
                        packValueToBytes(elemT, decodeSimdLane(elemT, sv.lane(k)), dst + k * elemSz);
                    }
                    return;
                }
                const auto* decl = programRef.declaration(t);
                const auto& tl = layoutCalc.layoutOf(t);
                if (decl && decl->isEnum) {
                    const auto& ag = std::get<AggregatePtr>(v);
                    const std::size_t idx = enumVariantIndices.at(std::string(ag->tag));
                    if (tl.tagSize == 4) {
                        const std::uint32_t u = static_cast<std::uint32_t>(idx);
                        std::memcpy(dst, &u, 4);
                    } else {
                        const std::uint8_t b = static_cast<std::uint8_t>(idx);
                        std::memcpy(dst, &b, 1);
                    }
                    return;
                }
                const auto& vl = tl.variants.at(0);
                const auto& fields = decl->variants.at(0).fields;
                const auto& ag = std::get<AggregatePtr>(v);
                for (unsigned phys = 0; phys < static_cast<unsigned>(fields.size()); ++phys) {
                    const unsigned logIdx = vl.physOrder[phys];
                    packValueToBytes(fields[logIdx].type, ag->fields[logIdx], dst + vl.physOffsets[phys]);
                }
                return;
            }
        }
    }

    RawPtrVal vmPtrFromAddr(std::uintptr_t addr, Type pointee) const {
        if (addr == 0) return RawPtrVal{pointee, 0};
        if (auto it = vmAddrMap.find(addr); it != vmAddrMap.end()) {
            RawPtrVal r = it->second;
            r.pointee = pointee;
            return r;
        }
        for (const auto& [baseAddr, rp] : vmAddrMap) {
            if (rp.vmArr != nullptr && addr >= baseAddr) {
                const std::size_t elemSz = std::max<std::size_t>(1, layoutCalc.sizeAlignOf(rp.slotType).first);
                const std::size_t total = rp.vmArr->items.size() * elemSz;
                if (rp.byteOffset + (addr - baseAddr) < total) {
                    RawPtrVal r = rp;
                    r.pointee = pointee;
                    r.byteOffset += static_cast<std::size_t>(addr - baseAddr);
                    return r;
                }
            }
        }
        return RawPtrVal{pointee, addr};
    }

    std::size_t vmElemSize(Type elemT) const {
        if (elemT == Type::Void || elemT == Type::Unknown) return 1;
        return std::max<std::size_t>(1, layoutCalc.sizeAlignOf(elemT).first);
    }

    Value unpackValueFromBytes(Type t, const std::uint8_t* src) {
        switch (t) {
            case Type::I8:
            case Type::U8: {
                std::uint8_t b = 0;
                std::memcpy(&b, src, 1);
                return makeIntValue(t, b);
            }
            case Type::I16:
            case Type::U16: {
                std::uint16_t u = 0;
                std::memcpy(&u, src, 2);
                return makeIntValue(t, u);
            }
            case Type::I32:
            case Type::U32: {
                std::uint32_t u = 0;
                std::memcpy(&u, src, 4);
                return makeIntValue(t, u);
            }
            case Type::Int: {
                std::int64_t iv = 0;
                std::memcpy(&iv, src, 8);
                return Value{iv};
            }
            case Type::U64: {
                std::uint64_t u = 0;
                std::memcpy(&u, src, 8);
                return makeIntValue(Type::U64, u);
            }
            case Type::F32: {
                float f = 0.0f;
                std::memcpy(&f, src, 4);
                return Value{f};
            }
            case Type::Float: {
                double d = 0.0;
                std::memcpy(&d, src, 8);
                return Value{d};
            }
            case Type::Bool: {
                std::uint8_t b = 0;
                std::memcpy(&b, src, 1);
                return Value{b != 0};
            }
            default: {
                if (programRef.isHandle(t)) {
                    HandleValue hv{};
                    std::memcpy(&hv.idx, src, 4);
                    std::memcpy(&hv.gen, src + 4, 4);
                    return Value{hv};
                }
                if (programRef.isAnyPtr(t)) {
                    std::uint64_t u = 0;
                    std::memcpy(&u, src, 8);
                    return Value{vmPtrFromAddr(static_cast<std::uintptr_t>(u), programRef.ptrElem(t))};
                }
                if (programRef.isSimd(t)) {
                    SimdVal sv;
                    sv.elemType = programRef.simdElem(t);
                    sv.lanes = programRef.simdLanes(t);
                    const std::size_t elemSz = vmElemSize(sv.elemType);
                    for (unsigned k = 0; k < sv.lanes; ++k) {
                        sv.setLane(k, encodeSimdLane(sv.elemType, unpackValueFromBytes(sv.elemType, src + k * elemSz)));
                    }
                    return Value{sv};
                }
                const auto* decl = programRef.declaration(t);
                const auto& tl = layoutCalc.layoutOf(t);
                if (decl && decl->isEnum) {
                    std::size_t idx = 0;
                    if (tl.tagSize == 4) {
                        std::uint32_t u = 0;
                        std::memcpy(&u, src, 4);
                        idx = u;
                    } else {
                        std::uint8_t b = 0;
                        std::memcpy(&b, src, 1);
                        idx = b;
                    }
                    if (idx >= decl->variants.size())
                        fail("R_BOUNDS", "Valor de enum inválido al leer puntero", {}, "ptrRead(p)");
                    auto ag = std::make_shared<Aggregate>();
                    ag->tag = storeString(programRef.nameOf(t) + "." + decl->variants[idx].name);
                    return ag;
                }
                const auto& vl = tl.variants.at(0);
                const auto& fields = decl->variants.at(0).fields;
                auto ag = std::make_shared<Aggregate>();
                ag->tag = storeString(programRef.nameOf(t));
                ag->fields.resize(fields.size());
                for (unsigned phys = 0; phys < static_cast<unsigned>(fields.size()); ++phys) {
                    const unsigned logIdx = vl.physOrder[phys];
                    ag->fields[logIdx] = unpackValueFromBytes(fields[logIdx].type, src + vl.physOffsets[phys]);
                }
                return ag;
            }
        }
    }

    [[noreturn]] static void failNullPtr(Span span) {
        fail("R_NULL_PTR", "Desreferencia de puntero nulo", span, "if !ptrIsNull(p) { ... }");
    }

    void vmReadBytes(const RawPtrVal& rp, std::uint8_t* dst, std::size_t len, Span span) const {
        if (len == 0) return;
        if (rp.addr != 0) {
            std::memcpy(dst, reinterpret_cast<const void*>(rp.addr), len);
            return;
        }
        if (rp.vmSlot != nullptr) {
            const std::size_t slotSz = vmElemSize(rp.slotType);
            std::vector<std::uint8_t> tmp(slotSz, 0);
            packValueToBytes(rp.slotType, *static_cast<const Value*>(rp.vmSlot), tmp.data());
            if (rp.byteOffset + len > tmp.size())
                fail("R_BOUNDS", "Lectura fuera de los límites del objeto", span, "ptrRead(p)");
            std::memcpy(dst, tmp.data() + rp.byteOffset, len);
            return;
        }
        if (rp.vmArr != nullptr) {
            const std::size_t elemSz = vmElemSize(rp.slotType);
            const std::size_t total = rp.vmArr->items.size() * elemSz;
            if (rp.byteOffset + len > total)
                fail("R_BOUNDS", "Lectura fuera de los límites del arreglo", span, "ptrRead(slicePtr(a))");
            std::vector<std::uint8_t> elemBuf(elemSz, 0);
            std::size_t rem = len;
            std::size_t curOff = rp.byteOffset;
            std::size_t dstPos = 0;
            while (rem > 0) {
                const std::size_t idx = curOff / elemSz;
                const std::size_t inElem = curOff % elemSz;
                const std::size_t chunk = std::min(rem, elemSz - inElem);
                packValueToBytes(rp.slotType, rp.vmArr->items[idx], elemBuf.data());
                std::memcpy(dst + dstPos, elemBuf.data() + inElem, chunk);
                rem -= chunk;
                curOff += chunk;
                dstPos += chunk;
            }
            return;
        }
        failNullPtr(span);
    }

    void vmWriteBytes(const RawPtrVal& rp, const std::uint8_t* src, std::size_t len, Span span) {
        if (len == 0) return;
        if (rp.addr != 0) {
            std::memcpy(reinterpret_cast<void*>(rp.addr), src, len);
            return;
        }
        if (rp.vmSlot != nullptr) {
            const std::size_t slotSz = vmElemSize(rp.slotType);
            std::vector<std::uint8_t> tmp(slotSz, 0);
            Value* slotPtr = static_cast<Value*>(rp.vmSlot);
            packValueToBytes(rp.slotType, *slotPtr, tmp.data());
            if (rp.byteOffset + len > tmp.size())
                fail("R_BOUNDS", "Escritura fuera de los límites del objeto", span, "ptrWrite(p, v)");
            std::memcpy(tmp.data() + rp.byteOffset, src, len);
            *slotPtr = unpackValueFromBytes(rp.slotType, tmp.data());
            return;
        }
        if (rp.vmArr != nullptr) {
            const std::size_t elemSz = vmElemSize(rp.slotType);
            const std::size_t total = rp.vmArr->items.size() * elemSz;
            if (rp.byteOffset + len > total)
                fail("R_BOUNDS", "Escritura fuera de los límites del arreglo", span, "ptrWrite(mutSlicePtr(mut a), v)");
            std::vector<std::uint8_t> elemBuf(elemSz, 0);
            std::size_t rem = len;
            std::size_t curOff = rp.byteOffset;
            std::size_t srcPos = 0;
            while (rem > 0) {
                const std::size_t idx = curOff / elemSz;
                const std::size_t inElem = curOff % elemSz;
                const std::size_t chunk = std::min(rem, elemSz - inElem);
                if (inElem != 0 || chunk != elemSz)
                    packValueToBytes(rp.slotType, rp.vmArr->items[idx], elemBuf.data());
                std::memcpy(elemBuf.data() + inElem, src + srcPos, chunk);
                rp.vmArr->items[idx] = unpackValueFromBytes(rp.slotType, elemBuf.data());
                rem -= chunk;
                curOff += chunk;
                srcPos += chunk;
            }
            return;
        }
        failNullPtr(span);
    }

    RawPtrVal vmMakePlacePtr(Frame& frame, const Expr& placeExpr, Span span, bool isMutable) {
        const std::size_t idxCount = countPlaceIndices(placeExpr);
        const auto idxs = popPlaceIndices(frame, idxCount);
        auto [rootName, steps] = buildPlaceSteps(placeExpr, idxs);
        Value& root = frame.locals.find(std::string(rootName))->target();
        if (steps.empty()) {
            return RawPtrVal{placeExpr.type, 0, &root, nullptr, placeExpr.type, 0};
        }
        Value* cur = &root;
        for (std::size_t si = 0; si + 1 < steps.size(); ++si) {
            const auto& st = steps[si];
            if (st.isIndex) {
                auto& arr = std::get<ArrayPtr>(*cur);
                const auto pos = arrayIndex(*arr, st.indexVal, span);
                if (isMutable && arr.use_count() > 1) {
                    try { arr = std::make_shared<ArrayBuffer>(*arr); }
                    catch (const std::bad_alloc&) { allocationFailed(span); }
                }
                cur = &arr->items[pos];
            } else {
                auto& ag = std::get<AggregatePtr>(*cur);
                if (isMutable && ag.use_count() > 1) {
                    try { ag = std::make_shared<Aggregate>(*ag); }
                    catch (const std::bad_alloc&) { allocationFailed(span); }
                }
                cur = &ag->fields[st.fieldIdx];
            }
        }
        const auto& last = steps.back();
        if (last.isIndex) {
            auto& arr = std::get<ArrayPtr>(*cur);
            const auto pos = arrayIndex(*arr, last.indexVal, span);
            if (isMutable && arr.use_count() > 1) {
                try { arr = std::make_shared<ArrayBuffer>(*arr); }
                catch (const std::bad_alloc&) { allocationFailed(span); }
            }
            const std::size_t elemSz = vmElemSize(placeExpr.type);
            return RawPtrVal{placeExpr.type, 0, nullptr, arr, placeExpr.type, pos * elemSz};
        }
        auto& ag = std::get<AggregatePtr>(*cur);
        if (isMutable && ag.use_count() > 1) {
            try { ag = std::make_shared<Aggregate>(*ag); }
            catch (const std::bad_alloc&) { allocationFailed(span); }
        }
        return RawPtrVal{placeExpr.type, 0, &ag->fields[last.fieldIdx], nullptr, placeExpr.type, 0};
    }

    RawPtrVal vmAdvancePtr(RawPtrVal p, Integer offset, Span span) const {
        if (p.pointee == Type::Void || p.pointee == Type::Unknown)
            fail("E_PTR_TYPE", "ptrAdd no admite void", span, "ptrAdd(ptrCast<u8>(p), 1)");
        const std::size_t elemSz = vmElemSize(p.pointee);
        const std::intptr_t delta = static_cast<std::intptr_t>(offset) * static_cast<std::intptr_t>(elemSz);
        if (p.addr != 0) {
            p.addr = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(p.addr) + delta);
            return p;
        }
        p.byteOffset = static_cast<std::size_t>(static_cast<std::intptr_t>(p.byteOffset) + delta);
        return p;
    }

    static void sbEnsure(SbPtr& sb, std::size_t add, Span span) {
        if (add > static_cast<std::size_t>(maxStringLength) ||
            sb->data.size() > static_cast<std::size_t>(maxStringLength) - add)
            allocationFailed(span);
        if (sb.use_count() > 1) {
            try { sb = std::make_shared<StringBuilderBuf>(*sb); }
            catch (const std::bad_alloc&) { allocationFailed(span); }
        }
        const std::size_t need = sb->data.size() + add;
        if (need > sb->cap) {
            std::size_t nc = sb->cap == 0 ? 16 : sb->cap * 2;
            while (nc < need) {
                if (nc >= static_cast<std::size_t>(maxStringLength)) { nc = static_cast<std::size_t>(maxStringLength); break; }
                nc *= 2;
            }
            if (nc > static_cast<std::size_t>(maxStringLength)) nc = static_cast<std::size_t>(maxStringLength);
            sb->cap = nc;
        }
    }

    Value pop() {
        auto result = operands.back();
        operands.pop_back();
        return result;
    }

    static std::size_t countPlaceIndices(const Expr& e) {
        std::size_t count = 0;
        for (const Expr* cur = &e; cur->kind == ExprKind::Field || cur->kind == ExprKind::Index; cur = &cur->args.at(0)) {
            if (cur->kind == ExprKind::Index) ++count;
        }
        return count;
    }

    static std::vector<Integer> popPlaceIndices(Frame& frame, std::size_t count) {
        std::vector<Integer> res(count);
        for (std::size_t n = count; n != 0; --n) {
            res[n - 1] = frame.placeIdxStack.back();
            frame.placeIdxStack.pop_back();
        }
        return res;
    }

    static std::vector<Integer> peekPlaceIndices(const Frame& frame, std::size_t count) {
        std::vector<Integer> res(count);
        const std::size_t base = frame.placeIdxStack.size() - count;
        for (std::size_t n = 0; n < count; ++n) res[n] = frame.placeIdxStack[base + n];
        return res;
    }

    static std::pair<std::string_view, std::vector<PlaceStepVM>> buildPlaceSteps(
        const Expr& e, const std::vector<Integer>& indices) {
        std::vector<std::pair<bool, std::size_t>> rev;
        const Expr* cur = &e;
        while (cur->kind == ExprKind::Field || cur->kind == ExprKind::Index) {
            if (cur->kind == ExprKind::Index) rev.emplace_back(true, 0);
            else rev.emplace_back(false, cur->fieldIndices.at(0));
            cur = &cur->args.at(0);
        }
        std::vector<PlaceStepVM> steps;
        steps.reserve(rev.size());
        std::size_t idxPos = 0;
        for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
            if (it->first) steps.push_back({true, 0, indices.at(idxPos++)});
            else steps.push_back({false, it->second, 0});
        }
        return {std::string_view(cur->text), std::move(steps)};
    }

    static Value readPlace(const Value& root, const std::vector<PlaceStepVM>& steps, Span span) {
        Value cur = root;
        for (const auto& st : steps) {
            if (st.isIndex) {
                const auto& arr = std::get<ArrayPtr>(cur);
                const auto pos = arrayIndex(*arr, st.indexVal, span);
                cur = arr->items[pos];
            } else {
                cur = std::get<AggregatePtr>(cur)->fields.at(st.fieldIdx);
            }
        }
        return cur;
    }

    static void writeBackPlace(Value& rootVal, const std::vector<PlaceStepVM>& steps, Value leafVal, Span span) {
        if (steps.empty()) {
            dropValue(rootVal);
            rootVal = std::move(leafVal);
            return;
        }
        Value* cur = &rootVal;
        for (const auto& st : steps) {
            if (st.isIndex) {
                auto& arr = std::get<ArrayPtr>(*cur);
                const auto pos = arrayIndex(*arr, st.indexVal, span);
                if (arr.use_count() > 1) {
                    try { arr = std::make_shared<ArrayBuffer>(*arr); }
                    catch (const std::bad_alloc&) { allocationFailed(span); }
                }
                cur = &arr->items[pos];
            } else {
                auto& ag = std::get<AggregatePtr>(*cur);
                if (ag.use_count() > 1) {
                    try { ag = std::make_shared<Aggregate>(*ag); }
                    catch (const std::bad_alloc&) { allocationFailed(span); }
                }
                cur = &ag->fields[st.fieldIdx];
            }
        }
        dropValue(*cur);
        *cur = std::move(leafVal);
    }

    template<class Fn>
    static void mutateTarget(Frame& frame, const Expr& targetExpr, Fn&& mutator) {
        const std::size_t idxCount = countPlaceIndices(targetExpr);
        const auto idxs = popPlaceIndices(frame, idxCount);
        auto [rootName, steps] = buildPlaceSteps(targetExpr, idxs);
        Value& root = frame.locals.find(std::string(rootName))->target();
        if (steps.empty()) {
            mutator(root);
        } else {
            Value leaf = readPlace(root, steps, targetExpr.span);
            mutator(leaf);
            writeBackPlace(root, steps, std::move(leaf), targetExpr.span);
        }
    }

    Value makeResultOk(Type resT, Value val) {
        auto ag = std::make_shared<Aggregate>();
        ag->tag = storeString(programRef.nameOf(resT) + ".Ok");
        ag->fields.push_back(std::move(val));
        return ag;
    }

    Value makeResultErr(Type resT, std::string_view err) {
        auto ag = std::make_shared<Aggregate>();
        ag->tag = storeString(programRef.nameOf(resT) + ".Err");
        ag->fields.emplace_back(err);
        return ag;
    }

    Value makeOptionValue(Type optT, std::optional<Value> val) {
        auto ag = std::make_shared<Aggregate>();
        const std::string optName = programRef.nameOf(optT);
        if (val) {
            ag->tag = storeString(optName + ".Some");
            ag->fields.push_back(std::move(*val));
        } else {
            ag->tag = storeString(optName + ".None");
        }
        return ag;
    }

    static void dropValue(Value& v) {
        if (auto* f = std::get_if<FilePtr>(&v)) {
            if (*f) (*f)->close();
        } else if (auto* l = std::get_if<TcpListenerPtr>(&v)) {
            if (*l) (*l)->close();
        } else if (auto* s = std::get_if<SocketPtr>(&v)) {
            if (*s) (*s)->close();
        } else if (auto* lk = std::get_if<LockPtr>(&v)) {
            if (*lk) (*lk)->unlock();
        } else if (auto* tk = std::get_if<TaskPtr>(&v)) {
            if (*tk && tk->use_count() == 1 && !(*tk)->consumed && (*tk)->hasResult) {
                (*tk)->consumed = true;
                dropValue((*tk)->result);
            }
        } else if (auto* sc = std::get_if<ScopePtr>(&v)) {
            if (*sc && sc->use_count() == 1 && (*sc)->open) {
                (*sc)->open = false;
                while (!(*sc)->tasks.empty()) {
                    Value tv = std::move((*sc)->tasks.back());
                    (*sc)->tasks.pop_back();
                    dropValue(tv);
                }
            }
        } else if (auto* rg = std::get_if<RegionPtr>(&v)) {
            if (*rg) (*rg)->close();
        } else if (auto* rq = std::get_if<RingQueuePtr>(&v)) {
            if (*rq && rq->use_count() == 1) {
                while (!(*rq)->items.empty()) {
                    Value ev = std::move((*rq)->items.front());
                    (*rq)->items.pop_front();
                    dropValue(ev);
                }
            }
        } else if (auto* ag = std::get_if<AggregatePtr>(&v)) {
            if (*ag && ag->use_count() == 1) {
                for (std::size_t k = (*ag)->fields.size(); k > 0; --k)
                    dropValue((*ag)->fields[k - 1]);
            }
        } else if (auto* arr = std::get_if<ArrayPtr>(&v)) {
            if (*arr && arr->use_count() == 1) {
                for (std::size_t k = (*arr)->items.size(); k > 0; --k)
                    dropValue((*arr)->items[k - 1]);
            }
        } else if (auto* pool = std::get_if<PoolPtr>(&v)) {
            if (*pool && pool->use_count() == 1) {
                for (std::size_t k = (*pool)->used; k > 0; --k)
                    if ((*pool)->slots[k - 1].alive) dropValue((*pool)->slots[k - 1].val);
            }
        }
        v = Value{};
    }

    static Value cloneValue(const Value& v) {
        if (const auto* ag = std::get_if<AggregatePtr>(&v)) {
            if (!*ag) return v;
            auto copy = std::make_shared<Aggregate>();
            copy->tag = (*ag)->tag;
            copy->fields.reserve((*ag)->fields.size());
            for (const auto& f : (*ag)->fields) copy->fields.push_back(cloneValue(f));
            return copy;
        }
        if (const auto* arr = std::get_if<ArrayPtr>(&v)) {
            if (!*arr) return v;
            auto copy = std::make_shared<ArrayBuffer>();
            copy->cap = (*arr)->cap;
            copy->items.reserve((*arr)->items.size());
            for (const auto& item : (*arr)->items) copy->items.push_back(cloneValue(item));
            return copy;
        }
        if (const auto* sb = std::get_if<SbPtr>(&v)) {
            if (!*sb) return v;
            return std::make_shared<StringBuilderBuf>(**sb);
        }
        if (const auto* tbl = std::get_if<TablePtr>(&v)) {
            if (!*tbl) return v;
            auto copy = std::make_shared<SwissTableBuf>(**tbl);
            for (auto& k : copy->keys) k = cloneValue(k);
            for (auto& val : copy->vals) val = cloneValue(val);
            return copy;
        }
        if (const auto* pool = std::get_if<PoolPtr>(&v)) {
            if (!*pool) return v;
            auto copy = std::make_shared<PoolObject>(**pool);
            for (auto& sl : copy->slots) if (sl.alive) sl.val = cloneValue(sl.val);
            return copy;
        }
        return v;
    }

    static void pushScope(Frame& frame) {
        frame.locals.levels.emplace_back();
        frame.declOrder.emplace_back();
    }

    static void popScope(Frame& frame) {
        auto& lvl = frame.locals.levels.back();
        auto& order = frame.declOrder.back();
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            if (auto sit = lvl.find(*it); sit != lvl.end()) {
                if (sit->second.ref == nullptr && sit->second.owned)
                    dropValue(sit->second.value);
            }
        }
        frame.declOrder.pop_back();
        frame.locals.levels.pop_back();
    }

    static void unwindFrame(Frame& frame) {
        while (!frame.locals.levels.empty()) popScope(frame);
    }

    static void applyWriteBacks(Frame& frame) {
        for (auto& wb : frame.writeBacks) {
            if (wb.rootSlot) writeBackPlace(*wb.rootSlot, wb.placeSteps, *wb.tempBox, wb.span);
        }
    }

    void activate(const Function& fn, Span at, const Expr* callExpr = nullptr) {
        if (fn.isExternDecl)
            fail("R_FFI_VM", "Las llamadas extern \"C\" requieren compilación nativa con 'veloz build'.", at,
                 "veloz build main.turi -o app");
        if (frames.size() >= limits.depth)
            fail("R_DEPTH", "Límite de llamadas alcanzado", at, "fn main() {}");
        const auto base = operands.size() - fn.params.size();
        Frame* caller = frames.empty() ? nullptr : &frames.back();
        std::vector<std::string_view> mutRoots(fn.params.size());
        std::vector<std::vector<PlaceStepVM>> mutSteps(fn.params.size());
        if (caller && callExpr) {
            for (std::size_t i = fn.params.size(); i-- > 0;) {
                if (fn.params[i].byRef) {
                    const std::size_t cnt = countPlaceIndices(callExpr->args[i]);
                    const auto idxs = popPlaceIndices(*caller, cnt);
                    auto [rName, steps] = buildPlaceSteps(callExpr->args[i], idxs);
                    mutRoots[i] = rName;
                    mutSteps[i] = std::move(steps);
                }
            }
        }
        auto& frame = frames.emplace_back(Frame{&bodies.at(&fn), 0, base, {}, {{}}, {}, {}, &fn, {}, {}});
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (fn.params[i].byRef && caller && callExpr) {
                Value* rootSlot = &caller->locals.find(std::string(mutRoots[i]))->target();
                if (mutSteps[i].empty()) {
                    frame.locals.levels.back().emplace(fn.params[i].name, LocalSlot{Value{}, rootSlot, false});
                } else {
                    auto box = std::make_shared<Value>(operands[base + i]);
                    frame.locals.levels.back().emplace(fn.params[i].name, LocalSlot{Value{}, box.get(), false});
                    frame.writeBacks.push_back({rootSlot, std::move(mutSteps[i]), std::move(box), nullptr, at});
                }
            } else {
                frame.locals.levels.back().emplace(fn.params[i].name, LocalSlot{operands[base + i], nullptr, fn.params[i].owned});
            }
            frame.declOrder.back().push_back(fn.params[i].name);
        }
        operands.resize(base);
    }

    void activateDynMut(const Expr& callExpr, Span at, Frame& caller) {
        if (frames.size() >= limits.depth)
            fail("R_DEPTH", "Límite de llamadas alcanzado", at, "fn main() {}");
        const Type iface = callExpr.assocType;
        const std::size_t mi = callExpr.fieldIndices[0];
        const auto base = operands.size() - callExpr.args.size();
        const Type concreteType = std::get<DynPtr>(operands[base])->concreteType;
        operands[base] = Value{};
        const auto& fnName = dynDispatch.at({iface, concreteType}).at(mi);
        const Function& fn = *functions.at(fnName);
        std::vector<std::string_view> mutRoots(fn.params.size());
        std::vector<std::vector<PlaceStepVM>> mutSteps(fn.params.size());
        for (std::size_t i = fn.params.size(); i-- > 0;) {
            if (i == 0 || fn.params[i].byRef) {
                const std::size_t cnt = countPlaceIndices(callExpr.args[i]);
                const auto idxs = popPlaceIndices(caller, cnt);
                auto [rName, steps] = buildPlaceSteps(callExpr.args[i], idxs);
                mutRoots[i] = rName;
                mutSteps[i] = std::move(steps);
            }
        }
        Value* rootSlot = &caller.locals.find(std::string(mutRoots[0]))->target();
        DynPtr dp;
        std::shared_ptr<Value> writeBackBox;
        Value* writeBackRoot = nullptr;
        if (mutSteps[0].empty()) {
            auto& slotDp = std::get<DynPtr>(*rootSlot);
            if (slotDp.use_count() > 1) {
                try { slotDp = std::make_shared<DynObject>(*slotDp); }
                catch (const std::bad_alloc&) { allocationFailed(at); }
            }
            dp = slotDp;
        } else {
            Value curDynVal = readPlace(*rootSlot, mutSteps[0], at);
            try { dp = std::make_shared<DynObject>(*std::get<DynPtr>(curDynVal)); }
            catch (const std::bad_alloc&) { allocationFailed(at); }
            writeBackBox = std::make_shared<Value>(dp);
            writeBackRoot = rootSlot;
        }
        auto& frame = frames.emplace_back(Frame{&bodies.at(&fn), 0, base, {}, {{}}, {}, {}, &fn, {}, {}});
        frame.locals.levels.back().emplace(fn.params[0].name, LocalSlot{Value{}, &dp->inner, false});
        frame.declOrder.back().push_back(fn.params[0].name);
        frame.writeBacks.push_back({writeBackRoot, std::move(mutSteps[0]), std::move(writeBackBox), dp, at});
        for (std::size_t i = 1; i < fn.params.size(); ++i) {
            if (fn.params[i].byRef) {
                Value* rSlot = &caller.locals.find(std::string(mutRoots[i]))->target();
                if (mutSteps[i].empty()) {
                    frame.locals.levels.back().emplace(fn.params[i].name, LocalSlot{Value{}, rSlot, false});
                } else {
                    auto box = std::make_shared<Value>(operands[base + i]);
                    frame.locals.levels.back().emplace(fn.params[i].name, LocalSlot{Value{}, box.get(), false});
                    frame.writeBacks.push_back({rSlot, std::move(mutSteps[i]), std::move(box), nullptr, at});
                }
            } else {
                frame.locals.levels.back().emplace(fn.params[i].name, LocalSlot{operands[base + i], nullptr, fn.params[i].owned});
            }
            frame.declOrder.back().push_back(fn.params[i].name);
        }
        operands.resize(base);
    }

    void print(Value value) {
        if (auto text = std::get_if<std::string_view>(&value)) out << *text;
        else out << formatScalar(value);
        out << '\n';
    }

    static std::pair<std::string, int> runProcess(const std::string& cmd) {
        std::string outText;
        int code = -1;
#ifdef _WIN32
        FILE* pipe = _popen(cmd.c_str(), "rb");
#else
        FILE* pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) return {{}, -1};
        char buf[4096];
        while (std::size_t got = std::fread(buf, 1, sizeof(buf), pipe)) outText.append(buf, got);
#ifdef _WIN32
        code = _pclose(pipe);
#else
        int st = pclose(pipe);
        code = st;
        if (st >= 0 && (st & 0x7f) == 0) code = (st >> 8) & 0xff;
#endif
        return {std::move(outText), code};
    }

    void execute(const Function& entry) {
        struct FrameUnwindGuard {
            std::deque<Frame>& frames;
            std::vector<Value>& operands;
            ~FrameUnwindGuard() {
                while (!frames.empty()) {
                    unwindFrame(frames.back());
                    frames.pop_back();
                }
                operands.clear();
            }
        } unwindGuard{frames, operands};
        activate(entry, entry.span);
        auto steps = limits.steps;
        runUntilDepth(0, steps);
    }

    Value callClosureSync(const ClosurePtr& cp, std::vector<Value> args, Span span, std::uint64_t& steps) {
        const std::size_t stopDepth = frames.size();
        const auto& cl = programRef.closures.at(cp->id);
        if (cl.envStruct != Type::Void) operands.push_back(cp->env);
        for (auto& a : args) operands.push_back(std::move(a));
        activate(*functions.at(cl.fnName), span, nullptr);
        runUntilDepth(stopDepth, steps);
        return pop();
    }

    void runUntilDepth(std::size_t stopDepth, std::uint64_t& steps) {
        while (frames.size() > stopDepth) {
            Frame& frame = frames.back();
            const Instruction& i = (*frame.code)[frame.ip++];
            if (steps == 0) fail("R_STEPS", "Presupuesto de ejecución agotado", i.span, "for i in 0..3 { print(i) }");
            --steps;
            switch (i.op) {
                case Op::Literal:
                    if (i.expr->kind == ExprKind::Boolean) operands.emplace_back(i.expr->text == "true");
                    else if (i.expr->kind == ExprKind::Integer) operands.push_back(evalIntLiteralVM(programRef, *i.expr));
                    else if (i.expr->kind == ExprKind::Float) operands.push_back(evalFloatLiteralVM(*i.expr));
                    else operands.emplace_back(std::string_view(i.expr->text));
                    break;
                case Op::Read: {
                    Value& slotVal = frame.locals.find(i.expr->text)->target();
                    if (i.expr->isResourceMove) {
                        operands.push_back(std::move(slotVal));
                        slotVal = Value{};
                    } else {
                        operands.push_back(slotVal);
                    }
                    break;
                }
                case Op::ArrayLit: {
                    const std::size_t count = i.expr->args.size();
                    if (count > static_cast<std::size_t>(maxArrayLength)) allocationFailed(i.span);
                    try {
                        auto buf = std::make_shared<ArrayBuffer>();
                        buf->items.resize(count);
                        for (std::size_t n = count; n != 0; --n) buf->items[n - 1] = pop();
                        buf->cap = count;
                        operands.emplace_back(std::move(buf));
                    } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    break;
                }
                case Op::Interp: {
                    const std::size_t count = i.expr->args.size();
                    std::vector<Value> parts(count);
                    for (std::size_t n = count; n != 0; --n) parts[n - 1] = pop();
                    std::string res;
                    for (const auto& part : parts) {
                        const std::string s = formatScalar(part);
                        if (s.size() > static_cast<std::size_t>(maxStringLength) - res.size())
                            allocationFailed(i.span);
                        res += s;
                    }
                    operands.emplace_back(storeString(std::move(res)));
                    break;
                }
                case Op::Slice: {
                    const Integer hi = std::get<Integer>(pop());
                    const Integer lo = std::get<Integer>(pop());
                    auto base = pop();
                    if (auto* arrPtr = std::get_if<ArrayPtr>(&base)) {
                        const auto& src = (*arrPtr)->items;
                        if (lo < 0 || hi < lo || static_cast<std::uint64_t>(hi) > src.size())
                            fail("R_BOUNDS", "Corte fuera de rango", i.span, "a[0..len(a)]");
                        const auto count = static_cast<std::size_t>(hi - lo);
                        try {
                            auto buf = std::make_shared<ArrayBuffer>();
                            buf->items.assign(src.begin() + static_cast<std::ptrdiff_t>(lo),
                                              src.begin() + static_cast<std::ptrdiff_t>(hi));
                            buf->cap = count;
                            operands.emplace_back(std::move(buf));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else {
                        const auto sv = std::get<std::string_view>(base);
                        if (lo < 0 || hi < lo || static_cast<std::uint64_t>(hi) > sv.size())
                            fail("R_BOUNDS", "Corte fuera de rango", i.span, "s[0..len(s)]");
                        operands.emplace_back(sv.substr(static_cast<std::size_t>(lo), static_cast<std::size_t>(hi - lo)));
                    }
                    break;
                }
                case Op::Call: {
                    const auto& fnName = i.expr->text;
                    if (auto ufn = functions.find(fnName); ufn != functions.end()) {
                        activate(*ufn->second, i.span, i.expr);
                    } else if (fnName == "readByte") {
                        auto byte = input ? input->get() : std::char_traits<char>::eof();
                        operands.emplace_back(static_cast<Integer>(byte == std::char_traits<char>::eof() ? -1 : byte));
                    } else if (fnName == "len") {
                        auto value = pop();
                        std::size_t length = 0;
                        if (const auto* array = std::get_if<ArrayPtr>(&value)) length = (*array)->items.size();
                        else if (const auto* sb = std::get_if<SbPtr>(&value)) length = (*sb)->data.size();
                        else if (const auto* tbl = std::get_if<TablePtr>(&value)) length = (*tbl)->len;
                        else if (const auto* pool = std::get_if<PoolPtr>(&value)) length = (*pool)->len;
                        else length = std::get<std::string_view>(value).size();
                        if (length > static_cast<std::uint64_t>(maxInt)) overflow(i.span);
                        operands.emplace_back(static_cast<Integer>(length));
                    } else if (fnName == "cap") {
                        auto value = pop();
                        if (const auto* sb = std::get_if<SbPtr>(&value))
                            operands.emplace_back(static_cast<Integer>((*sb)->cap));
                        else if (const auto* tbl = std::get_if<TablePtr>(&value))
                            operands.emplace_back(static_cast<Integer>(tableMaxLoad((*tbl)->buckets)));
                        else if (const auto* pool = std::get_if<PoolPtr>(&value))
                            operands.emplace_back(static_cast<Integer>((*pool)->cap));
                        else
                            operands.emplace_back(static_cast<Integer>(std::get<ArrayPtr>(value)->cap));
                    } else if (fnName == "push") {
                        auto elem = pop();
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& arr = std::get<ArrayPtr>(target);
                            if (arr->items.size() >= static_cast<std::size_t>(maxArrayLength)) allocationFailed(i.span);
                            try {
                                if (arr.use_count() > 1) arr = std::make_shared<ArrayBuffer>(*arr);
                                if (arr->items.size() >= arr->cap) {
                                    std::size_t nc = arr->cap == 0 ? 8 : arr->cap * 2;
                                    if (nc > static_cast<std::size_t>(maxArrayLength)) nc = static_cast<std::size_t>(maxArrayLength);
                                    arr->cap = nc;
                                }
                                arr->items.push_back(std::move(elem));
                            } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "pop") {
                        Value back;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& arr = std::get<ArrayPtr>(target);
                            if (arr->items.empty())
                                fail("R_BOUNDS", "No se puede hacer pop de un array vacío", i.span, "pop(mut a)");
                            if (arr.use_count() > 1) {
                                try { arr = std::make_shared<ArrayBuffer>(*arr); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            back = std::move(arr->items.back());
                            arr->items.pop_back();
                        });
                        operands.push_back(std::move(back));
                    } else if (fnName == "clear") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* tbl = std::get_if<TablePtr>(&target)) {
                                if ((*tbl)->buckets > 0u) {
                                    if (tbl->use_count() > 1) {
                                        try { *tbl = std::make_shared<SwissTableBuf>(**tbl); }
                                        catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                    }
                                    SwissTableBuf& t = **tbl;
                                    std::fill(t.ctrl.begin(), t.ctrl.end(), static_cast<std::uint8_t>(0xFFu));
                                    std::fill(t.keys.begin(), t.keys.end(), Value{});
                                    std::fill(t.vals.begin(), t.vals.end(), Value{});
                                    t.len = 0u;
                                    t.growthLeft = tableMaxLoad(t.buckets);
                                }
                            } else if (auto* poolPtr = std::get_if<PoolPtr>(&target)) {
                                if ((*poolPtr)->len > 0u) {
                                    if (poolPtr->use_count() > 1) {
                                        try { *poolPtr = std::make_shared<PoolObject>(**poolPtr); }
                                        catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                    }
                                    PoolObject& p = **poolPtr;
                                    for (std::size_t idx = p.used; idx > 0u; --idx) {
                                        PoolSlot& slot = p.slots[idx - 1u];
                                        if (!slot.alive) continue;
                                        slot.val = Value{};
                                        slot.alive = false;
                                        ++slot.gen;
                                        if (slot.gen != 0u) p.freeList.push_back(static_cast<std::uint32_t>(idx - 1u));
                                    }
                                    p.len = 0u;
                                }
                            } else {
                                auto& arr = std::get<ArrayPtr>(target);
                                if (arr.use_count() > 1) {
                                    try { arr = std::make_shared<ArrayBuffer>(*arr); }
                                    catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                }
                                for (std::size_t k = arr->items.size(); k > 0; --k) dropValue(arr->items[k - 1]);
                                arr->items.clear();
                            }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "truncate") {
                        const Integer keep = std::get<Integer>(pop());
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& arr = std::get<ArrayPtr>(target);
                            const auto n = static_cast<std::size_t>(
                                std::clamp(keep, Integer{0}, static_cast<Integer>(arr->items.size())));
                            if (n < arr->items.size()) {
                                try { if (arr.use_count() > 1) arr = std::make_shared<ArrayBuffer>(*arr); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                for (std::size_t k = arr->items.size(); k > n; --k) dropValue(arr->items[k - 1]);
                                arr->items.resize(n);
                            }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "reserve") {
                        const Integer minCap = std::get<Integer>(pop());
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* tbl = std::get_if<TablePtr>(&target)) {
                                if (minCap > 0 && (*tbl).use_count() > 1) {
                                    try { *tbl = std::make_shared<SwissTableBuf>(**tbl); }
                                    catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                }
                                tableReserve(**tbl, minCap, i.span);
                            } else if (auto* poolPtr = std::get_if<PoolPtr>(&target)) {
                                if (minCap > 0 && static_cast<std::size_t>(minCap) > (*poolPtr)->cap && poolPtr->use_count() > 1) {
                                    try { *poolPtr = std::make_shared<PoolObject>(**poolPtr); }
                                    catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                }
                                poolReserve(**poolPtr, minCap, i.span);
                            } else {
                                if (minCap < 0) fail("R_ARRAY_SIZE", "La longitud debe ser no negativa", i.span, "reserve(mut a, 8)");
                                if (minCap > maxArrayLength) allocationFailed(i.span);
                                auto& arr = std::get<ArrayPtr>(target);
                                if (static_cast<std::size_t>(minCap) > arr->cap) {
                                    try {
                                        if (arr.use_count() > 1) arr = std::make_shared<ArrayBuffer>(*arr);
                                        std::size_t nc = arr->cap == 0 ? 8 : arr->cap * 2;
                                        while (nc < static_cast<std::size_t>(minCap)) nc *= 2;
                                        if (nc > static_cast<std::size_t>(maxArrayLength)) nc = static_cast<std::size_t>(maxArrayLength);
                                        arr->cap = nc;
                                    } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                                }
                            }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "repeat") {
                        auto count = std::get<Integer>(pop());
                        auto element = pop();
                        operands.emplace_back(repeatArray(std::move(element), count, i.span));
                    } else if (fnName == "sizeOf" || fnName == "alignOf") {
                        const auto [sz, al] = layoutCalc.sizeAlignOf(i.expr->typeArgs.at(0));
                        operands.emplace_back(static_cast<Integer>(fnName == "sizeOf" ? sz : al));
                    } else if (fnName == "bitcast") {
                        const Value srcVal = pop();
                        const Type fromT = i.expr->args.at(0).type;
                        const Type toT = i.expr->type;
                        const std::size_t sz = layoutCalc.sizeAlignOf(fromT).first;
                        std::vector<std::uint8_t> buf(std::max<std::size_t>(1, sz), 0);
                        packValueToBytes(fromT, srcVal, buf.data());
                        operands.push_back(unpackValueFromBytes(toT, buf.data()));
                    } else if (auto targetNum = numericBuiltinType(fnName)) {
                        operands.push_back(castNumericVM(*targetNum, i.expr->args.at(0).type, pop(), i.span));
                    } else if (fnName == "writeByte") {
                        auto b = std::get<Integer>(pop());
                        out.put(static_cast<char>(static_cast<unsigned char>(b & 0xff)));
                        operands.emplace_back(Value{});
                    } else if (fnName == "write") {
                        auto s = std::get<std::string_view>(pop());
                        out.write(s.data(), static_cast<std::streamsize>(s.size()));
                        operands.emplace_back(Value{});
                    } else if (fnName == "ewrite") {
                        auto s = std::get<std::string_view>(pop());
                        std::fwrite(s.data(), 1, s.size(), stderr);
                        operands.emplace_back(Value{});
                    } else if (fnName == "eprint") {
                        auto s = std::get<std::string_view>(pop());
                        std::fwrite(s.data(), 1, s.size(), stderr);
                        std::fputc('\n', stderr);
                        operands.emplace_back(Value{});
                    } else if (fnName == "flush") {
                        out.flush();
                        operands.emplace_back(Value{});
                    } else if (fnName == "readFile") {
                        auto path = std::string(std::get<std::string_view>(pop()));
                        std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
                        std::string content;
                        if (f) {
                            f.seekg(0, std::ios::end);
                            const auto sz = f.tellg();
                            if (sz > 0) { content.resize(static_cast<std::size_t>(sz)); f.seekg(0); f.read(content.data(), sz); }
                        }
                        operands.emplace_back(storeString(std::move(content)));
                    } else if (fnName == "writeFile" || fnName == "appendFile") {
                        auto content = std::get<std::string_view>(pop());
                        auto path = std::string(std::get<std::string_view>(pop()));
                        auto mode = std::ios::binary | (fnName == "appendFile" ? std::ios::app : std::ios::trunc);
                        std::ofstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), mode);
                        bool ok = static_cast<bool>(f);
                        if (ok && !content.empty()) { f.write(content.data(), static_cast<std::streamsize>(content.size())); ok = static_cast<bool>(f); }
                        operands.emplace_back(ok);
                    } else if (fnName == "tryReadFile") {
                        auto path = std::string(std::get<std::string_view>(pop()));
                        std::ifstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
                        if (!f) {
                            operands.push_back(makeResultErr(i.expr->type, "No se pudo abrir el archivo"));
                        } else {
                            std::string content;
                            f.seekg(0, std::ios::end);
                            const auto sz = f.tellg();
                            if (sz > 0) {
                                content.resize(static_cast<std::size_t>(sz));
                                f.seekg(0);
                                f.read(content.data(), sz);
                            }
                            if (f.bad()) {
                                operands.push_back(makeResultErr(i.expr->type, "Error de lectura en archivo"));
                            } else {
                                operands.push_back(makeResultOk(i.expr->type, storeString(std::move(content))));
                            }
                        }
                    } else if (fnName == "tryWriteFile") {
                        auto content = std::get<std::string_view>(pop());
                        auto path = std::string(std::get<std::string_view>(pop()));
                        std::ofstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary | std::ios::trunc);
                        bool ok = static_cast<bool>(f);
                        if (ok && !content.empty()) {
                            f.write(content.data(), static_cast<std::streamsize>(content.size()));
                            ok = static_cast<bool>(f);
                        }
                        if (ok) f.flush();
                        ok = ok && static_cast<bool>(f);
                        if (!ok) operands.push_back(makeResultErr(i.expr->type, "No se pudo escribir el archivo"));
                        else operands.push_back(makeResultOk(i.expr->type, true));
                    } else if (fnName == "fileExists") {
                        auto path = std::string(std::get<std::string_view>(pop()));
                        std::error_code ec;
                        operands.emplace_back(std::filesystem::exists(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), ec));
                    } else if (fnName == "removeFile") {
                        auto path = std::string(std::get<std::string_view>(pop()));
                        std::error_code ec;
                        operands.emplace_back(std::filesystem::remove(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), ec));
                    } else if (fnName == "makeDir") {
                        auto path = std::string(std::get<std::string_view>(pop()));
                        std::error_code ec;
                        auto u8p = std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str()));
                        bool ok = std::filesystem::create_directories(u8p, ec) || std::filesystem::is_directory(u8p, ec);
                        operands.emplace_back(ok);
                    } else if (fnName == "argsCount") {
                        operands.emplace_back(static_cast<Integer>(programArgs.size()));
                    } else if (fnName == "argAt") {
                        auto idx = std::get<Integer>(pop());
                        if (idx >= 0 && static_cast<std::size_t>(idx) < programArgs.size())
                            operands.emplace_back(std::string_view(programArgs[static_cast<std::size_t>(idx)]));
                        else
                            operands.emplace_back(std::string_view(""));
                    } else if (fnName == "envGet") {
                        auto name = std::string(std::get<std::string_view>(pop()));
#ifdef _WIN32
                        char* raw = nullptr;
                        std::size_t rawLen = 0;
                        std::string envVal;
                        if (_dupenv_s(&raw, &rawLen, name.c_str()) == 0 && raw) {
                            envVal = raw;
                            std::free(raw);
                            operands.emplace_back(storeString(std::move(envVal)));
                        } else {
                            operands.emplace_back(std::string_view(""));
                        }
#else
                        const char* val = std::getenv(name.c_str());
                        operands.emplace_back(val ? storeString(std::string(val)) : std::string_view(""));
#endif
                    } else if (fnName == "clockNs") {
                        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        operands.emplace_back(static_cast<Integer>(ns));
                    } else if (fnName == "sleepMs") {
                        auto ms = std::get<Integer>(pop());
                        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                        operands.emplace_back(Value{});
                    } else if (fnName == "execCapture") {
                        auto cmd = std::string(std::get<std::string_view>(pop()));
                        operands.emplace_back(storeString(runProcess(cmd).first));
                    } else if (fnName == "execStatus") {
                        auto cmd = std::string(std::get<std::string_view>(pop()));
                        operands.emplace_back(static_cast<Integer>(runProcess(cmd).second));
                    } else if (fnName == "wrappingAdd" || fnName == "wrappingSub" || fnName == "wrappingMul") {
                        const auto rv = pop();
                        const auto lv = pop();
                        if (auto* lei = std::get_if<ExactInt>(&lv)) {
                            const auto rei = std::get<ExactInt>(rv);
                            const auto r = fnName == "wrappingAdd" ? (lei->bits + rei.bits)
                                         : fnName == "wrappingSub" ? (lei->bits - rei.bits)
                                                                   : (lei->bits * rei.bits);
                            operands.push_back(makeIntValue(lei->type, r));
                        } else {
                            const auto b = static_cast<std::uint64_t>(std::get<Integer>(rv));
                            const auto a = static_cast<std::uint64_t>(std::get<Integer>(lv));
                            const auto r = fnName == "wrappingAdd" ? (a + b) : fnName == "wrappingSub" ? (a - b) : (a * b);
                            operands.emplace_back(static_cast<Integer>(r));
                        }
                    } else if (fnName == "saturatingAdd" || fnName == "saturatingSub" || fnName == "saturatingMul") {
                        const auto rv = pop();
                        const auto lv = pop();
                        if (auto* lei = std::get_if<ExactInt>(&lv)) {
                            const auto rei = std::get<ExactInt>(rv);
                            const Type ty = lei->type;
                            const unsigned w = Program::intBitWidth(ty);
                            if (Program::isSignedInt(ty)) {
                                const Integer sa = signExtendWidth(lei->bits, w);
                                const Integer sb = signExtendWidth(rei.bits, w);
                                const Integer smin = minSignedForWidth(w);
                                const Integer smax = maxSignedForWidth(w);
                                Integer res = 0;
                                if (fnName == "saturatingAdd") {
                                    auto ov = checkedAddWidth(sa, sb, w);
                                    res = ov ? *ov : (sa >= 0 ? smax : smin);
                                } else if (fnName == "saturatingSub") {
                                    auto ov = checkedSubWidth(sa, sb, w);
                                    res = ov ? *ov : (sa >= 0 ? smax : smin);
                                } else {
                                    auto ov = checkedMulWidth(sa, sb, w);
                                    res = ov ? *ov : (((sa < 0) != (sb < 0)) ? smin : smax);
                                }
                                operands.push_back(makeIntValue(ty, static_cast<std::uint64_t>(res)));
                            } else {
                                const std::uint64_t mask = maskForWidth(w);
                                std::uint64_t res = 0;
                                if (fnName == "saturatingAdd") {
                                    auto ov = checkedUAddWidth(lei->bits, rei.bits, w);
                                    res = ov ? *ov : mask;
                                } else if (fnName == "saturatingSub") {
                                    auto ov = checkedUSubWidth(lei->bits, rei.bits);
                                    res = ov ? *ov : 0ULL;
                                } else {
                                    auto ov = checkedUMulWidth(lei->bits, rei.bits, w);
                                    res = ov ? *ov : mask;
                                }
                                operands.push_back(makeIntValue(ty, res));
                            }
                        } else {
                            const auto b = std::get<Integer>(rv);
                            const auto a = std::get<Integer>(lv);
                            if (fnName == "saturatingAdd") {
                                auto r = checkedAdd(a, b);
                                operands.emplace_back(r ? *r : (a >= 0 ? maxInt : minInt));
                            } else if (fnName == "saturatingSub") {
                                auto r = checkedSub(a, b);
                                operands.emplace_back(r ? *r : (a >= 0 ? maxInt : minInt));
                            } else {
                                auto r = checkedMul(a, b);
                                operands.emplace_back(r ? *r : (((a < 0) != (b < 0)) ? minInt : maxInt));
                            }
                        }
                    } else if (fnName == "abs") {
                        auto v = pop();
                        if (auto* d = std::get_if<double>(&v)) operands.emplace_back(std::fabs(*d));
                        else if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::fabs(*f));
                        else if (auto* ei = std::get_if<ExactInt>(&v)) {
                            const unsigned w = Program::intBitWidth(ei->type);
                            const Integer sa = signExtendWidth(ei->bits, w);
                            if (sa == minSignedForWidth(w)) overflow(i.span);
                            operands.push_back(makeIntValue(ei->type, static_cast<std::uint64_t>(sa < 0 ? -sa : sa)));
                        } else {
                            const auto n = std::get<Integer>(v);
                            if (n == minInt) overflow(i.span);
                            operands.emplace_back(n < 0 ? -n : n);
                        }
                    } else if (fnName == "min" || fnName == "max") {
                        auto r = pop();
                        auto l = pop();
                        if (auto* ld = std::get_if<double>(&l)) {
                            const double rd = std::get<double>(r);
                            operands.emplace_back(fnName == "min" ? std::fmin(*ld, rd) : std::fmax(*ld, rd));
                        } else if (auto* lf = std::get_if<float>(&l)) {
                            const float rf = std::get<float>(r);
                            operands.emplace_back(fnName == "min" ? std::fmin(*lf, rf) : std::fmax(*lf, rf));
                        } else if (auto* lei = std::get_if<ExactInt>(&l)) {
                            const auto rei = std::get<ExactInt>(r);
                            const unsigned w = Program::intBitWidth(lei->type);
                            if (Program::isSignedInt(lei->type)) {
                                const Integer sa = signExtendWidth(lei->bits, w), sb = signExtendWidth(rei.bits, w);
                                operands.push_back(makeIntValue(lei->type, static_cast<std::uint64_t>(fnName == "min" ? std::min(sa, sb) : std::max(sa, sb))));
                            } else {
                                operands.push_back(makeIntValue(lei->type, fnName == "min" ? std::min(lei->bits, rei.bits) : std::max(lei->bits, rei.bits)));
                            }
                        } else {
                            const auto li = std::get<Integer>(l), ri = std::get<Integer>(r);
                            operands.emplace_back(fnName == "min" ? std::min(li, ri) : std::max(li, ri));
                        }
                    } else if (fnName == "sqrt") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::sqrt(*f));
                        else operands.emplace_back(std::sqrt(std::get<double>(v)));
                    } else if (fnName == "floor") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::floor(*f));
                        else operands.emplace_back(std::floor(std::get<double>(v)));
                    } else if (fnName == "ceil") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::ceil(*f));
                        else operands.emplace_back(std::ceil(std::get<double>(v)));
                    } else if (fnName == "round") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::round(*f));
                        else operands.emplace_back(std::round(std::get<double>(v)));
                    } else if (fnName == "sin") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::sin(*f));
                        else operands.emplace_back(std::sin(std::get<double>(v)));
                    } else if (fnName == "cos") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::cos(*f));
                        else operands.emplace_back(std::cos(std::get<double>(v)));
                    } else if (fnName == "pow") {
                        auto b = pop();
                        auto a = pop();
                        if (auto* af = std::get_if<float>(&a)) operands.emplace_back(std::pow(*af, std::get<float>(b)));
                        else operands.emplace_back(std::pow(std::get<double>(a), std::get<double>(b)));
                    } else if (fnName == "isNaN") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::isnan(*f));
                        else operands.emplace_back(std::isnan(std::get<double>(v)));
                    } else if (fnName == "isInf") {
                        auto v = pop();
                        if (auto* f = std::get_if<float>(&v)) operands.emplace_back(std::isinf(*f));
                        else operands.emplace_back(std::isinf(std::get<double>(v)));
                    } else if (fnName == "str") {
                        auto v = pop();
                        if (auto* sv = std::get_if<std::string_view>(&v)) operands.emplace_back(*sv);
                        else operands.emplace_back(storeString(formatScalar(v)));
                    } else if (fnName == "strFromByte") {
                        const auto b = std::get<Integer>(pop());
                        operands.emplace_back(storeString(std::string(1, static_cast<char>(static_cast<unsigned char>(b & 0xff)))));
                    } else if (fnName == "parseInt") {
                        const auto sv = std::get<std::string_view>(pop());
                        Integer parsed{};
                        if (!parseIntExact(sv, parsed))
                            fail("R_PARSE", "Entero inválido al convertir texto", i.span, R"(parseInt("42"))");
                        operands.emplace_back(parsed);
                    } else if (fnName == "parseFloat") {
                        const auto sv = std::get<std::string_view>(pop());
                        double parsed{};
                        if (!parseFloatExact(sv, parsed))
                            fail("R_PARSE", "Flotante inválido al convertir texto", i.span, R"(parseFloat("3.5"))");
                        operands.emplace_back(parsed);
                    } else if (fnName == "tryParseInt") {
                        const auto fb = std::get<Integer>(pop());
                        const auto sv = std::get<std::string_view>(pop());
                        Integer parsed{};
                        operands.emplace_back(parseIntExact(sv, parsed) ? parsed : fb);
                    } else if (fnName == "tryParseFloat") {
                        const auto fb = std::get<double>(pop());
                        const auto sv = std::get<std::string_view>(pop());
                        double parsed{};
                        operands.emplace_back(parseFloatExact(sv, parsed) ? parsed : fb);
                    } else if (fnName == "strContains") {
                        const auto sub = std::get<std::string_view>(pop());
                        const auto s = std::get<std::string_view>(pop());
                        operands.emplace_back(s.find(sub) != std::string_view::npos);
                    } else if (fnName == "strStartsWith") {
                        const auto pfx = std::get<std::string_view>(pop());
                        const auto s = std::get<std::string_view>(pop());
                        operands.emplace_back(s.starts_with(pfx));
                    } else if (fnName == "strEndsWith") {
                        const auto sfx = std::get<std::string_view>(pop());
                        const auto s = std::get<std::string_view>(pop());
                        operands.emplace_back(s.ends_with(sfx));
                    } else if (fnName == "strIndexOf") {
                        const auto sub = std::get<std::string_view>(pop());
                        const auto s = std::get<std::string_view>(pop());
                        const auto p = s.find(sub);
                        operands.emplace_back(p == std::string_view::npos ? Integer{-1} : static_cast<Integer>(p));
                    } else if (fnName == "sbNew") {
                        try { operands.emplace_back(std::make_shared<StringBuilderBuf>()); }
                        catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "sbWithCap") {
                        const Integer c = std::get<Integer>(pop());
                        if (c < 0) fail("R_ARRAY_SIZE", "La longitud debe ser no negativa", i.span, "sbWithCap(16)");
                        if (c > maxStringLength) allocationFailed(i.span);
                        try {
                            auto sb = std::make_shared<StringBuilderBuf>();
                            sb->cap = static_cast<std::size_t>(c);
                            sb->data.reserve(sb->cap);
                            operands.emplace_back(std::move(sb));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "sbPush") {
                        const auto s = std::get<std::string_view>(pop());
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sb = std::get<SbPtr>(target);
                            sbEnsure(sb, s.size(), i.span);
                            try { sb->data.append(s); }
                            catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "sbPushByte") {
                        const auto b = std::get<Integer>(pop());
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sb = std::get<SbPtr>(target);
                            sbEnsure(sb, 1, i.span);
                            try { sb->data.push_back(static_cast<char>(static_cast<unsigned char>(b & 0xff))); }
                            catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "sbPushInt") {
                        const std::string s = std::to_string(std::get<Integer>(pop()));
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sb = std::get<SbPtr>(target);
                            sbEnsure(sb, s.size(), i.span);
                            try { sb->data.append(s); }
                            catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "sbPushFloat") {
                        const std::string s = formatFloat(std::get<double>(pop()));
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sb = std::get<SbPtr>(target);
                            sbEnsure(sb, s.size(), i.span);
                            try { sb->data.append(s); }
                            catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "sbClear") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sb = std::get<SbPtr>(target);
                            if (sb.use_count() > 1) {
                                try { sb = std::make_shared<StringBuilderBuf>(*sb); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            sb->data.clear();
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "sbStr") {
                        const auto sb = std::get<SbPtr>(pop());
                        operands.emplace_back(storeString(sb->data));
                    } else if (fnName == "sbTake") {
                        std::string taken;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sb = std::get<SbPtr>(target);
                            if (sb.use_count() > 1) {
                                try { sb = std::make_shared<StringBuilderBuf>(*sb); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            taken = std::move(sb->data);
                            sb->data.clear();
                            sb->cap = 0;
                        });
                        operands.emplace_back(storeString(std::move(taken)));
                    } else if (fnName == "isSome" || fnName == "isNone") {
                        const auto ag = std::get<AggregatePtr>(pop());
                        const auto dot = ag->tag.rfind('.');
                        const auto var = dot == std::string_view::npos ? ag->tag : ag->tag.substr(dot + 1);
                        operands.emplace_back(fnName == "isSome" ? (var == "Some") : (var == "None"));
                    } else if (fnName == "isOk" || fnName == "isErr") {
                        const auto ag = std::get<AggregatePtr>(pop());
                        const auto dot = ag->tag.rfind('.');
                        const auto var = dot == std::string_view::npos ? ag->tag : ag->tag.substr(dot + 1);
                        operands.emplace_back(fnName == "isOk" ? (var == "Ok") : (var == "Err"));
                    } else if (fnName == "unwrapOr") {
                        auto fb = pop();
                        const auto ag = std::get<AggregatePtr>(pop());
                        const auto dot = ag->tag.rfind('.');
                        const auto var = dot == std::string_view::npos ? ag->tag : ag->tag.substr(dot + 1);
                        operands.push_back((var == "Some" || var == "Ok") ? ag->fields.at(0) : std::move(fb));
                    } else if (fnName == "mapNew" || fnName == "setNew" || fnName == "mapSecure" || fnName == "setSecure") {
                        try {
                            auto tbl = std::make_shared<SwissTableBuf>();
                            tbl->keyKind = tableKeyKind(i.expr->type);
                            tbl->isMap = programRef.isMap(i.expr->type);
                            if (fnName == "mapSecure" || fnName == "setSecure") {
                                tbl->secure = true;
                                tbl->seed = randomTableSeed();
                            }
                            operands.emplace_back(std::move(tbl));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "mapWithCap" || fnName == "setWithCap") {
                        const Integer c = std::get<Integer>(pop());
                        try {
                            auto tbl = std::make_shared<SwissTableBuf>();
                            tbl->keyKind = tableKeyKind(i.expr->type);
                            tbl->isMap = programRef.isMap(i.expr->type);
                            tableReserve(*tbl, c, i.span);
                            operands.emplace_back(std::move(tbl));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "mapSet") {
                        auto val = pop();
                        auto key = pop();
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& tbl = std::get<TablePtr>(target);
                            tableInsert(tbl, key, std::move(val), i.span);
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "mapGet") {
                        auto key = pop();
                        const auto tbl = std::get<TablePtr>(pop());
                        auto ag = std::make_shared<Aggregate>();
                        const std::string optName = programRef.nameOf(i.expr->type);
                        if (auto found = tableFind(*tbl, key)) {
                            ag->tag = storeString(optName + ".Some");
                            ag->fields.push_back(std::move(*found));
                        } else {
                            ag->tag = storeString(optName + ".None");
                        }
                        operands.emplace_back(std::move(ag));
                    } else if (fnName == "mapHas") {
                        auto key = pop();
                        const auto tbl = std::get<TablePtr>(pop());
                        operands.emplace_back(tableFind(*tbl, key).has_value());
                    } else if (fnName == "mapDel") {
                        auto key = pop();
                        bool deleted = false;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& tbl = std::get<TablePtr>(target);
                            deleted = tableDel(tbl, key, i.span);
                        });
                        operands.emplace_back(deleted);
                    } else if (fnName == "mapKeys" || fnName == "mapValues" || fnName == "setItems") {
                        const auto tbl = std::get<TablePtr>(pop());
                        try {
                            auto buf = std::make_shared<ArrayBuffer>();
                            buf->items.reserve(tbl->len);
                            const bool wantVals = (fnName == "mapValues");
                            for (std::uint32_t idx = 0; idx < tbl->buckets; ++idx) {
                                if ((tbl->ctrl[idx] & 0x80u) != 0u) continue;
                                buf->items.push_back(wantVals ? tbl->vals[idx] : tbl->keys[idx]);
                            }
                            buf->cap = buf->items.size();
                            operands.emplace_back(std::move(buf));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "setAdd") {
                        auto item = pop();
                        bool inserted = false;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& tbl = std::get<TablePtr>(target);
                            inserted = tableInsert(tbl, item, Value{}, i.span);
                        });
                        operands.emplace_back(inserted);
                    } else if (fnName == "setHas") {
                        auto item = pop();
                        const auto tbl = std::get<TablePtr>(pop());
                        operands.emplace_back(tableFind(*tbl, item).has_value());
                    } else if (fnName == "setDel") {
                        auto item = pop();
                        bool deleted = false;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& tbl = std::get<TablePtr>(target);
                            deleted = tableDel(tbl, item, i.span);
                        });
                        operands.emplace_back(deleted);
                    } else if (fnName == "sort" || fnName == "sortStable") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& arr = std::get<ArrayPtr>(target);
                            if (arr.use_count() > 1) {
                                try { arr = std::make_shared<ArrayBuffer>(*arr); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            const Type elemT = programRef.elemOf(i.expr->args[0].type);
                            auto less = [&](const Value& a, const Value& b) -> bool {
                                if (elemT == Type::Int) return std::get<Integer>(a) < std::get<Integer>(b);
                                if (Program::isSignedInt(elemT)) {
                                    const unsigned w = Program::intBitWidth(elemT);
                                    return signExtendWidth(std::get<ExactInt>(a).bits, w) <
                                           signExtendWidth(std::get<ExactInt>(b).bits, w);
                                }
                                if (Program::isUnsignedInt(elemT))
                                    return std::get<ExactInt>(a).bits < std::get<ExactInt>(b).bits;
                                if (elemT == Type::Float) return floatTotalKey(std::get<double>(a)) < floatTotalKey(std::get<double>(b));
                                if (elemT == Type::F32) return float32TotalKey(std::get<float>(a)) < float32TotalKey(std::get<float>(b));
                                if (elemT == Type::Str) return std::get<std::string_view>(a) < std::get<std::string_view>(b);
                                return !std::get<bool>(a) && std::get<bool>(b);
                            };
                            if (fnName == "sortStable") std::stable_sort(arr->items.begin(), arr->items.end(), less);
                            else std::sort(arr->items.begin(), arr->items.end(), less);
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "poolNew") {
                        try { operands.emplace_back(std::make_shared<PoolObject>()); }
                        catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "poolWithCap") {
                        const Integer c = std::get<Integer>(pop());
                        try {
                            auto pool = std::make_shared<PoolObject>();
                            poolReserve(*pool, c, i.span);
                            operands.emplace_back(std::move(pool));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "poolAdd") {
                        auto val = pop();
                        HandleValue handle{};
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& pool = std::get<PoolPtr>(target);
                            if (pool.use_count() > 1) {
                                try { pool = std::make_shared<PoolObject>(*pool); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            PoolObject& p = *pool;
                            if (!p.freeList.empty()) {
                                const std::uint32_t idx = p.freeList.back();
                                p.freeList.pop_back();
                                PoolSlot& slot = p.slots[idx];
                                slot.val = std::move(val);
                                slot.alive = true;
                                ++p.len;
                                handle = HandleValue{idx, slot.gen};
                            } else {
                                if (p.used >= static_cast<std::size_t>(maxPoolCap)) allocationFailed(i.span);
                                if (p.used >= p.cap) poolReserve(p, static_cast<Integer>(p.used + 1), i.span);
                                const auto idx = static_cast<std::uint32_t>(p.used++);
                                PoolSlot& slot = p.slots[idx];
                                slot.val = std::move(val);
                                slot.gen = 1u;
                                slot.alive = true;
                                ++p.len;
                                handle = HandleValue{idx, 1u};
                            }
                        });
                        operands.emplace_back(handle);
                    } else if (fnName == "poolGet") {
                        const auto h = std::get<HandleValue>(pop());
                        const auto pool = std::get<PoolPtr>(pop());
                        auto ag = std::make_shared<Aggregate>();
                        const std::string optName = programRef.nameOf(i.expr->type);
                        if (h.gen != 0u && h.idx < pool->used && pool->slots[h.idx].alive &&
                            pool->slots[h.idx].gen == h.gen) {
                            ag->tag = storeString(optName + ".Some");
                            ag->fields.push_back(pool->slots[h.idx].val);
                        } else {
                            ag->tag = storeString(optName + ".None");
                        }
                        operands.emplace_back(std::move(ag));
                    } else if (fnName == "poolHas") {
                        const auto h = std::get<HandleValue>(pop());
                        const auto pool = std::get<PoolPtr>(pop());
                        const bool ok = h.gen != 0u && h.idx < pool->used && pool->slots[h.idx].alive &&
                                        pool->slots[h.idx].gen == h.gen;
                        operands.emplace_back(ok);
                    } else if (fnName == "poolSet") {
                        auto val = pop();
                        const auto h = std::get<HandleValue>(pop());
                        bool ok = false;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& pool = std::get<PoolPtr>(target);
                            if (h.gen == 0u || h.idx >= pool->used || !pool->slots[h.idx].alive ||
                                pool->slots[h.idx].gen != h.gen)
                                return;
                            if (pool.use_count() > 1) {
                                try { pool = std::make_shared<PoolObject>(*pool); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            pool->slots[h.idx].val = std::move(val);
                            ok = true;
                        });
                        operands.emplace_back(ok);
                    } else if (fnName == "poolRemove") {
                        const auto h = std::get<HandleValue>(pop());
                        std::optional<Value> removed;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& pool = std::get<PoolPtr>(target);
                            if (h.gen == 0u || h.idx >= pool->used || !pool->slots[h.idx].alive ||
                                pool->slots[h.idx].gen != h.gen)
                                return;
                            if (pool.use_count() > 1) {
                                try { pool = std::make_shared<PoolObject>(*pool); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            PoolSlot& slot = pool->slots[h.idx];
                            removed = std::move(slot.val);
                            slot.val = Value{};
                            slot.alive = false;
                            ++slot.gen;
                            --pool->len;
                            if (slot.gen != 0u) pool->freeList.push_back(h.idx);
                        });
                        auto ag = std::make_shared<Aggregate>();
                        const std::string optName = programRef.nameOf(i.expr->type);
                        if (removed) {
                            ag->tag = storeString(optName + ".Some");
                            ag->fields.push_back(std::move(*removed));
                        } else {
                            ag->tag = storeString(optName + ".None");
                        }
                        operands.emplace_back(std::move(ag));
                    } else if (fnName == "poolHandles") {
                        const auto pool = std::get<PoolPtr>(pop());
                        try {
                            auto buf = std::make_shared<ArrayBuffer>();
                            buf->items.reserve(pool->len);
                            for (std::size_t idx = 0; idx < pool->used; ++idx) {
                                if (pool->slots[idx].alive)
                                    buf->items.emplace_back(HandleValue{static_cast<std::uint32_t>(idx), pool->slots[idx].gen});
                            }
                            buf->cap = buf->items.size();
                            operands.emplace_back(std::move(buf));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "sharedNew") {
                        auto val = pop();
                        try { operands.emplace_back(std::make_shared<SharedObject>(SharedObject{std::move(val)})); }
                        catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "sharedGet") {
                        const auto sh = std::get<SharedPtr>(pop());
                        operands.push_back(sh->payload);
                    } else if (fnName == "sharedSet") {
                        auto val = pop();
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sh = std::get<SharedPtr>(target);
                            if (sh.use_count() > 1) {
                                try { sh = std::make_shared<SharedObject>(SharedObject{std::move(val)}); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            } else {
                                sh->payload = std::move(val);
                            }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "sharedUpdate") {
                        const ClosurePtr cp = std::get<ClosurePtr>(pop());
                        Value curSharedVal;
                        {
                            const std::size_t cnt = countPlaceIndices(i.expr->args[0]);
                            const auto idxs = peekPlaceIndices(frame, cnt);
                            auto [rootName, steps] = buildPlaceSteps(i.expr->args[0], idxs);
                            Value& root = frame.locals.find(std::string(rootName))->target();
                            curSharedVal = steps.empty() ? root : readPlace(root, steps, i.span);
                        }
                        const Value oldPayload = std::get<SharedPtr>(curSharedVal)->payload;
                        const auto& cl = programRef.closures.at(cp->id);
                        if (cl.envStruct != Type::Void) operands.push_back(cp->env);
                        operands.push_back(oldPayload);
                        activate(*functions.at(cl.fnName), i.span, nullptr);
                    } else if (fnName == "fileOpen") {
                        const auto modeSv = std::get<std::string_view>(pop());
                        const auto pathSv = std::get<std::string_view>(pop());
                        const char* cMode = nullptr;
                        if (modeSv == "r") cMode = "rb";
                        else if (modeSv == "w") cMode = "wb";
                        else if (modeSv == "a") cMode = "ab";
                        else if (modeSv == "r+") cMode = "r+b";
                        else if (modeSv == "w+") cMode = "w+b";
                        else if (modeSv == "a+") cMode = "a+b";
                        if (!cMode) {
                            operands.push_back(makeResultErr(i.expr->type, "Modo de archivo inválido"));
                        } else if (pathSv.empty()) {
                            operands.push_back(makeResultErr(i.expr->type, "No se pudo abrir el archivo"));
                        } else {
                            FILE* fp = nullptr;
#ifdef _WIN32
                            const int wpn = MultiByteToWideChar(CP_UTF8, 0, pathSv.data(), static_cast<int>(pathSv.size()), nullptr, 0);
                            if (wpn > 0) {
                                std::wstring wp(static_cast<std::size_t>(wpn), L'\0');
                                MultiByteToWideChar(CP_UTF8, 0, pathSv.data(), static_cast<int>(pathSv.size()), wp.data(), wpn);
                                wchar_t wmode[8]{};
                                for (std::size_t mi = 0; cMode[mi] && mi + 1 < 8; ++mi) wmode[mi] = static_cast<wchar_t>(cMode[mi]);
                                fp = _wfsopen(wp.c_str(), wmode, _SH_DENYNO);
                            }
#else
                            std::string pathStr(pathSv);
                            fp = std::fopen(pathStr.c_str(), cMode);
#endif
                            if (!fp) operands.push_back(makeResultErr(i.expr->type, "No se pudo abrir el archivo"));
                            else {
                                auto f = std::make_shared<FileObject>();
                                f->fp = fp;
                                operands.push_back(makeResultOk(i.expr->type, std::move(f)));
                            }
                        }
                    } else if (fnName == "fileRead") {
                        const Integer maxBytes = std::get<Integer>(pop());
                        if (maxBytes < 0) fail("R_ARRAY_SIZE", "La longitud de lectura debe ser no negativa", i.span, "fileRead(mut f, 1024)");
                        if (maxBytes > maxStringLength) allocationFailed(i.span);
                        FilePtr f;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<FilePtr>(&target)) f = *p;
                        });
                        if (!f || !f->fp) {
                            operands.push_back(makeResultErr(i.expr->type, "Archivo cerrado"));
                        } else if (maxBytes == 0) {
                            operands.push_back(makeResultOk(i.expr->type, std::string_view("")));
                        } else {
                            std::string buf(static_cast<std::size_t>(maxBytes), '\0');
                            const std::size_t got = std::fread(buf.data(), 1, static_cast<std::size_t>(maxBytes), f->fp);
                            if (got == 0 && std::ferror(f->fp)) {
                                operands.push_back(makeResultErr(i.expr->type, "Error de lectura en archivo"));
                            } else {
                                buf.resize(got);
                                operands.push_back(makeResultOk(i.expr->type, storeString(std::move(buf))));
                            }
                        }
                    } else if (fnName == "fileReadAll") {
                        FilePtr f;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<FilePtr>(&target)) f = *p;
                        });
                        if (!f || !f->fp) {
                            operands.push_back(makeResultErr(i.expr->type, "Archivo cerrado"));
                        } else {
                            std::string total;
                            char tmp[4096];
                            bool err = false;
                            for (;;) {
                                const std::size_t got = std::fread(tmp, 1, sizeof(tmp), f->fp);
                                if (got > 0) {
                                    if (got > static_cast<std::size_t>(maxStringLength) - total.size())
                                        allocationFailed(i.span);
                                    total.append(tmp, got);
                                }
                                if (got < sizeof(tmp)) {
                                    if (std::ferror(f->fp)) err = true;
                                    break;
                                }
                            }
                            if (err) operands.push_back(makeResultErr(i.expr->type, "Error de lectura en archivo"));
                            else operands.push_back(makeResultOk(i.expr->type, storeString(std::move(total))));
                        }
                    } else if (fnName == "fileReadLine") {
                        FilePtr f;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<FilePtr>(&target)) f = *p;
                        });
                        if (!f || !f->fp) {
                            operands.push_back(makeResultErr(i.expr->type, "Archivo cerrado"));
                        } else {
                            const Type optT = programRef.resultOk(i.expr->type);
                            std::string line;
                            bool any = false;
                            bool err = false;
                            for (;;) {
                                const int ch = std::fgetc(f->fp);
                                if (ch == EOF) {
                                    if (std::ferror(f->fp)) err = true;
                                    break;
                                }
                                any = true;
                                if (ch == '\n') break;
                                if (line.size() >= static_cast<std::size_t>(maxStringLength))
                                    allocationFailed(i.span);
                                line.push_back(static_cast<char>(ch));
                            }
                            if (err) {
                                operands.push_back(makeResultErr(i.expr->type, "Error de lectura en archivo"));
                            } else if (!any) {
                                operands.push_back(makeResultOk(i.expr->type, makeOptionValue(optT, std::nullopt)));
                            } else {
                                if (!line.empty() && line.back() == '\r') line.pop_back();
                                operands.push_back(makeResultOk(i.expr->type, makeOptionValue(optT, Value{storeString(std::move(line))})));
                            }
                        }
                    } else if (fnName == "fileWrite") {
                        const auto dataSv = std::get<std::string_view>(pop());
                        FilePtr f;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<FilePtr>(&target)) f = *p;
                        });
                        if (!f || !f->fp) {
                            operands.push_back(makeResultErr(i.expr->type, "Archivo cerrado"));
                        } else if (dataSv.empty()) {
                            operands.push_back(makeResultOk(i.expr->type, Integer{0}));
                        } else {
                            const std::size_t put = std::fwrite(dataSv.data(), 1, dataSv.size(), f->fp);
                            if (put != dataSv.size() || std::ferror(f->fp))
                                operands.push_back(makeResultErr(i.expr->type, "Error de escritura en archivo"));
                            else
                                operands.push_back(makeResultOk(i.expr->type, static_cast<Integer>(put)));
                        }
                    } else if (fnName == "fileFlush") {
                        FilePtr f;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<FilePtr>(&target)) f = *p;
                        });
                        if (!f || !f->fp) {
                            operands.push_back(makeResultErr(i.expr->type, "Archivo cerrado"));
                        } else if (std::fflush(f->fp) != 0) {
                            operands.push_back(makeResultErr(i.expr->type, "Error al vaciar el archivo"));
                        } else {
                            operands.push_back(makeResultOk(i.expr->type, true));
                        }
                    } else if (fnName == "fileClose") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<FilePtr>(&target); p && *p) (*p)->close();
                            target = Value{};
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "fileIsOpen") {
                        const Value v = pop();
                        const auto* p = std::get_if<FilePtr>(&v);
                        operands.emplace_back(p && *p && (*p)->fp != nullptr);
                    } else if (fnName == "tcpListen") {
                        const Integer portVal = std::get<Integer>(pop());
                        const auto hostSv = std::get<std::string_view>(pop());
                        if (portVal < 0 || portVal > 65535) {
                            operands.push_back(makeResultErr(i.expr->type, "Puerto inválido"));
                        } else {
                            ensureWinsock();
                            std::string hostStr = hostSv.empty() ? "0.0.0.0" : (hostSv == "localhost" ? "127.0.0.1" : std::string(hostSv));
                            struct sockaddr_in addr{};
                            addr.sin_family = AF_INET;
                            addr.sin_port = htons(static_cast<std::uint16_t>(portVal));
                            bool resolved = (inet_pton(AF_INET, hostStr.c_str(), &addr.sin_addr) == 1);
                            if (!resolved) {
                                struct addrinfo hints{};
                                hints.ai_family = AF_INET;
                                hints.ai_socktype = SOCK_STREAM;
                                struct addrinfo* res = nullptr;
                                if (getaddrinfo(hostStr.c_str(), nullptr, &hints, &res) == 0 && res) {
                                    addr.sin_addr = reinterpret_cast<const struct sockaddr_in*>(res->ai_addr)->sin_addr;
                                    resolved = true;
                                    freeaddrinfo(res);
                                }
                            }
                            if (!resolved) {
                                operands.push_back(makeResultErr(i.expr->type, "No se pudo escuchar en el puerto"));
                            } else {
#ifdef _WIN32
                                SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                                const bool badSock = (s == INVALID_SOCKET);
#else
                                int s = socket(AF_INET, SOCK_STREAM, 0);
                                const bool badSock = (s < 0);
#endif
                                if (badSock) {
                                    operands.push_back(makeResultErr(i.expr->type, "No se pudo crear el socket"));
                                } else {
                                    int yes = 1;
                                    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#ifdef _WIN32
                                    using SockLen = int;
#else
                                    using SockLen = socklen_t;
#endif
                                    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), static_cast<SockLen>(sizeof(addr))) != 0 ||
                                        listen(s, 64) != 0) {
#ifdef _WIN32
                                        closesocket(s);
#else
                                        ::close(s);
#endif
                                        operands.push_back(makeResultErr(i.expr->type, "No se pudo escuchar en el puerto"));
                                    } else {
                                        struct sockaddr_in bound{};
                                        SockLen blen = static_cast<SockLen>(sizeof(bound));
                                        int actualPort = static_cast<int>(portVal);
                                        if (getsockname(s, reinterpret_cast<struct sockaddr*>(&bound), &blen) == 0)
                                            actualPort = static_cast<int>(ntohs(bound.sin_port));
                                        auto lst = std::make_shared<TcpListenerObject>();
                                        lst->fd = static_cast<std::uintptr_t>(s);
                                        lst->port = actualPort;
                                        lst->open = true;
                                        operands.push_back(makeResultOk(i.expr->type, std::move(lst)));
                                    }
                                }
                            }
                        }
                    } else if (fnName == "tcpLocalPort") {
                        const Value v = pop();
                        const auto* p = std::get_if<TcpListenerPtr>(&v);
                        operands.emplace_back(Integer{(p && *p && (*p)->open) ? (*p)->port : 0});
                    } else if (fnName == "tcpAccept") {
                        TcpListenerPtr lst;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<TcpListenerPtr>(&target)) lst = *p;
                        });
                        if (!lst || !lst->open) {
                            operands.push_back(makeResultErr(i.expr->type, "Listener cerrado"));
                        } else {
#ifdef _WIN32
                            SOCKET cli = accept(static_cast<SOCKET>(lst->fd), nullptr, nullptr);
                            const bool badCli = (cli == INVALID_SOCKET);
#else
                            int cli = accept(static_cast<int>(lst->fd), nullptr, nullptr);
                            const bool badCli = (cli < 0);
#endif
                            if (badCli) {
                                operands.push_back(makeResultErr(i.expr->type, "Error al aceptar conexión"));
                            } else {
                                auto sk = std::make_shared<SocketObject>();
                                sk->fd = static_cast<std::uintptr_t>(cli);
                                sk->open = true;
                                operands.push_back(makeResultOk(i.expr->type, std::move(sk)));
                            }
                        }
                    } else if (fnName == "tcpConnect") {
                        const Integer portVal = std::get<Integer>(pop());
                        const auto hostSv = std::get<std::string_view>(pop());
                        if (portVal <= 0 || portVal > 65535) {
                            operands.push_back(makeResultErr(i.expr->type, "Puerto inválido"));
                        } else {
                            ensureWinsock();
                            std::string hostStr = hostSv.empty() ? "127.0.0.1" : (hostSv == "localhost" ? "127.0.0.1" : std::string(hostSv));
                            std::string portStr = std::to_string(portVal);
                            struct addrinfo hints{};
                            hints.ai_family = AF_INET;
                            hints.ai_socktype = SOCK_STREAM;
                            struct addrinfo* res = nullptr;
                            if (getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) {
                                operands.push_back(makeResultErr(i.expr->type, "No se pudo conectar"));
                            } else {
#ifdef _WIN32
                                SOCKET conn = INVALID_SOCKET;
                                for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
                                    SOCKET s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                                    if (s == INVALID_SOCKET) continue;
                                    if (connect(s, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
                                        conn = s;
                                        break;
                                    }
                                    closesocket(s);
                                }
                                const bool ok = (conn != INVALID_SOCKET);
#else
                                int conn = -1;
                                for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
                                    int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                                    if (s < 0) continue;
                                    if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
                                        conn = s;
                                        break;
                                    }
                                    ::close(s);
                                }
                                const bool ok = (conn >= 0);
#endif
                                freeaddrinfo(res);
                                if (!ok) {
                                    operands.push_back(makeResultErr(i.expr->type, "No se pudo conectar"));
                                } else {
                                    auto sk = std::make_shared<SocketObject>();
                                    sk->fd = static_cast<std::uintptr_t>(conn);
                                    sk->open = true;
                                    operands.push_back(makeResultOk(i.expr->type, std::move(sk)));
                                }
                            }
                        }
                    } else if (fnName == "socketRead") {
                        const Integer maxBytes = std::get<Integer>(pop());
                        if (maxBytes < 0) fail("R_ARRAY_SIZE", "La longitud de lectura debe ser no negativa", i.span, "socketRead(mut s, 1024)");
                        if (maxBytes > maxStringLength) allocationFailed(i.span);
                        SocketPtr sk;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<SocketPtr>(&target)) sk = *p;
                        });
                        if (!sk || !sk->open) {
                            operands.push_back(makeResultErr(i.expr->type, "Socket cerrado"));
                        } else if (maxBytes == 0) {
                            operands.push_back(makeResultOk(i.expr->type, std::string_view("")));
                        } else {
                            std::string buf(static_cast<std::size_t>(maxBytes), '\0');
#ifdef _WIN32
                            const int n = recv(static_cast<SOCKET>(sk->fd), buf.data(), static_cast<int>(maxBytes), 0);
                            if (n < 0) {
                                operands.push_back(makeResultErr(i.expr->type, "Error de lectura en socket"));
                            } else {
                                buf.resize(static_cast<std::size_t>(n));
                                operands.push_back(makeResultOk(i.expr->type, storeString(std::move(buf))));
                            }
#else
                            ssize_t n = 0;
                            do {
                                n = recv(static_cast<int>(sk->fd), buf.data(), static_cast<std::size_t>(maxBytes), 0);
                            } while (n < 0 && errno == EINTR);
                            if (n < 0) {
                                operands.push_back(makeResultErr(i.expr->type, "Error de lectura en socket"));
                            } else {
                                buf.resize(static_cast<std::size_t>(n));
                                operands.push_back(makeResultOk(i.expr->type, storeString(std::move(buf))));
                            }
#endif
                        }
                    } else if (fnName == "socketWrite") {
                        const auto dataSv = std::get<std::string_view>(pop());
                        SocketPtr sk;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<SocketPtr>(&target)) sk = *p;
                        });
                        if (!sk || !sk->open) {
                            operands.push_back(makeResultErr(i.expr->type, "Socket cerrado"));
                        } else {
                            std::size_t sent = 0;
                            bool err = false;
                            while (sent < dataSv.size()) {
#ifdef _WIN32
                                const int n = send(static_cast<SOCKET>(sk->fd), dataSv.data() + sent,
                                                   static_cast<int>(dataSv.size() - sent), 0);
                                if (n <= 0) { err = true; break; }
#else
                                const ssize_t n = send(static_cast<int>(sk->fd), dataSv.data() + sent, dataSv.size() - sent, 0);
                                if (n < 0 && errno == EINTR) continue;
                                if (n <= 0) { err = true; break; }
#endif
                                sent += static_cast<std::size_t>(n);
                            }
                            if (err) operands.push_back(makeResultErr(i.expr->type, "Error de escritura en socket"));
                            else operands.push_back(makeResultOk(i.expr->type, static_cast<Integer>(sent)));
                        }
                    } else if (fnName == "socketClose") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<SocketPtr>(&target); p && *p) (*p)->close();
                            target = Value{};
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "socketIsOpen") {
                        const Value v = pop();
                        const auto* p = std::get_if<SocketPtr>(&v);
                        operands.emplace_back(p && *p && (*p)->open);
                    } else if (fnName == "listenerClose") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<TcpListenerPtr>(&target); p && *p) (*p)->close();
                            target = Value{};
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "listenerIsOpen") {
                        const Value v = pop();
                        const auto* p = std::get_if<TcpListenerPtr>(&v);
                        operands.emplace_back(p && *p && (*p)->open);
                    } else if (fnName == "mutexNew") {
                        auto m = std::make_shared<MutexObject>();
                        m->state = std::make_shared<MutexState>();
                        operands.emplace_back(std::move(m));
                    } else if (fnName == "mutexLock") {
                        MutexPtr mp;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<MutexPtr>(&target)) mp = *p;
                        });
                        if (!mp || !mp->state) allocationFailed(i.span);
                        {
                            std::unique_lock<std::mutex> lk(mp->state->mu);
                            mp->state->cv.wait(lk, [&]() { return !mp->state->locked; });
                            mp->state->locked = true;
                        }
                        auto guard = std::make_shared<LockObject>();
                        guard->state = mp->state;
                        operands.emplace_back(std::move(guard));
                    } else if (fnName == "mutexTryLock") {
                        MutexPtr mp;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<MutexPtr>(&target)) mp = *p;
                        });
                        std::optional<Value> got;
                        if (mp && mp->state) {
                            std::lock_guard<std::mutex> lk(mp->state->mu);
                            if (!mp->state->locked) {
                                mp->state->locked = true;
                                auto guard = std::make_shared<LockObject>();
                                guard->state = mp->state;
                                got = Value{std::move(guard)};
                            }
                        }
                        operands.push_back(makeOptionValue(i.expr->type, std::move(got)));
                    } else if (fnName == "unlock") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            if (auto* p = std::get_if<LockPtr>(&target); p && *p) (*p)->unlock();
                            target = Value{};
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "lockIsHeld") {
                        const Value v = pop();
                        const auto* p = std::get_if<LockPtr>(&v);
                        operands.emplace_back(p && *p && (*p)->state != nullptr);
                    } else if (fnName == "taskScope") {
                        operands.emplace_back(std::make_shared<ScopeObject>());
                    } else if (fnName == "taskSpawn") {
                        const auto cp = std::get<ClosurePtr>(pop());
                        Value res = callClosureSync(cp, {}, i.span, steps);
                        const bool hasRes = programRef.closures.at(cp->id).result != Type::Void;
                        auto task = std::make_shared<TaskObject>(TaskObject{std::move(res), false, hasRes});
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sp = std::get<ScopePtr>(target);
                            if (sp) sp->tasks.push_back(task);
                        });
                        operands.emplace_back(std::move(task));
                    } else if (fnName == "taskSpawnMove") {
                        const auto cp = std::get<ClosurePtr>(pop());
                        Value movedVal = pop();
                        std::vector<Value> cargs;
                        cargs.push_back(std::move(movedVal));
                        Value res = callClosureSync(cp, std::move(cargs), i.span, steps);
                        const bool hasRes = programRef.closures.at(cp->id).result != Type::Void;
                        auto task = std::make_shared<TaskObject>(TaskObject{std::move(res), false, hasRes});
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& sp = std::get<ScopePtr>(target);
                            if (sp) sp->tasks.push_back(task);
                        });
                        operands.emplace_back(std::move(task));
                    } else if (fnName == "taskJoin") {
                        auto tv = pop();
                        auto task = std::get<TaskPtr>(tv);
                        task->consumed = true;
                        Value res = task->hasResult ? std::move(task->result) : Value{};
                        task->result = Value{};
                        operands.push_back(std::move(res));
                    } else if (fnName == "parallelMap") {
                        const auto cp = std::get<ClosurePtr>(pop());
                        const Integer n = std::get<Integer>(pop());
                        if (n < 0) fail("R_ARRAY_SIZE", "La longitud debe ser no negativa", i.span, "parallelMap(8, |i| i * 2)");
                        if (n > maxArrayLength) allocationFailed(i.span);
                        auto buf = std::make_shared<ArrayBuffer>();
                        buf->items.reserve(static_cast<std::size_t>(n));
                        for (Integer k = 0; k < n; ++k) {
                            std::vector<Value> cargs;
                            cargs.emplace_back(k);
                            buf->items.push_back(callClosureSync(cp, std::move(cargs), i.span, steps));
                        }
                        buf->cap = buf->items.size();
                        operands.emplace_back(std::move(buf));
                    } else if (fnName == "freeze") {
                        const Value v = pop();
                        operands.emplace_back(std::make_shared<FrozenObject>(FrozenObject{cloneValue(v)}));
                    } else if (fnName == "frozenGet") {
                        const auto fz = std::get<FrozenPtr>(pop());
                        operands.push_back(cloneValue(fz->val));
                    } else if (fnName == "frozenRead") {
                        const auto cp = std::get<ClosurePtr>(pop());
                        const auto fz = std::get<FrozenPtr>(pop());
                        std::vector<Value> cargs;
                        cargs.push_back(fz->val);
                        operands.push_back(callClosureSync(cp, std::move(cargs), i.span, steps));
                    } else if (fnName == "ptrNull" || fnName == "mutPtrNull") {
                        operands.emplace_back(RawPtrVal{i.expr->typeArgs.at(0), 0});
                    } else if (fnName == "ptrIsNull") {
                        const auto rp = std::get<RawPtrVal>(pop());
                        operands.emplace_back(vmRawPtrAddr(rp) == 0);
                    } else if (fnName == "ptrAddr") {
                        const auto rp = std::get<RawPtrVal>(pop());
                        const std::uintptr_t a = vmRawPtrAddr(rp);
                        if (a != 0 && (rp.vmSlot != nullptr || rp.vmArr != nullptr)) vmAddrMap[a] = rp;
                        operands.push_back(makeIntValue(Type::U64, static_cast<std::uint64_t>(a)));
                    } else if (fnName == "ptrConst" || fnName == "ptrMut") {
                        auto rp = std::get<RawPtrVal>(pop());
                        operands.emplace_back(std::move(rp));
                    } else if (fnName == "ptrCast" || fnName == "mutPtrCast") {
                        auto rp = std::get<RawPtrVal>(pop());
                        rp.pointee = i.expr->typeArgs.at(0);
                        operands.emplace_back(std::move(rp));
                    } else if (fnName == "ptrFromAddr" || fnName == "mutPtrFromAddr") {
                        const Value v = pop();
                        const std::uint64_t a = std::holds_alternative<ExactInt>(v)
                            ? std::get<ExactInt>(v).bits
                            : static_cast<std::uint64_t>(std::get<Integer>(v));
                        operands.emplace_back(vmPtrFromAddr(static_cast<std::uintptr_t>(a), i.expr->typeArgs.at(0)));
                    } else if (fnName == "ptrOf" || fnName == "mutPtrOf") {
                        operands.emplace_back(vmMakePlacePtr(frame, i.expr->args[0], i.span, fnName == "mutPtrOf"));
                    } else if (fnName == "slicePtr") {
                        auto arr = std::get<ArrayPtr>(pop());
                        const Type elemT = programRef.ptrElem(i.expr->type);
                        if (!arr || (arr->items.empty() && arr->cap == 0)) {
                            operands.emplace_back(RawPtrVal{elemT, 0});
                        } else {
                            operands.emplace_back(RawPtrVal{elemT, 0, nullptr, std::move(arr), elemT, 0});
                        }
                    } else if (fnName == "mutSlicePtr") {
                        const Type elemT = programRef.ptrElem(i.expr->type);
                        RawPtrVal res{elemT, 0};
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& arr = std::get<ArrayPtr>(target);
                            if (arr && arr.use_count() > 1) {
                                try { arr = std::make_shared<ArrayBuffer>(*arr); }
                                catch (const std::bad_alloc&) { allocationFailed(i.span); }
                            }
                            if (arr && (!arr->items.empty() || arr->cap > 0)) {
                                res = RawPtrVal{elemT, 0, nullptr, arr, elemT, 0};
                            }
                        });
                        operands.emplace_back(std::move(res));
                    } else if (fnName == "ptrAdd") {
                        const Integer off = std::get<Integer>(pop());
                        auto rp = std::get<RawPtrVal>(pop());
                        if (!i.rawMode && vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        operands.emplace_back(vmAdvancePtr(std::move(rp), off, i.span));
                    } else if (fnName == "ptrRead") {
                        const auto rp = std::get<RawPtrVal>(pop());
                        if (!i.rawMode && vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        if (vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        const Type elemT = i.expr->type;
                        const std::size_t elemSz = vmElemSize(elemT);
                        std::vector<std::uint8_t> buf(elemSz, 0);
                        vmReadBytes(rp, buf.data(), elemSz, i.span);
                        operands.push_back(unpackValueFromBytes(elemT, buf.data()));
                    } else if (fnName == "ptrWrite") {
                        const Value val = pop();
                        const auto rp = std::get<RawPtrVal>(pop());
                        if (!i.rawMode && vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        if (vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        const Type elemT = programRef.ptrElem(i.expr->args[0].type);
                        const std::size_t elemSz = vmElemSize(elemT);
                        std::vector<std::uint8_t> buf(elemSz, 0);
                        packValueToBytes(elemT, val, buf.data());
                        vmWriteBytes(rp, buf.data(), elemSz, i.span);
                        operands.emplace_back(Value{});
                    } else if (fnName == "ptrAlloc" || fnName == "ptrAllocAligned") {
                        Integer align = 16;
                        if (fnName == "ptrAllocAligned") align = std::get<Integer>(pop());
                        const Integer count = std::get<Integer>(pop());
                        if (count < 0) fail("R_ARRAY_SIZE", "Cantidad negativa en " + fnName, i.span, "ptrAlloc<u8>(8)");
                        if (fnName == "ptrAllocAligned" && (align <= 0 || (align & (align - 1)) != 0))
                            fail("R_ARRAY_SIZE", "Alineación inválida en ptrAllocAligned (requiere potencia de 2 positiva)", i.span, "ptrAllocAligned<u8>(8, 16)");
                        const Type elemT = i.expr->typeArgs.at(0);
                        const std::size_t elemSz = vmElemSize(elemT);
                        const std::uint64_t ucnt = static_cast<std::uint64_t>(count);
                        if (ucnt > 0 && static_cast<std::uint64_t>(elemSz) > std::numeric_limits<std::size_t>::max() / ucnt)
                            fail("R_ARRAY_SIZE", "Desbordamiento de tamaño en " + fnName, i.span, "ptrAlloc<u8>(8)");
                        const std::size_t totalBytes = static_cast<std::size_t>(ucnt * elemSz);
                        const std::size_t allocBytes = totalBytes == 0 ? 1 : totalBytes;
                        void* mem = nullptr;
                        bool usedAligned = false;
#ifdef _WIN32
                        if (fnName == "ptrAllocAligned") {
                            mem = _aligned_malloc(allocBytes, static_cast<std::size_t>(align));
                            usedAligned = true;
                        } else {
                            mem = std::malloc(allocBytes);
                        }
#else
                        if (fnName == "ptrAllocAligned") {
                            const std::size_t effAlign = std::max<std::size_t>(static_cast<std::size_t>(align), sizeof(void*));
                            if (posix_memalign(&mem, effAlign, allocBytes) != 0) mem = nullptr;
                            usedAligned = true;
                        } else {
                            mem = std::malloc(allocBytes);
                        }
#endif
                        if (!mem) allocationFailed(i.span);
                        const auto a = reinterpret_cast<std::uintptr_t>(mem);
                        vmAllocs.emplace(a, VmAlloc{allocBytes, usedAligned});
                        operands.emplace_back(RawPtrVal{elemT, a});
                    } else if (fnName == "ptrFree") {
                        const auto rp = std::get<RawPtrVal>(pop());
                        const std::uintptr_t a = vmRawPtrAddr(rp);
                        if (a != 0) {
                            auto it = vmAllocs.find(a);
                            if (it == vmAllocs.end())
                                fail("R_NULL_PTR", "ptrFree requiere un puntero asignado con ptrAlloc/ptrAllocAligned", i.span, "ptrFree(p)");
#ifdef _WIN32
                            if (it->second.aligned) _aligned_free(reinterpret_cast<void*>(a));
                            else std::free(reinterpret_cast<void*>(a));
#else
                            std::free(reinterpret_cast<void*>(a));
#endif
                            vmAllocs.erase(it);
                        }
                        operands.emplace_back(Value{});
                    } else if (fnName == "ptrCopy") {
                        const Integer count = std::get<Integer>(pop());
                        const auto src = std::get<RawPtrVal>(pop());
                        const auto dst = std::get<RawPtrVal>(pop());
                        if (count < 0) fail("R_ARRAY_SIZE", "Cantidad negativa en ptrCopy", i.span, "ptrCopy(dst, src, 1)");
                        const Type elemT = programRef.ptrElem(i.expr->args[0].type);
                        const std::size_t elemSz = vmElemSize(elemT);
                        const std::uint64_t ucnt = static_cast<std::uint64_t>(count);
                        if (ucnt > 0 && static_cast<std::uint64_t>(elemSz) > std::numeric_limits<std::size_t>::max() / ucnt)
                            fail("R_ARRAY_SIZE", "Desbordamiento de tamaño en ptrCopy", i.span, "ptrCopy(dst, src, 1)");
                        const std::size_t bytes = static_cast<std::size_t>(ucnt * elemSz);
                        if (!i.rawMode && bytes > 0 && (vmRawPtrAddr(dst) == 0 || vmRawPtrAddr(src) == 0))
                            failNullPtr(i.span);
                        if (bytes > 0) {
                            std::vector<std::uint8_t> tmp(bytes, 0);
                            vmReadBytes(src, tmp.data(), bytes, i.span);
                            vmWriteBytes(dst, tmp.data(), bytes, i.span);
                        }
                        operands.emplace_back(Value{});
                    } else if (fnName == "ptrZero") {
                        const Integer count = std::get<Integer>(pop());
                        const auto dst = std::get<RawPtrVal>(pop());
                        if (count < 0) fail("R_ARRAY_SIZE", "Cantidad negativa en ptrZero", i.span, "ptrZero(dst, 1)");
                        const Type elemT = programRef.ptrElem(i.expr->args[0].type);
                        const std::size_t elemSz = vmElemSize(elemT);
                        const std::uint64_t ucnt = static_cast<std::uint64_t>(count);
                        if (ucnt > 0 && static_cast<std::uint64_t>(elemSz) > std::numeric_limits<std::size_t>::max() / ucnt)
                            fail("R_ARRAY_SIZE", "Desbordamiento de tamaño en ptrZero", i.span, "ptrZero(dst, 1)");
                        const std::size_t bytes = static_cast<std::size_t>(ucnt * elemSz);
                        if (!i.rawMode && bytes > 0 && vmRawPtrAddr(dst) == 0) failNullPtr(i.span);
                        if (bytes > 0) {
                            std::vector<std::uint8_t> zeros(bytes, 0);
                            vmWriteBytes(dst, zeros.data(), bytes, i.span);
                        }
                        operands.emplace_back(Value{});
                    } else if (fnName == "cstr") {
                        const auto sv = std::get<std::string_view>(pop());
                        if (sv.size() >= static_cast<std::size_t>(maxArrayLength)) allocationFailed(i.span);
                        auto buf = std::make_shared<ArrayBuffer>();
                        buf->cap = sv.size() + 1;
                        buf->items.reserve(sv.size() + 1);
                        for (unsigned char ch : sv) buf->items.push_back(makeIntValue(Type::U8, ch));
                        buf->items.push_back(makeIntValue(Type::U8, 0));
                        operands.emplace_back(std::move(buf));
                    } else if (fnName == "strFromCStr") {
                        const auto rp = std::get<RawPtrVal>(pop());
                        if (!i.rawMode && vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        std::string outStr;
                        for (std::size_t off = 0;; ++off) {
                            RawPtrVal cur = vmAdvancePtr(rp, static_cast<Integer>(off), i.span);
                            std::uint8_t b = 0;
                            vmReadBytes(cur, &b, 1, i.span);
                            if (b == 0) break;
                            if (outStr.size() >= static_cast<std::size_t>(maxStringLength)) allocationFailed(i.span);
                            outStr.push_back(static_cast<char>(b));
                        }
                        operands.emplace_back(storeString(std::move(outStr)));
                    } else if (fnName == "strFromPtr") {
                        const Integer lenVal = std::get<Integer>(pop());
                        const auto rp = std::get<RawPtrVal>(pop());
                        if (lenVal < 0 || lenVal > maxStringLength)
                            fail("R_ARRAY_SIZE", "Longitud fuera de rango en strFromPtr", i.span, "strFromPtr(p, n)");
                        const std::size_t n = static_cast<std::size_t>(lenVal);
                        if (!i.rawMode && n > 0 && vmRawPtrAddr(rp) == 0) failNullPtr(i.span);
                        std::string outStr(n, '\0');
                        if (n > 0) vmReadBytes(rp, reinterpret_cast<std::uint8_t*>(outStr.data()), n, i.span);
                        operands.emplace_back(storeString(std::move(outStr)));
                    } else if (fnName == "fnPtr") {
                        const std::string& sym = i.expr->labels.at(0);
                        auto [it, inserted] = fnSentinels.emplace(sym, nextFnSentinel);
                        if (inserted) nextFnSentinel += 8u;
                        operands.emplace_back(RawPtrVal{Type::Void, it->second});
                    } else if (fnName == "simdSplat") {
                        const Type vt = i.expr->type;
                        const Type elemT = programRef.simdElem(vt);
                        const unsigned lanes = programRef.simdLanes(vt);
                        const std::uint64_t bits = encodeSimdLane(elemT, pop());
                        SimdVal sv{elemT, lanes, {}};
                        for (unsigned k = 0; k < lanes; ++k) sv.setLane(k, bits);
                        operands.emplace_back(sv);
                    } else if (fnName == "simdLanes") {
                        const unsigned lanes = programRef.simdLanes(i.expr->typeArgs.at(0));
                        operands.emplace_back(static_cast<Integer>(lanes));
                    } else if (fnName == "simdGet") {
                        const Integer idx = std::get<Integer>(pop());
                        const auto sv = std::get<SimdVal>(pop());
                        if (idx < 0 || static_cast<unsigned>(idx) >= sv.lanes)
                            fail("R_BOUNDS", "Índice de carril fuera de límites en simdGet", i.span, "simdGet(v, 0)");
                        operands.push_back(decodeSimdLane(sv.elemType, sv.lane(static_cast<unsigned>(idx))));
                    } else if (fnName == "simdSet") {
                        const Value val = pop();
                        const Integer idx = std::get<Integer>(pop());
                        auto sv = std::get<SimdVal>(pop());
                        if (idx < 0 || static_cast<unsigned>(idx) >= sv.lanes)
                            fail("R_BOUNDS", "Índice de carril fuera de límites en simdSet", i.span, "simdSet(v, 0, x)");
                        sv.setLane(static_cast<unsigned>(idx), encodeSimdLane(sv.elemType, val));
                        operands.emplace_back(sv);
                    } else if (fnName == "simdFromArray") {
                        const Integer off = std::get<Integer>(pop());
                        const auto arr = std::get<ArrayPtr>(pop());
                        const Type vt = i.expr->type;
                        const Type elemT = programRef.simdElem(vt);
                        const unsigned lanes = programRef.simdLanes(vt);
                        const std::size_t n = arr ? arr->items.size() : 0;
                        if (off < 0 || static_cast<std::size_t>(off) > n || n - static_cast<std::size_t>(off) < lanes)
                            fail("R_BOUNDS", "Rango fuera de límites en simdFromArray", i.span, "simdFromArray<Simd<f32, 4>>(arr, 0)");
                        SimdVal sv{elemT, lanes, {}};
                        for (unsigned k = 0; k < lanes; ++k) {
                            sv.setLane(k, encodeSimdLane(elemT, arr->items[static_cast<std::size_t>(off) + k]));
                        }
                        operands.emplace_back(sv);
                    } else if (fnName == "simdToArray") {
                        const Integer off = std::get<Integer>(pop());
                        const auto sv = std::get<SimdVal>(pop());
                        mutateTarget(frame, i.expr->args[1], [&](Value& target) {
                            const auto& arr = std::get<ArrayPtr>(target);
                            const std::size_t n = arr ? arr->items.size() : 0;
                            if (off < 0 || static_cast<std::size_t>(off) > n || n - static_cast<std::size_t>(off) < sv.lanes)
                                fail("R_BOUNDS", "Rango fuera de límites en simdToArray", i.span, "simdToArray(v, mut arr, 0)");
                            for (unsigned k = 0; k < sv.lanes; ++k) {
                                arr->items[static_cast<std::size_t>(off) + k] = decodeSimdLane(sv.elemType, sv.lane(k));
                            }
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "simdLoad" || fnName == "simdLoadAligned") {
                        const auto rp = std::get<RawPtrVal>(pop());
                        const std::uintptr_t a = vmRawPtrAddr(rp);
                        if (!i.rawMode && a == 0) failNullPtr(i.span);
                        if (a == 0) failNullPtr(i.span);
                        const Type vt = i.expr->type;
                        const auto [bytes, align] = layoutCalc.sizeAlignOf(vt);
                        if (fnName == "simdLoadAligned" && !i.rawMode && (a % align) != 0)
                            fail("R_ALIGN", "Puntero desalineado en simdLoadAligned", i.span, "ptrAllocAligned<f32>(8, 32)");
                        std::vector<std::uint8_t> buf(bytes, 0);
                        vmReadBytes(rp, buf.data(), bytes, i.span);
                        operands.push_back(unpackValueFromBytes(vt, buf.data()));
                    } else if (fnName == "simdStore" || fnName == "simdStoreAligned") {
                        const Value v = pop();
                        const auto rp = std::get<RawPtrVal>(pop());
                        const std::uintptr_t a = vmRawPtrAddr(rp);
                        if (!i.rawMode && a == 0) failNullPtr(i.span);
                        if (a == 0) failNullPtr(i.span);
                        const Type vt = i.expr->args[1].type;
                        const auto [bytes, align] = layoutCalc.sizeAlignOf(vt);
                        if (fnName == "simdStoreAligned" && !i.rawMode && (a % align) != 0)
                            fail("R_ALIGN", "Puntero desalineado en simdStoreAligned", i.span, "ptrAllocAligned<f32>(8, 32)");
                        std::vector<std::uint8_t> buf(bytes, 0);
                        packValueToBytes(vt, v, buf.data());
                        vmWriteBytes(rp, buf.data(), bytes, i.span);
                        operands.emplace_back(Value{});
                    } else if (fnName == "simdFma") {
                        const auto c = std::get<SimdVal>(pop());
                        const auto b = std::get<SimdVal>(pop());
                        const auto a = std::get<SimdVal>(pop());
                        SimdVal out{a.elemType, a.lanes, {}};
                        for (unsigned k = 0; k < a.lanes; ++k) {
                            if (a.elemType == Type::Float) {
                                const double da = std::bit_cast<double>(a.lane(k));
                                const double db = std::bit_cast<double>(b.lane(k));
                                const double dc = std::bit_cast<double>(c.lane(k));
                                out.setLane(k, std::bit_cast<std::uint64_t>(std::fma(da, db, dc)));
                            } else {
                                const float fa = std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k)));
                                const float fb = std::bit_cast<float>(static_cast<std::uint32_t>(b.lane(k)));
                                const float fc = std::bit_cast<float>(static_cast<std::uint32_t>(c.lane(k)));
                                out.setLane(k, std::bit_cast<std::uint32_t>(std::fma(fa, fb, fc)));
                            }
                        }
                        operands.emplace_back(out);
                    } else if (fnName == "simdMin" || fnName == "simdMax") {
                        const bool isMin = (fnName == "simdMin");
                        const auto b = std::get<SimdVal>(pop());
                        const auto a = std::get<SimdVal>(pop());
                        SimdVal out{a.elemType, a.lanes, {}};
                        if (a.elemType == Type::Float) {
                            for (unsigned k = 0; k < a.lanes; ++k) {
                                const double da = std::bit_cast<double>(a.lane(k));
                                const double db = std::bit_cast<double>(b.lane(k));
                                out.setLane(k, std::bit_cast<std::uint64_t>((isMin ? (da < db) : (da > db)) ? da : db));
                            }
                        } else if (a.elemType == Type::F32) {
                            for (unsigned k = 0; k < a.lanes; ++k) {
                                const float fa = std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k)));
                                const float fb = std::bit_cast<float>(static_cast<std::uint32_t>(b.lane(k)));
                                out.setLane(k, std::bit_cast<std::uint32_t>((isMin ? (fa < fb) : (fa > fb)) ? fa : fb));
                            }
                        } else if (Program::isSignedInt(a.elemType)) {
                            const unsigned w = Program::intBitWidth(a.elemType);
                            for (unsigned k = 0; k < a.lanes; ++k) {
                                const auto av = a.lane(k), bv = b.lane(k);
                                const Integer sa = signExtendWidth(av, w);
                                const Integer sb = signExtendWidth(bv, w);
                                out.setLane(k, (isMin ? (sa < sb) : (sa > sb)) ? av : bv);
                            }
                        } else {
                            for (unsigned k = 0; k < a.lanes; ++k) {
                                const auto av = a.lane(k), bv = b.lane(k);
                                out.setLane(k, (isMin ? (av < bv) : (av > bv)) ? av : bv);
                            }
                        }
                        operands.emplace_back(out);
                    } else if (fnName == "simdAbs") {
                        const auto a = std::get<SimdVal>(pop());
                        SimdVal out{a.elemType, a.lanes, {}};
                        if (a.elemType == Type::Float) {
                            for (unsigned k = 0; k < a.lanes; ++k)
                                out.setLane(k, std::bit_cast<std::uint64_t>(std::fabs(std::bit_cast<double>(a.lane(k)))));
                        } else if (a.elemType == Type::F32) {
                            for (unsigned k = 0; k < a.lanes; ++k)
                                out.setLane(k, std::bit_cast<std::uint32_t>(std::fabs(std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k))))));
                        } else {
                            const unsigned w = Program::intBitWidth(a.elemType);
                            for (unsigned k = 0; k < a.lanes; ++k) {
                                const auto value = a.lane(k);
                                out.setLane(k, signExtendWidth(value, w) < 0 ? 0ULL - value : value);
                            }
                        }
                        operands.emplace_back(out);
                    } else if (fnName == "simdSqrt") {
                        const auto a = std::get<SimdVal>(pop());
                        SimdVal out{a.elemType, a.lanes, {}};
                        for (unsigned k = 0; k < a.lanes; ++k) {
                            if (a.elemType == Type::Float)
                                out.setLane(k, std::bit_cast<std::uint64_t>(std::sqrt(std::bit_cast<double>(a.lane(k)))));
                            else
                                out.setLane(k, std::bit_cast<std::uint32_t>(std::sqrt(std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k))))));
                        }
                        operands.emplace_back(out);
                    } else if (fnName == "simdSum") {
                        const auto a = std::get<SimdVal>(pop());
                        if (a.elemType == Type::Float) {
                            double acc = -0.0;
                            for (unsigned k = 0; k < a.lanes; ++k) acc += std::bit_cast<double>(a.lane(k));
                            operands.emplace_back(acc);
                        } else if (a.elemType == Type::F32) {
                            float acc = -0.0f;
                            for (unsigned k = 0; k < a.lanes; ++k) acc += std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k)));
                            operands.emplace_back(acc);
                        } else {
                            std::uint64_t acc = 0;
                            for (unsigned k = 0; k < a.lanes; ++k) acc += a.lane(k);
                            operands.push_back(decodeSimdLane(a.elemType, acc));
                        }
                    } else if (fnName == "simdReduceMin" || fnName == "simdReduceMax") {
                        const bool isMin = (fnName == "simdReduceMin");
                        const auto a = std::get<SimdVal>(pop());
                        if (a.elemType == Type::Float) {
                            double acc = std::bit_cast<double>(a.lane(0));
                            for (unsigned k = 1; k < a.lanes; ++k) {
                                const double d = std::bit_cast<double>(a.lane(k));
                                if (isMin ? (d < acc) : (d > acc)) acc = d;
                            }
                            operands.emplace_back(acc);
                        } else if (a.elemType == Type::F32) {
                            float acc = std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(0)));
                            for (unsigned k = 1; k < a.lanes; ++k) {
                                const float f = std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k)));
                                if (isMin ? (f < acc) : (f > acc)) acc = f;
                            }
                            operands.emplace_back(acc);
                        } else if (Program::isSignedInt(a.elemType)) {
                            const unsigned w = Program::intBitWidth(a.elemType);
                            std::uint64_t best = a.lane(0);
                            Integer bestS = signExtendWidth(best, w);
                            for (unsigned k = 1; k < a.lanes; ++k) {
                                const auto value = a.lane(k);
                                const Integer curS = signExtendWidth(value, w);
                                if (isMin ? (curS < bestS) : (curS > bestS)) { bestS = curS; best = value; }
                            }
                            operands.push_back(decodeSimdLane(a.elemType, best));
                        } else {
                            std::uint64_t best = a.lane(0);
                            for (unsigned k = 1; k < a.lanes; ++k) {
                                const auto value = a.lane(k);
                                if (isMin ? (value < best) : (value > best)) best = value;
                            }
                            operands.push_back(decodeSimdLane(a.elemType, best));
                        }
                    } else if (fnName == "simdCmpEq" || fnName == "simdCmpLt" || fnName == "simdCmpLe" ||
                               fnName == "simdCmpGt" || fnName == "simdCmpGe") {
                        const auto b = std::get<SimdVal>(pop());
                        const auto a = std::get<SimdVal>(pop());
                        const Type outElem = programRef.simdElem(i.expr->type);
                        const std::uint64_t allOnes = maskForWidth(Program::intBitWidth(outElem));
                        SimdVal out{outElem, a.lanes, {}};
                        for (unsigned k = 0; k < a.lanes; ++k) {
                            bool hit = false;
                            if (a.elemType == Type::Float) {
                                const double da = std::bit_cast<double>(a.lane(k));
                                const double db = std::bit_cast<double>(b.lane(k));
                                hit = (fnName == "simdCmpEq") ? (da == db)
                                    : (fnName == "simdCmpLt") ? (da < db)
                                    : (fnName == "simdCmpLe") ? (da <= db)
                                    : (fnName == "simdCmpGt") ? (da > db) : (da >= db);
                            } else if (a.elemType == Type::F32) {
                                const float fa = std::bit_cast<float>(static_cast<std::uint32_t>(a.lane(k)));
                                const float fb = std::bit_cast<float>(static_cast<std::uint32_t>(b.lane(k)));
                                hit = (fnName == "simdCmpEq") ? (fa == fb)
                                    : (fnName == "simdCmpLt") ? (fa < fb)
                                    : (fnName == "simdCmpLe") ? (fa <= fb)
                                    : (fnName == "simdCmpGt") ? (fa > fb) : (fa >= fb);
                            } else if (Program::isSignedInt(a.elemType)) {
                                const unsigned w = Program::intBitWidth(a.elemType);
                                const Integer sa = signExtendWidth(a.lane(k), w);
                                const Integer sb = signExtendWidth(b.lane(k), w);
                                hit = (fnName == "simdCmpEq") ? (sa == sb)
                                    : (fnName == "simdCmpLt") ? (sa < sb)
                                    : (fnName == "simdCmpLe") ? (sa <= sb)
                                    : (fnName == "simdCmpGt") ? (sa > sb) : (sa >= sb);
                            } else {
                                const std::uint64_t ua = a.lane(k);
                                const std::uint64_t ub = b.lane(k);
                                hit = (fnName == "simdCmpEq") ? (ua == ub)
                                    : (fnName == "simdCmpLt") ? (ua < ub)
                                    : (fnName == "simdCmpLe") ? (ua <= ub)
                                    : (fnName == "simdCmpGt") ? (ua > ub) : (ua >= ub);
                            }
                            out.setLane(k, hit ? allOnes : 0ULL);
                        }
                        operands.emplace_back(out);
                    } else if (fnName == "simdSelect") {
                        const auto b = std::get<SimdVal>(pop());
                        const auto a = std::get<SimdVal>(pop());
                        const auto mask = std::get<SimdVal>(pop());
                        SimdVal out{a.elemType, a.lanes, {}};
                        for (unsigned k = 0; k < a.lanes; ++k) {
                            out.setLane(k, (mask.lane(k) != 0) ? a.lane(k) : b.lane(k));
                        }
                        operands.emplace_back(out);
                    } else if (fnName == "clz" || fnName == "ctz" || fnName == "popcnt" || fnName == "bswap") {
                        const Value v = pop();
                        const Type vt = i.expr->args[0].type;
                        operands.push_back(evalBitIntrinsicVM(vt, fnName, encodeSimdLane(vt, v)));
                    } else if (fnName == "rotl" || fnName == "rotr") {
                        const Value sv = pop();
                        const Value xv = pop();
                        const Type vt = i.expr->args[0].type;
                        operands.push_back(evalBitIntrinsicVM(vt, fnName, encodeSimdLane(vt, xv), encodeSimdLane(vt, sv)));
                    } else if (fnName == "prefetchRead" || fnName == "prefetchWrite") {
                        (void)pop();
                        operands.emplace_back(Value{});
                    } else if (fnName == "spinHint") {
                        operands.emplace_back(Value{});
                    } else if (fnName == "atomicNew") {
                        const Value v = pop();
                        const Type et = programRef.atomicElem(i.expr->type);
                        std::uint64_t initBits = 0;
                        if (et == Type::Bool) initBits = std::get<bool>(v) ? 1ULL : 0ULL;
                        else if (et == Type::Int) initBits = static_cast<std::uint64_t>(std::get<Integer>(v));
                        else initBits = std::get<ExactInt>(v).bits & maskForType(et);
                        try {
                            operands.emplace_back(std::make_shared<AtomicObject>(et, initBits));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "atomicLoad" || fnName == "atomicLoadOrd") {
                        std::memory_order mo = std::memory_order_seq_cst;
                        if (fnName == "atomicLoadOrd") {
                            const auto ordStr = std::get<std::string_view>(pop());
                            if (ordStr == "relaxed") mo = std::memory_order_relaxed;
                            else if (ordStr == "acquire") mo = std::memory_order_acquire;
                        }
                        const auto ap = std::get<AtomicPtr>(pop());
                        const std::uint64_t bits = ap->bits.load(mo);
                        operands.push_back(ap->elemType == Type::Bool ? Value{(bits & 1ULL) != 0} : makeIntValue(ap->elemType, bits));
                    } else if (fnName == "atomicStore" || fnName == "atomicStoreOrd") {
                        std::memory_order mo = std::memory_order_seq_cst;
                        if (fnName == "atomicStoreOrd") {
                            const auto ordStr = std::get<std::string_view>(pop());
                            if (ordStr == "relaxed") mo = std::memory_order_relaxed;
                            else if (ordStr == "release") mo = std::memory_order_release;
                        }
                        const Value v = pop();
                        const auto ap = std::get<AtomicPtr>(pop());
                        const std::uint64_t bits = (ap->elemType == Type::Bool)
                            ? (std::get<bool>(v) ? 1ULL : 0ULL)
                            : ((ap->elemType == Type::Int) ? static_cast<std::uint64_t>(std::get<Integer>(v))
                                                           : (std::get<ExactInt>(v).bits & maskForType(ap->elemType)));
                        ap->bits.store(bits, mo);
                        operands.emplace_back(Value{});
                    } else if (fnName == "atomicSwap" || fnName == "atomicSwapOrd") {
                        std::memory_order mo = std::memory_order_seq_cst;
                        if (fnName == "atomicSwapOrd") {
                            const auto ordStr = std::get<std::string_view>(pop());
                            if (ordStr == "relaxed") mo = std::memory_order_relaxed;
                            else if (ordStr == "acquire") mo = std::memory_order_acquire;
                            else if (ordStr == "release") mo = std::memory_order_release;
                            else if (ordStr == "acqRel") mo = std::memory_order_acq_rel;
                        }
                        const Value v = pop();
                        const auto ap = std::get<AtomicPtr>(pop());
                        const std::uint64_t bits = (ap->elemType == Type::Bool)
                            ? (std::get<bool>(v) ? 1ULL : 0ULL)
                            : ((ap->elemType == Type::Int) ? static_cast<std::uint64_t>(std::get<Integer>(v))
                                                           : (std::get<ExactInt>(v).bits & maskForType(ap->elemType)));
                        const std::uint64_t old = ap->bits.exchange(bits, mo);
                        operands.push_back(ap->elemType == Type::Bool ? Value{(old & 1ULL) != 0} : makeIntValue(ap->elemType, old));
                    } else if (fnName == "atomicCas" || fnName == "atomicCasOrd") {
                        std::memory_order mo = std::memory_order_seq_cst;
                        if (fnName == "atomicCasOrd") {
                            const auto ordStr = std::get<std::string_view>(pop());
                            if (ordStr == "relaxed") mo = std::memory_order_relaxed;
                            else if (ordStr == "acquire") mo = std::memory_order_acquire;
                            else if (ordStr == "release") mo = std::memory_order_release;
                            else if (ordStr == "acqRel") mo = std::memory_order_acq_rel;
                        }
                        const Value dv = pop();
                        const Value ev = pop();
                        const auto ap = std::get<AtomicPtr>(pop());
                        auto enc = [&](const Value& x) -> std::uint64_t {
                            if (ap->elemType == Type::Bool) return std::get<bool>(x) ? 1ULL : 0ULL;
                            if (ap->elemType == Type::Int) return static_cast<std::uint64_t>(std::get<Integer>(x));
                            return std::get<ExactInt>(x).bits & maskForType(ap->elemType);
                        };
                        std::uint64_t expBits = enc(ev);
                        const std::uint64_t desBits = enc(dv);
                        const std::memory_order failMo = (mo == std::memory_order_release || mo == std::memory_order_relaxed)
                            ? std::memory_order_relaxed
                            : ((mo == std::memory_order_seq_cst) ? std::memory_order_seq_cst : std::memory_order_acquire);
                        const bool ok = ap->bits.compare_exchange_strong(expBits, desBits, mo, failMo);
                        operands.emplace_back(ok);
                    } else if (fnName.starts_with("atomicAdd") || fnName.starts_with("atomicSub") ||
                               fnName.starts_with("atomicAnd") || fnName.starts_with("atomicOr") ||
                               fnName.starts_with("atomicXor")) {
                        std::memory_order mo = std::memory_order_seq_cst;
                        if (fnName.ends_with("Ord")) {
                            const auto ordStr = std::get<std::string_view>(pop());
                            if (ordStr == "relaxed") mo = std::memory_order_relaxed;
                            else if (ordStr == "acquire") mo = std::memory_order_acquire;
                            else if (ordStr == "release") mo = std::memory_order_release;
                            else if (ordStr == "acqRel") mo = std::memory_order_acq_rel;
                        }
                        const Value dv = pop();
                        const auto ap = std::get<AtomicPtr>(pop());
                        const std::uint64_t m = maskForType(ap->elemType);
                        const std::uint64_t deltaBits = (ap->elemType == Type::Int)
                            ? static_cast<std::uint64_t>(std::get<Integer>(dv))
                            : (std::get<ExactInt>(dv).bits & m);
                        std::uint64_t cur = ap->bits.load(std::memory_order_relaxed);
                        for (;;) {
                            std::uint64_t next = cur;
                            if (fnName.starts_with("atomicAdd")) next = (cur + deltaBits) & m;
                            else if (fnName.starts_with("atomicSub")) next = (cur - deltaBits) & m;
                            else if (fnName.starts_with("atomicAnd")) next = (cur & deltaBits) & m;
                            else if (fnName.starts_with("atomicOr")) next = (cur | deltaBits) & m;
                            else next = (cur ^ deltaBits) & m;
                            if (ap->bits.compare_exchange_weak(cur, next, mo, std::memory_order_relaxed)) break;
                        }
                        operands.push_back(makeIntValue(ap->elemType, cur & m));
                    } else if (fnName == "atomicFence") {
                        const auto ordStr = std::get<std::string_view>(pop());
                        std::memory_order mo = std::memory_order_seq_cst;
                        if (ordStr == "acquire") mo = std::memory_order_acquire;
                        else if (ordStr == "release") mo = std::memory_order_release;
                        else if (ordStr == "acqRel") mo = std::memory_order_acq_rel;
                        std::atomic_thread_fence(mo);
                        operands.emplace_back(Value{});
                    } else if (fnName == "ringNew") {
                        const Integer cap = std::get<Integer>(pop());
                        if (cap <= 0 || cap > maxRingCapacity)
                            fail("R_ARRAY_SIZE", "Capacidad fuera de rango en ringNew", i.span, "let q = ringNew<int>(16)");
                        std::size_t realCap = 2u;
                        while (realCap < static_cast<std::size_t>(cap)) realCap <<= 1u;
                        const Type et = programRef.ringQueueElem(i.expr->type);
                        try {
                            operands.emplace_back(std::make_shared<RingQueueObject>(et, realCap));
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    } else if (fnName == "ringPush") {
                        const Value item = pop();
                        const auto rq = std::get<RingQueuePtr>(pop());
                        bool ok = false;
                        {
                            std::lock_guard<std::mutex> lk(rq->mu);
                            if (rq->items.size() < rq->cap) {
                                rq->items.push_back(cloneValue(item));
                                ok = true;
                            }
                        }
                        operands.emplace_back(ok);
                    } else if (fnName == "ringPop") {
                        const auto rq = std::get<RingQueuePtr>(pop());
                        std::optional<Value> got;
                        {
                            std::lock_guard<std::mutex> lk(rq->mu);
                            if (!rq->items.empty()) {
                                got = std::move(rq->items.front());
                                rq->items.pop_front();
                            }
                        }
                        operands.push_back(makeOptionValue(i.expr->type, std::move(got)));
                    } else if (fnName == "ringLen") {
                        const auto rq = std::get<RingQueuePtr>(pop());
                        std::lock_guard<std::mutex> lk(rq->mu);
                        operands.emplace_back(static_cast<Integer>(rq->items.size()));
                    } else if (fnName == "ringCap") {
                        const auto rq = std::get<RingQueuePtr>(pop());
                        operands.emplace_back(static_cast<Integer>(rq->cap));
                    } else if (fnName == "regionNew") {
                        const Integer initBytes = std::get<Integer>(pop());
                        if (initBytes < 0 || initBytes > (1LL << 30))
                            fail("R_ARRAY_SIZE", "Capacidad inicial fuera de rango en regionNew", i.span, "var r = regionNew(4096)");
                        auto rg = std::make_shared<RegionObject>();
                        rg->initCap = static_cast<std::size_t>(initBytes);
                        if (initBytes > 0) {
                            const std::size_t cap = static_cast<std::size_t>(initBytes);
                            std::uint8_t* mem = RegionObject::allocAlignedMem(cap, 64u);
                            if (!mem) allocationFailed(i.span);
                            rg->chunks.push_back(RegionChunk{mem, cap, 0u, 64u});
                            rg->totalCap = cap;
                        }
                        operands.emplace_back(std::move(rg));
                    } else if (fnName == "regionAlloc" || fnName == "regionAllocAligned") {
                        Integer align = 1;
                        if (fnName == "regionAllocAligned") align = std::get<Integer>(pop());
                        const Integer count = std::get<Integer>(pop());
                        if (count < 0)
                            fail("R_ARRAY_SIZE", "Cantidad negativa en " + fnName, i.span, "regionAlloc<u8>(mut r, 64)");
                        if (fnName == "regionAllocAligned" && (align <= 0 || (align & (align - 1)) != 0))
                            fail("R_ARRAY_SIZE", "Alineación inválida en regionAllocAligned (requiere potencia de 2 positiva)", i.span, "regionAllocAligned<f32>(mut r, 8, 32)");
                        const Type elemT = i.expr->typeArgs.at(0);
                        const auto [elemSz, elemAl] = layoutCalc.sizeAlignOf(elemT);
                        const std::size_t effAlign = fnName == "regionAllocAligned"
                            ? static_cast<std::size_t>(align)
                            : std::max<std::size_t>(1u, elemAl);
                        const std::uint64_t ucnt = static_cast<std::uint64_t>(count);
                        if (ucnt > 0 && static_cast<std::uint64_t>(elemSz) > std::numeric_limits<std::size_t>::max() / ucnt)
                            fail("R_ARRAY_SIZE", "Desbordamiento de tamaño en " + fnName, i.span, "regionAlloc<u8>(mut r, 64)");
                        const std::size_t totalBytes = static_cast<std::size_t>(ucnt * elemSz);
                        std::uint8_t* ptr = nullptr;
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& rg = std::get<RegionPtr>(target);
                            if (!rg) allocationFailed(i.span);
                            ptr = rg->alloc(totalBytes, effAlign);
                        });
                        if (!ptr) allocationFailed(i.span);
                        operands.emplace_back(RawPtrVal{elemT, reinterpret_cast<std::uintptr_t>(ptr)});
                    } else if (fnName == "regionReset") {
                        mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                            auto& rg = std::get<RegionPtr>(target);
                            if (rg) rg->reset();
                        });
                        operands.emplace_back(Value{});
                    } else if (fnName == "regionUsed") {
                        const auto rg = std::get<RegionPtr>(pop());
                        operands.emplace_back(static_cast<Integer>(rg ? rg->totalUsed : 0u));
                    } else if (fnName == "regionCapacity") {
                        const auto rg = std::get<RegionPtr>(pop());
                        operands.emplace_back(static_cast<Integer>(rg ? rg->totalCap : 0u));
                    } else {
                        fail("E_FUNCTION", "Función en tiempo de ejecución desconocida: " + fnName, i.span, "");
                    }
                    break;
                }
                case Op::SharedUpdateEnd: {
                    auto newPayload = pop();
                    mutateTarget(frame, i.expr->args[0], [&](Value& target) {
                        auto& sh = std::get<SharedPtr>(target);
                        if (sh.use_count() > 1) {
                            try { sh = std::make_shared<SharedObject>(SharedObject{std::move(newPayload)}); }
                            catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        } else {
                            sh->payload = std::move(newPayload);
                        }
                    });
                    operands.emplace_back(Value{});
                    break;
                }
                case Op::BoxDyn: {
                    auto innerVal = pop();
                    try {
                        operands.emplace_back(std::make_shared<DynObject>(
                            DynObject{i.expr->args[0].type, i.expr->assocType, std::move(innerVal)}));
                    } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    break;
                }
                case Op::DynCall: {
                    if (i.expr->args[0].isMutArg) {
                        activateDynMut(*i.expr, i.span, frame);
                    } else {
                        const auto base = operands.size() - i.expr->args.size();
                        const DynPtr dp = std::get<DynPtr>(operands[base]);
                        operands[base] = dp->inner;
                        const auto& fnName = dynDispatch.at({i.expr->assocType, dp->concreteType}).at(i.expr->fieldIndices[0]);
                        activate(*functions.at(fnName), i.span, i.expr);
                    }
                    break;
                }
                case Op::MakeClosure: {
                    const auto& cl = programRef.closures.at(i.expr->closure->id);
                    Value envVal;
                    if (cl.envStruct != Type::Void) {
                        const std::size_t nCaps = i.expr->args.size();
                        std::vector<Value> fields(nCaps);
                        for (std::size_t k = nCaps; k != 0; --k) fields[k - 1] = pop();
                        try {
                            envVal = std::make_shared<Aggregate>(
                                Aggregate{storeString(programRef.nameOf(cl.envStruct)), std::move(fields)});
                        } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    }
                    try {
                        operands.emplace_back(std::make_shared<const ClosureObject>(
                            ClosureObject{cl.id, std::move(envVal)}));
                    } catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    break;
                }
                case Op::ClosureCall: {
                    const auto base = operands.size() - i.expr->args.size();
                    const ClosurePtr cp = std::get<ClosurePtr>(operands[base]);
                    const auto& cl = programRef.closures.at(cp->id);
                    if (cl.envStruct != Type::Void) {
                        operands[base] = cp->env;
                    } else {
                        operands.erase(operands.begin() + static_cast<std::ptrdiff_t>(base));
                    }
                    activate(*functions.at(cl.fnName), i.span, nullptr);
                    break;
                }
                case Op::Try: {
                    auto ag = std::get<AggregatePtr>(pop());
                    const auto dot = ag->tag.rfind('.');
                    const auto var = dot == std::string_view::npos ? ag->tag : ag->tag.substr(dot + 1);
                    if (var == "Some" || var == "Ok") {
                        if (ag.use_count() == 1) {
                            operands.push_back(std::move(ag->fields.at(0)));
                            ag->fields[0] = Value{};
                        } else {
                            operands.push_back(ag->fields.at(0));
                        }
                    } else if (frame.fn && frame.fn->isTest) {
                        fail("R_ASSERT", std::string("El operador ? recibió ") + std::string(var) + " en test", i.span, "assert isOk(x)");
                    } else {
                        auto outAg = std::make_shared<Aggregate>();
                        outAg->tag = storeString(programRef.nameOf(frame.fn->result) + "." + std::string(var));
                        if (var == "Err") outAg->fields.push_back(ag->fields.at(0));
                        applyWriteBacks(frame);
                        unwindFrame(frame);
                        operands.resize(frame.base);
                        frames.pop_back();
                        if (!frames.empty()) operands.emplace_back(std::move(outAg));
                    }
                    break;
                }
                case Op::Index: {
                    auto index = std::get<Integer>(pop());
                    auto base = pop();
                    if (const auto* sv = std::get_if<std::string_view>(&base)) {
                        if (index < 0 || static_cast<std::uint64_t>(index) >= sv->size())
                            fail("R_BOUNDS", "Índice de texto fuera de rango", i.span, R"("abc"[0])");
                        operands.emplace_back(static_cast<Integer>(static_cast<unsigned char>((*sv)[static_cast<std::size_t>(index)])));
                    } else {
                        auto array = std::get<ArrayPtr>(base);
                        operands.push_back(array->items[arrayIndex(*array, index, i.span)]);
                    }
                    break;
                }
                case Op::IndexSet: {
                    auto element = pop();
                    auto index = std::get<Integer>(pop());
                    auto& array = std::get<ArrayPtr>(frame.locals.find(i.stmt->name)->target());
                    auto position = arrayIndex(*array, index, i.span);
                    // The independent reference VM preserves value semantics using copy-on-write.
                    if (array.use_count() > 1) {
                        try { array = std::make_shared<ArrayBuffer>(*array); }
                        catch (const std::bad_alloc&) { allocationFailed(i.span); }
                    }
                    dropValue(array->items[position]);
                    array->items[position] = std::move(element);
                    break;
                }
                case Op::FieldSet: {
                    auto element = pop();
                    std::optional<Integer> indexVal;
                    if (i.stmt->exprs.size() == 2) indexVal = std::get<Integer>(pop());
                    Value& rootVal = frame.locals.find(i.stmt->name)->target();
                    Value cur = rootVal;
                    std::vector<std::pair<std::shared_ptr<Aggregate>, std::size_t>> chain;
                    for (std::size_t fIdx : i.stmt->fieldIndices) {
                        const auto& old = std::get<AggregatePtr>(cur);
                        auto copy = std::make_shared<Aggregate>(*old);
                        cur = copy->fields[fIdx];
                        chain.emplace_back(std::move(copy), fIdx);
                    }
                    if (indexVal) {
                        auto& array = std::get<ArrayPtr>(cur);
                        const auto pos = arrayIndex(*array, *indexVal, i.span);
                        if (array.use_count() > 1) {
                            try { array = std::make_shared<ArrayBuffer>(*array); }
                            catch (const std::bad_alloc&) { allocationFailed(i.span); }
                        }
                        array->items[pos] = std::move(element);
                    } else {
                        cur = std::move(element);
                    }
                    Value updated = std::move(cur);
                    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                        it->first->fields[it->second] = std::move(updated);
                        updated = std::move(it->first);
                    }
                    rootVal = std::move(updated);
                    break;
                }
                case Op::PlaceSet: {
                    auto element = pop();
                    std::size_t idxCount = 0;
                    for (const auto& st : i.stmt->placeSteps)
                        if (st.isIndex) ++idxCount;
                    std::vector<Integer> idxVals(idxCount);
                    for (std::size_t k = idxCount; k != 0; --k)
                        idxVals[k - 1] = std::get<Integer>(pop());
                    std::vector<PlaceStepVM> steps;
                    steps.reserve(i.stmt->placeSteps.size());
                    std::size_t pos = 0;
                    for (const auto& sstep : i.stmt->placeSteps) {
                        if (sstep.isIndex) steps.push_back({true, 0, idxVals[pos++]});
                        else steps.push_back({false, sstep.fieldIdx, 0});
                    }
                    Value& rootVal = frame.locals.find(i.stmt->name)->target();
                    writeBackPlace(rootVal, steps, std::move(element), i.span);
                    break;
                }
                case Op::SavePlaceIdx:
                    frame.placeIdxStack.push_back(std::get<Integer>(pop()));
                    break;
                case Op::ReadPlace: {
                    const std::size_t cnt = countPlaceIndices(*i.expr);
                    const auto idxs = peekPlaceIndices(frame, cnt);
                    auto [rootName, steps] = buildPlaceSteps(*i.expr, idxs);
                    const Value& root = frame.locals.find(std::string(rootName))->target();
                    operands.push_back(readPlace(root, steps, i.span));
                    break;
                }
                case Op::DropOperands: {
                    for (std::size_t n = 0; n < i.target; ++n) {
                        auto v = pop();
                        dropValue(v);
                    }
                    break;
                }
                case Op::Unary:
                    if (i.expr->text == "!") operands.emplace_back(!std::get<bool>(pop()));
                    else if (i.expr->text == "~") {
                        auto v = pop();
                        if (auto* sv = std::get_if<SimdVal>(&v)) {
                            for (unsigned k = 0; k < sv->lanes; ++k) sv->setLane(k, ~sv->lane(k));
                            operands.emplace_back(*sv);
                        } else if (auto* ei = std::get_if<ExactInt>(&v))
                            operands.push_back(makeIntValue(ei->type, ~ei->bits));
                        else
                            operands.emplace_back(~std::get<Integer>(v));
                    } else {
                        auto v = pop();
                        if (auto* sv = std::get_if<SimdVal>(&v)) {
                            if (sv->elemType == Type::Float) {
                                for (unsigned k = 0; k < sv->lanes; ++k)
                                    sv->setLane(k, std::bit_cast<std::uint64_t>(-std::bit_cast<double>(sv->lane(k))));
                            } else if (sv->elemType == Type::F32) {
                                for (unsigned k = 0; k < sv->lanes; ++k)
                                    sv->setLane(k, std::bit_cast<std::uint32_t>(-std::bit_cast<float>(static_cast<std::uint32_t>(sv->lane(k)))));
                            } else {
                                for (unsigned k = 0; k < sv->lanes; ++k)
                                    sv->setLane(k, 0ULL - sv->lane(k));
                            }
                            operands.emplace_back(*sv);
                        } else if (auto* d = std::get_if<double>(&v)) operands.emplace_back(-(*d));
                        else if (auto* f = std::get_if<float>(&v)) operands.emplace_back(-(*f));
                        else if (auto* ei = std::get_if<ExactInt>(&v)) {
                            const unsigned w = Program::intBitWidth(ei->type);
                            const Integer a = signExtendWidth(ei->bits, w);
                            if (a == minSignedForWidth(w)) overflow(i.span);
                            operands.push_back(makeIntValue(ei->type, static_cast<std::uint64_t>(-a)));
                        } else {
                            const auto value = std::get<Integer>(v);
                            if (value == minInt) overflow(i.span);
                            operands.emplace_back(-value);
                        }
                    }
                    break;
                case Op::Binary: {
                    const auto right = pop();
                    const auto left = pop();
                    if (i.expr->text == "+") {
                        if (const auto* ls = std::get_if<std::string_view>(&left)) {
                            const auto rs = std::get<std::string_view>(right);
                            if (rs.size() > static_cast<std::size_t>(maxStringLength) - ls->size())
                                allocationFailed(i.span);
                            std::string outStr;
                            outStr.reserve(ls->size() + rs.size());
                            outStr.append(*ls);
                            outStr.append(rs);
                            operands.emplace_back(storeString(std::move(outStr)));
                            break;
                        }
                    }
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
                    if (i.arm->variant == "_") {
                        auto discarded = pop();
                        dropValue(discarded);
                        pushScope(frame);
                        break;
                    }
                    auto data = std::get<AggregatePtr>(operands.back());
                    if (data->tag != i.arm->variant) {
                        frame.ip = i.target;
                        break;
                    }
                    pop();
                    pushScope(frame);
                    auto& bindings = frame.locals.levels.back();
                    auto& order = frame.declOrder.back();
                    const bool canMove = (data.use_count() == 1);
                    for (std::size_t n = 0; n < i.arm->names.size(); ++n) {
                        if (i.arm->names[n] == "_") {
                            if (canMove) {
                                Value tmp = std::move(data->fields[n]);
                                data->fields[n] = Value{};
                                dropValue(tmp);
                            }
                            continue;
                        }
                        if (canMove) {
                            bindings.emplace(i.arm->names[n], LocalSlot{std::move(data->fields[n]), nullptr, true});
                            data->fields[n] = Value{};
                        } else {
                            bindings.emplace(i.arm->names[n], LocalSlot{data->fields[n], nullptr, true});
                        }
                        order.push_back(i.arm->names[n]);
                    }
                    break;
                }
                case Op::Bind: {
                    auto val = pop();
                    if (i.stmt->name == "_") {
                        dropValue(val);
                    } else {
                        frame.locals.levels.back().emplace(i.stmt->name, LocalSlot{std::move(val), nullptr, true});
                        frame.declOrder.back().push_back(i.stmt->name);
                    }
                    break;
                }
                case Op::Write: {
                    auto rhs = pop();
                    Value& target = frame.locals.find(i.stmt->name)->target();
                    dropValue(target);
                    target = std::move(rhs);
                    break;
                }
                case Op::Discard: {
                    auto discarded = pop();
                    dropValue(discarded);
                    break;
                }
                case Op::Print: print(pop()); break;
                case Op::Assert:
                    if (!std::get<bool>(pop())) fail("R_ASSERT", "Falló la aserción", i.span, "assert 1 == 1");
                    break;
                case Op::ContractCheck:
                    if (!std::get<bool>(pop()))
                        fail("R_CONTRACT", "Violación de contrato (requires/ensures)", i.span, "requires x > 0");
                    break;
                case Op::BeginEnsures: {
                    Value retVal = i.target ? pop() : Value{};
                    frame.ranges.clear();
                    frame.forEachs.clear();
                    while (frame.locals.levels.size() > 1) popScope(frame);
                    pushScope(frame);
                    if (i.target) {
                        frame.locals.levels.back().emplace("result", LocalSlot{std::move(retVal), nullptr, false});
                        frame.declOrder.back().push_back("result");
                    }
                    break;
                }
                case Op::EndEnsuresReturn: {
                    Value result{};
                    if (i.target) {
                        auto& rSlot = frame.locals.levels.back().at("result");
                        result = std::move(rSlot.value);
                        rSlot.value = Value{};
                    }
                    applyWriteBacks(frame);
                    unwindFrame(frame);
                    operands.resize(frame.base);
                    frames.pop_back();
                    if (!frames.empty()) operands.push_back(std::move(result));
                    break;
                }
                case Op::IfFalse: if (!std::get<bool>(pop())) frame.ip = i.target; break;
                case Op::Jump: frame.ip = i.target; break;
                case Op::Enter: pushScope(frame); break;
                case Op::Leave: popScope(frame); break;
                case Op::RangeEnter: {
                    const auto end = std::get<Integer>(pop());
                    const auto start = std::get<Integer>(pop());
                    if (start >= end) frame.ip = i.target;
                    else frame.ranges.push_back({start, end});
                    break;
                }
                case Op::RangeBind:
                    pushScope(frame);
                    if (i.stmt->name != "_") {
                        frame.locals.levels.back().emplace(i.stmt->name, LocalSlot{Value{frame.ranges.back().cursor}, nullptr, false});
                        frame.declOrder.back().push_back(i.stmt->name);
                    }
                    break;
                case Op::RangeNext: {
                    popScope(frame);
                    auto& range = frame.ranges.back();
                    // cursor < end <= INT64_MAX, so increment is always representable.
                    if (++range.cursor < range.end) frame.ip = i.target;
                    else frame.ranges.pop_back();
                    break;
                }
                case Op::RangePop:
                    frame.ranges.pop_back();
                    break;
                case Op::ForEachEnter: {
                    auto arr = std::get<ArrayPtr>(pop());
                    if (arr->items.empty()) frame.ip = i.target;
                    else {
                        const auto n = arr->items.size();
                        frame.forEachs.push_back({std::move(arr), 0, n});
                    }
                    break;
                }
                case Op::ForEachBind: {
                    pushScope(frame);
                    auto& fe = frame.forEachs.back();
                    if (i.stmt->name != "_") {
                        frame.locals.levels.back().emplace(i.stmt->name, LocalSlot{fe.array->items[fe.cursor], nullptr, false});
                        frame.declOrder.back().push_back(i.stmt->name);
                    }
                    break;
                }
                case Op::ForEachNext: {
                    popScope(frame);
                    auto& fe = frame.forEachs.back();
                    if (++fe.cursor < fe.end) frame.ip = i.target;
                    else frame.forEachs.pop_back();
                    break;
                }
                case Op::ForEachPop:
                    frame.forEachs.pop_back();
                    break;
                case Op::Return: {
                    const auto result = i.stmt && !i.stmt->exprs.empty() ? pop() : Value{};
                    applyWriteBacks(frame);
                    unwindFrame(frame);
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
        if (fn.isTest && (program.rootModules.empty() ||
            std::find(program.rootModules.begin(), program.rootModules.end(), fn.module) != program.rootModules.end()))
            interpreter.execute(fn);
    }
}

void runMain(const Program& program, std::ostream& output, Limits limits, std::istream* input) {
    Interpreter interpreter(program, output, limits);
    interpreter.input = input;
    const auto main = interpreter.functions.find(program.entryMain);
    if (main == interpreter.functions.end()) fail("E_MAIN", "Falta la función main", {}, "fn main() {}");
    interpreter.execute(*main->second);
}

} // namespace veloz
