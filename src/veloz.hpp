#pragma once
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace veloz {
// UTF-8 byte offsets, half-open. CLI derives one-based line/column.
struct Span { std::size_t start = 0, end = 0; };
struct Edit { Span span; std::string replacement; };
struct Diagnostic : std::exception {
    std::string id, message;
    Span span;
    std::string expected, actual;
    std::vector<Span> related;
    std::optional<Edit> fix;
    std::string example;
    Diagnostic(std::string code, std::string description, Span at)
        : id(std::move(code)), message(std::move(description)), span(at) {}
    const char* what() const noexcept override { return message.c_str(); }
};
enum class Type { Int, Bool, Str, Void, Unknown, NamedBase = 16 };
inline std::string typeName(Type t) {
    switch (t) {
        case Type::Int: return "int"; case Type::Bool: return "bool";
        case Type::Str: return "str"; case Type::Void: return "void";
        default: return "?";
    }
}
enum class ExprKind { Integer, Boolean, String, Name, Call, Unary, Binary, Construct, Field };
struct Expr {
    ExprKind kind; Span span;
    // Decoded string, identifier, literal spelling or operator.
    std::string text;
    std::vector<Expr> args;
    Type type = Type::Unknown; // annotated by check
    // Construct: text = "Point" or "Search.Found"; struct arguments have field labels.
    // Field: text = field name, args[0] = base.
    std::vector<std::string> labels{};
    std::vector<std::size_t> fieldIndices{}; // source arguments -> declared field indices
};
enum class StmtKind { Let, Var, Assign, Call, If, While, For, Return, Assert, Print, Match };
struct MatchArm;
struct Stmt {
    StmtKind kind; Span span;
    std::string name; // binding/assignment target or for iterator
    Type annotation = Type::Unknown; // optional local annotation
    std::vector<Expr> exprs; // For: lower, upper; Return: zero or one
    std::vector<Stmt> body, otherwise;
    std::vector<MatchArm> arms;
};
struct MatchArm {
    std::string variant; // "Search.Found"
    Span span;
    std::vector<std::string> names; // positional field bindings
    std::vector<Stmt> body;
};
struct Param { std::string name; Type type; Span span; };
struct Function {
    std::string name; Span span; std::vector<Param> params;
    Type result = Type::Void; std::vector<Stmt> body;
    bool isTest = false; // name is the decoded test title
    std::vector<Type> generics; // named type IDs scoped to this function
};
struct Variant { std::string name; Span span; std::vector<Param> fields; };
struct DataDecl {
    Type type; bool isEnum; Span span;
    std::vector<Variant> variants; // struct has one variant named after itself
};
struct Program {
    std::vector<Function> functions;
    std::vector<std::string> typeNames;
    std::vector<DataDecl> data;
    std::string source;
    Type intern(std::string_view name) {
        std::size_t index = 0;
        for (; index < typeNames.size(); ++index) if (typeNames[index] == name) break;
        if (index == typeNames.size()) typeNames.emplace_back(name);
        return static_cast<Type>(static_cast<int>(Type::NamedBase) + index);
    }
    std::string nameOf(Type type) const {
        if (type < Type::NamedBase) return typeName(type);
        return typeNames.at(static_cast<std::size_t>(type) - static_cast<std::size_t>(Type::NamedBase));
    }
    const DataDecl* declaration(Type type) const {
        for (const auto& d : data) if (d.type == type) return &d;
        return nullptr;
    }
};
Program parse(std::string_view source);
void check(Program& program); // throws Diagnostic; populates Expr::type
struct Limits { std::uint64_t steps = 10000000; unsigned depth = 256; };
// main() has zero parameters and returns void; optional for check.
void runMain(const Program& program, std::ostream& output, Limits limits = {});
void runTests(const Program& program, std::ostream& output, Limits limits = {});
std::string emitLLVM(const Program& program); // whole checked program
}
