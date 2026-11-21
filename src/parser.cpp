#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace veloz::syntax {

bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool upper(char c) { return c >= 'A' && c <= 'Z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool wordChar(char c) { return lower(c) || upper(c) || digit(c) || c == '_'; }

template<class List>
bool contains(const List& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

void validateUtf8(std::string_view input) {
    for (std::size_t i = 0; i < input.size();) {
        const auto byte = static_cast<unsigned char>(input[i]);
        if (byte < 0x80) { ++i; continue; }
        const unsigned length = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : 2;
        const auto next = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        bool valid = byte >= 0xc2 && byte <= 0xf4 && length <= input.size() - i;
        valid = valid && !(byte == 0xe0 && next < 0xa0) && !(byte == 0xed && next >= 0xa0) &&
                !(byte == 0xf0 && next < 0x90) && !(byte == 0xf4 && next >= 0x90);
        for (unsigned k = 1; valid && k < length; ++k)
            valid = (static_cast<unsigned char>(input[i + k]) & 0xc0) == 0x80;
        if (!valid) throw Diagnostic("E_UTF8", "La fuente debe contener UTF-8 válido.", {i, i + 1});
        i += length;
    }
}

std::vector<Token> lex(std::string_view source, bool keepComments) {
    validateUtf8(source);
    std::vector<Token> tokens;
    std::size_t cursor = source.starts_with("\xef\xbb\xbf") ? 3 : 0;
    while (cursor < source.size()) {
        const auto start = cursor;
        const char c = source[cursor++];
        if (c == ' ' || c == '\t') continue;
        if (c == '/' && cursor < source.size() && source[cursor] == '/') {
            while (cursor < source.size() && source[cursor] != '\n' && source[cursor] != '\r') ++cursor;
            if (keepComments)
                tokens.push_back({TokenKind::Comment, {start, cursor}, std::string(source.substr(start, cursor - start))});
            continue;
        }
        if (c == '\n' || (c == '\r' && cursor < source.size() && source[cursor] == '\n')) {
            if (c == '\r') ++cursor;
            tokens.push_back({TokenKind::Newline, {start, cursor}, "\n"});
            continue;
        }
        TokenKind kind = TokenKind::Symbol;
        if (lower(c) || upper(c) || c == '_') {
            kind = TokenKind::Word;
            while (cursor < source.size() && wordChar(source[cursor])) ++cursor;
        } else if (digit(c)) {
            kind = TokenKind::Digits;
            while (cursor < source.size() && digit(source[cursor])) ++cursor;
        } else if (c == '"') {
            std::string value;
            while (cursor < source.size() && source[cursor] != '"') {
                char ch = source[cursor++];
                if (static_cast<unsigned char>(ch) < 0x20)
                    throw Diagnostic("E_STRING", "Carácter de control dentro del texto.", {cursor - 1, cursor});
                if (ch == '\\') {
                    if (cursor == source.size()) break;
                    ch = source[cursor++];
                    switch (ch) {
                        case 'n': ch = '\n'; break;
                        case 'r': ch = '\r'; break;
                        case 't': ch = '\t'; break;
                        case '"': case '\\': break;
                        default: throw Diagnostic("E_ESCAPE", "Escape de texto desconocido.", {cursor - 2, cursor});
                    }
                }
                value += ch;
            }
            if (cursor == source.size())
                throw Diagnostic("E_STRING", "Falta cerrar el texto con comillas.", {start, cursor});
            tokens.push_back({TokenKind::Quoted, {start, ++cursor}, std::move(value)});
            continue;
        } else {
            constexpr std::array two = {"->", "=>", "..", "==", "!=", "<=", ">=", "&&", "||",
                                        "+=", "-=", "*=", "/=", "%="};
            if (contains(two, source.substr(start, 2))) ++cursor;
            else if (std::string_view("{}[](),:!+-*/%<>=.;").find(c) == std::string_view::npos)
                throw Diagnostic("E_CHAR", "Carácter inesperado.", {start, cursor});
        }
        tokens.push_back({kind, {start, cursor}, std::string(source.substr(start, cursor - start))});
    }
    tokens.push_back({TokenKind::End, {source.size(), source.size()}, {}});
    return tokens;
}
} // namespace veloz::syntax

namespace veloz {
namespace {
using namespace syntax;

constexpr unsigned maxDepth = 96;

void guardDepth(unsigned depth, Span span) {
    if (depth > maxDepth)
        throw Diagnostic("E_DEPTH", "Se excedieron los 96 niveles de anidamiento.", span);
}

int precedence(std::string_view op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=") return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%") return 6;
    return 0;
}

bool reserved(std::string_view word) {
    constexpr std::array words = {"fn", "let", "var", "struct", "enum", "match", "if", "else",
        "for", "in", "while", "return", "import", "test", "mut", "int", "bool", "str", "void",
        "float", "true", "false", "assert", "print"};
    return contains(words, word);
}

[[noreturn]] void suggest(std::string id, std::string message, Span span,
                          std::string replacement, std::string example) {
    Diagnostic diagnostic(std::move(id), std::move(message), span);
    diagnostic.fix = Edit{span, std::move(replacement)};
    diagnostic.example = std::move(example);
    throw diagnostic;
}

struct Tree {
    Expr value;
    unsigned height;
};

class Parser {
    std::string_view source;
    std::vector<Token> tokens;
    std::size_t pos = 0;
    Program result;

    const Token& current() const { return tokens[pos]; }
    const Token& previous() const { return tokens[pos - 1]; }
    bool at(std::string_view text) const {
        return (current().kind == TokenKind::Symbol || current().kind == TokenKind::Word) &&
               current().text == text;
    }
    bool take(std::string_view text) {
        if (!at(text)) return false;
        ++pos;
        return true;
    }
    bool newlines() {
        auto before = pos;
        while (current().kind == TokenKind::Newline) ++pos;
        return before != pos;
    }
    [[noreturn]] void expected(std::string what) const {
        if (at(";"))
            suggest("E_SEMICOLON", "Separa las sentencias con un salto de línea.", current().span,
                    pos + 1 < tokens.size() && tokens[pos + 1].kind != TokenKind::Newline &&
                    tokens[pos + 1].kind != TokenKind::End && tokens[pos + 1].text != "}" ? "\n" : "",
                    "let n = 1");
        if (at("elif")) {
            Span span = current().span;
            auto prev = pos;
            while (prev && tokens[prev - 1].kind == TokenKind::Newline) --prev;
            if (prev && prev < pos && tokens[prev - 1].text == "}") span.start = tokens[prev - 1].span.end;
            suggest("E_ELIF", "Escribe else if después de la llave.", span,
                    span.start == current().span.start ? "else if" : " else if",
                    "if a { return 1 } else if b { return 2 }");
        }
        Diagnostic diagnostic("E_PARSE", "Se esperaba " + what + ".", current().span);
        diagnostic.expected = std::move(what);
        diagnostic.actual = current().kind == TokenKind::End ? "fin de archivo" : current().text;
        throw diagnostic;
    }
    void need(std::string_view text) {
        if (!take(text)) expected(std::string(text));
    }
    Token name() {
        if (at("elif")) expected("nombre o sentencia");
        if (at("println") && tokens[pos + 1].text == "!" && tokens[pos + 1].kind == TokenKind::Symbol)
            suggest("E_PRINTLN", "Usa print para imprimir una línea.", {current().span.start, tokens[pos+1].span.end},
                    "print", "print(42)");
        if (current().kind != TokenKind::Word || reserved(current().text)) expected("nombre");
        if (!lower(current().text.front()) || current().text.find('_') != std::string::npos)
            throw Diagnostic("E_NAME", "Los nombres de valores siguen camelCase ASCII.", current().span);
        return tokens[pos++];
    }
    bool atTypeName() const {
        return current().kind == TokenKind::Word && upper(current().text.front()) &&
               current().text.find('_') == std::string::npos;
    }
    Token typeNameToken() {
        if (!atTypeName()) expected("nombre de tipo PascalCase");
        return tokens[pos++];
    }
    Type type() {
        if (take("[")) { need("int"); need("]"); return Type::IntArray; }
        if (take("int")) return Type::Int;
        if (take("bool")) return Type::Bool;
        if (take("str")) return Type::Str;
        if (take("void")) return Type::Void;
        if (atTypeName()) return result.intern(typeNameToken().text);
        expected("tipo");
    }

    Param parameter() {
        auto id = name();
        need(":");
        auto annotation = type();
        return {id.text, annotation, {id.span.start, previous().span.end}};
    }

    std::vector<Param> parameters(bool functionParams = false) {
        std::vector<Param> fields;
        newlines();
        if (!take(")")) {
            do {
                newlines();
                const bool writable = functionParams && take("var");
                auto p = parameter();
                p.writable = writable;
                fields.push_back(std::move(p));
            } while (take(","));
            need(")");
        }
        return fields;
    }

    std::vector<Expr> args(unsigned depth, unsigned& height) {
        std::vector<Expr> list;
        newlines();
        if (!take(")")) {
            do {
                newlines();
                Tree arg = expression(1, depth + 1);
                height = std::max(height, arg.height + 1);
                list.push_back(std::move(arg.value));
            } while (take(","));
            need(")");
        }
        return list;
    }

    Tree constructor(unsigned depth) {
        auto nominal = typeNameToken();
        Expr value{ExprKind::Construct, nominal.span, nominal.text, {}};
        unsigned height = 1;
        if (take(".")) {
            value.text += "." + typeNameToken().text;
            if (take("(")) value.args = args(depth, height);
        } else {
            need("{");
            newlines();
            if (!take("}")) {
                do {
                    newlines();
                    value.labels.push_back(name().text);
                    need(":");
                    Tree field = expression(1, depth + 1);
                    height = std::max(height, field.height + 1);
                    value.args.push_back(std::move(field.value));
                } while (take(","));
                newlines();
                need("}");
            }
        }
        value.span.end = previous().span.end;
        return {std::move(value), height};
    }

    Tree primary(unsigned depth) {
        const Token first = current();
        if (take("-") || take("!")) {
            newlines();
            if (first.text == "-" && current().kind == TokenKind::Digits) {
                auto digits = std::string_view(current().text);
                const auto nonzero = digits.find_first_not_of('0');
                if (nonzero != digits.npos && digits.substr(nonzero) == "9223372036854775808") {
                    ++pos;
                    return {{ExprKind::Integer, {first.span.start, previous().span.end}, "-9223372036854775808", {}}, 1};
                }
            }
            Tree child = expression(7, depth + 1);
            Expr node{ExprKind::Unary, {first.span.start, child.value.span.end}, first.text, {}};
            node.args.push_back(std::move(child.value));
            return {std::move(node), child.height + 1};
        }
        if (take("(")) {
            newlines();
            Tree inside = expression(1, depth + 1);
            need(")");
            inside.value.span = {first.span.start, previous().span.end};
            return inside;
        }
        if (first.kind == TokenKind::Digits || first.kind == TokenKind::Quoted || at("true") || at("false")) {
            ++pos;
            auto kind = first.kind == TokenKind::Digits ? ExprKind::Integer :
                        first.kind == TokenKind::Quoted ? ExprKind::String : ExprKind::Boolean;
            return {{kind, first.span, first.text, {}}, 1};
        }
        if (atTypeName()) return constructor(depth);
        auto id = name();
        if (!take("(")) return {{ExprKind::Name, id.span, id.text, {}}, 1};
        unsigned height = 1;
        auto arguments = args(depth, height);
        return {{ExprKind::Call, {id.span.start, previous().span.end}, id.text, std::move(arguments)}, height};
    }

    Tree expression(int minimum = 1, unsigned depth = 1) {
        guardDepth(depth, current().span);
        auto left = primary(depth);
        while (true) {
            guardDepth(left.height, left.value.span);
            if (take("[")) {
                Tree index = expression(1, depth + 1);
                need("]");
                Expr access{ExprKind::Index, {left.value.span.start, previous().span.end}, {}, {}};
                access.args.push_back(std::move(left.value));
                access.args.push_back(std::move(index.value));
                left = {std::move(access), 1 + std::max(left.height, index.height)};
                continue;
            }
            if (take(".")) {
                auto attr = name();
                auto base = left.value.span;
                Expr field{ExprKind::Field, {base.start, attr.span.end}, attr.text, {}};
                field.args.push_back(std::move(left.value));
                left = {std::move(field), left.height + 1};
                continue;
            }
            const int rank = current().kind == TokenKind::Symbol ? precedence(current().text) : 0;
            if (rank < minimum) return left;
            auto op = tokens[pos++].text;
            newlines();
            auto right = expression(rank + 1, depth + 1);
            Expr binary{ExprKind::Binary, {left.value.span.start, right.value.span.end}, op, {}};
            binary.args.push_back(std::move(left.value));
            binary.args.push_back(std::move(right.value));
            left = {std::move(binary), 1 + std::max(left.height, right.height)};
        }
    }

    std::vector<Stmt> block(unsigned depth) {
        guardDepth(depth, current().span);
        need("{");
        newlines();
        std::vector<Stmt> body;
        while (!take("}")) {
            body.push_back(statement(depth));
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
        return body;
    }

    std::vector<MatchArm> matchArms(unsigned depth) {
        need("{");
        newlines();
        std::vector<MatchArm> arms;
        while (!take("}")) {
            auto nominal = typeNameToken();
            need(".");
            MatchArm arm;
            arm.variant = nominal.text + "." + typeNameToken().text;
            if (take("(")) {
                newlines();
                if (!take(")")) {
                    do {
                        newlines();
                        arm.names.push_back(name().text);
                    } while (take(","));
                    need(")");
                }
            }
            need("=>");
            newlines();
            if (at("{")) arm.body = block(depth);
            else arm.body.push_back(statement(depth));
            arm.span = {nominal.span.start, previous().span.end};
            arms.push_back(std::move(arm));
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
        return arms;
    }

    Stmt statement(unsigned depth) {
        guardDepth(depth, current().span);
        const auto start = current().span.start;
        Stmt stmt{};
        if (take("let") || take("var")) {
            stmt.kind = previous().text == "let" ? StmtKind::Let : StmtKind::Var;
            if (stmt.kind == StmtKind::Let && at("mut"))
                suggest("E_LET_MUT", "Declara una variable mutable con var.", {start, current().span.end},
                        "var", "var n = 1");
            stmt.name = name().text;
            if (take(":")) stmt.annotation = type();
            need("=");
            newlines();
            stmt.exprs.push_back(expression().value);
        } else if (take("if") || take("while")) {
            stmt.kind = previous().text == "if" ? StmtKind::If : StmtKind::While;
            stmt.exprs.push_back(expression().value);
            stmt.body = block(depth + 1);
            if (stmt.kind == StmtKind::If && take("else")) {
                if (at("if")) stmt.otherwise.push_back(statement(depth + 1));
                else stmt.otherwise = block(depth + 1);
            }
        } else if (take("match")) {
            stmt.kind = StmtKind::Match;
            stmt.exprs.push_back(expression().value);
            stmt.arms = matchArms(depth + 1);
        } else if (take("for")) {
            stmt.kind = StmtKind::For;
            stmt.name = name().text;
            need("in");
            stmt.exprs.push_back(expression().value);
            need("..");
            newlines();
            stmt.exprs.push_back(expression().value);
            stmt.body = block(depth + 1);
        } else if (take("return")) {
            stmt.kind = StmtKind::Return;
            if (!at("}") && current().kind != TokenKind::Newline && current().kind != TokenKind::End)
                stmt.exprs.push_back(expression().value);
        } else if (take("assert")) {
            stmt.kind = StmtKind::Assert;
            stmt.exprs.push_back(expression().value);
        } else if (take("print")) {
            stmt.kind = StmtKind::Print;
            need("(");
            newlines();
            stmt.exprs.push_back(expression().value);
            need(")");
        } else {
            auto target = name();
            if (take("[")) {
                stmt.kind = StmtKind::IndexSet;
                stmt.name = target.text;
                stmt.exprs.push_back(expression().value);
                need("]");
                need("=");
                newlines();
                stmt.exprs.push_back(expression().value);
            } else if (take("(")) {
                stmt.kind = StmtKind::Call;
                unsigned height = 1;
                auto arguments = args(1, height);
                guardDepth(height, target.span);
                stmt.exprs.push_back({ExprKind::Call, {start, previous().span.end}, target.text, std::move(arguments)});
            } else {
                constexpr std::array assign = {"=", "+=", "-=", "*=", "/=", "%="};
                if (current().kind != TokenKind::Symbol || !contains(assign, current().text))
                    expected("asignación o llamada");
                auto op = tokens[pos++].text;
                newlines();
                auto rhs = expression();
                if (op.size() == 2) {
                    guardDepth(rhs.height + 1, target.span);
                    Expr binary{ExprKind::Binary, {start, rhs.value.span.end}, op.substr(0, 1), {}};
                    binary.args.push_back({ExprKind::Name, target.span, target.text, {}});
                    binary.args.push_back(std::move(rhs.value));
                    rhs.value = std::move(binary);
                }
                stmt.kind = StmtKind::Assign;
                stmt.name = target.text;
                stmt.exprs.push_back(std::move(rhs.value));
            }
        }
        stmt.span = {start, previous().span.end};
        return stmt;
    }

    DataDecl dataDeclaration() {
        const auto start = current().span.start;
        bool enumeration = take("enum");
        if (!enumeration) need("struct");
        auto id = typeNameToken();
        DataDecl data{result.intern(id.text), enumeration, {}, {}};
        if (!enumeration) data.variants.push_back({id.text, id.span, {}});
        need("{");
        newlines();
        while (!take("}")) {
            if (enumeration) {
                auto variant = typeNameToken();
                auto fields = take("(") ? parameters() : std::vector<Param>{};
                data.variants.push_back({variant.text, {variant.span.start, previous().span.end}, std::move(fields)});
            } else {
                data.variants[0].fields.push_back(parameter());
            }
            if (take(",")) newlines();
            else if (!at("}") && !newlines()) expected("coma, salto de línea o }");
        }
        data.span = {start, previous().span.end};
        return data;
    }

    Function declaration() {
        Function fn;
        const auto start = current().span.start;
        if (take("test")) {
            if (current().kind != TokenKind::Quoted) expected("nombre de prueba entre comillas");
            fn.name = tokens[pos++].text;
            fn.isTest = true;
        } else {
            need("fn");
            fn.name = name().text;
            if (take("<")) {
                do {
                    newlines();
                    auto id = typeNameToken();
                    auto generic = result.intern(id.text);
                    if (std::find(fn.generics.begin(), fn.generics.end(), generic) != fn.generics.end())
                        throw Diagnostic("E_DUP_GENERIC", "Parámetro genérico repetido.", id.span);
                    fn.generics.push_back(generic);
                } while (take(","));
                need(">");
            }
            need("(");
            fn.params = parameters(true);
            if (take("->")) {
                newlines();
                fn.result = type();
            }
        }
        fn.bodyStart = current().span.end;
        fn.body = block(1);
        fn.span = {start, previous().span.end};
        return fn;
    }
public:
    explicit Parser(std::string_view input) : source(input), tokens(lex(input)) {}
    Program program() {
        result.source = source;
        newlines();
        while (current().kind != TokenKind::End) {
            if (at("def")) {
                Diagnostic d("E_PYTHON_DEF", "En Turi una función empieza con fn, lleva llaves y devuelve el resultado con return.", current().span);
                d.expected = "fn"; d.actual = "def";
                d.example = "fn next(n: int) -> int { return n + 1 }";
                throw d;
            }
            if (at("struct") || at("enum")) result.data.push_back(dataDeclaration());
            else result.functions.push_back(declaration());
            if (current().kind != TokenKind::End && !newlines()) expected("salto de línea");
        }
        return std::move(result);
    }
};

} // namespace
Program parse(std::string_view source) { return Parser(source).program(); }
} // namespace veloz
