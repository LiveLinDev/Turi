#include "lexer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <limits>
#include <unordered_set>
#include <utility>

#ifndef TURI_PARAM_VAR
#define TURI_PARAM_VAR 1
#endif

namespace veloz::syntax {

bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool upper(char c) { return c >= 'A' && c <= 'Z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool hexDigit(char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool binDigit(char c) { return c == '0' || c == '1'; }
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

void scanStringToken(std::string_view source, std::size_t& cursor, std::size_t start,
                     bool& isInterp, std::string& plain) {
    while (cursor < source.size()) {
        if (source[cursor] == '"') {
            ++cursor;
            return;
        }
        char ch = source[cursor++];
        if (static_cast<unsigned char>(ch) < 0x20)
            throw Diagnostic("E_STRING", "Carácter de control dentro del texto.", {cursor - 1, cursor});
        if (ch == '\\') {
            if (cursor == source.size())
                throw Diagnostic("E_STRING", "Falta cerrar el texto con comillas.", {start, cursor});
            ch = source[cursor++];
            switch (ch) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case '"': case '\\': case '{': case '}': break;
                default: throw Diagnostic("E_ESCAPE", "Escape de texto desconocido.", {cursor - 2, cursor});
            }
            plain += ch;
        } else if (ch == '{') {
            if (cursor < source.size() && source[cursor] == '{') {
                ++cursor;
                plain += '{';
            } else {
                isInterp = true;
                int depth = 1;
                while (cursor < source.size() && depth > 0) {
                    char ec = source[cursor++];
                    if (static_cast<unsigned char>(ec) < 0x20)
                        throw Diagnostic("E_STRING", "Falta cerrar '}' en la interpolación.", {cursor - 1, cursor});
                    if (ec == '{') ++depth;
                    else if (ec == '}') --depth;
                    else if (ec == '"') {
                        bool dummyInterp = false;
                        std::string dummyPlain;
                        scanStringToken(source, cursor, cursor - 1, dummyInterp, dummyPlain);
                    }
                }
                if (depth > 0)
                    throw Diagnostic("E_STRING", "Falta cerrar '}' en la interpolación.", {start, cursor});
            }
        } else if (ch == '}') {
            if (cursor < source.size() && source[cursor] == '}') {
                ++cursor;
                plain += '}';
            } else {
                throw Diagnostic("E_STRING", "Llave '}' sin abrir en el texto; usa \\} o }}.", {cursor - 1, cursor});
            }
        } else {
            plain += ch;
        }
    }
    throw Diagnostic("E_STRING", "Falta cerrar el texto con comillas.", {start, cursor});
}

std::vector<Token> lex(std::string_view source, bool keepComments, std::size_t file) {
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
            if (c == '0' && cursor < source.size() && (source[cursor] == 'x' || source[cursor] == 'X')) {
                ++cursor;
                const auto digitsStart = cursor;
                while (cursor < source.size() && hexDigit(source[cursor])) ++cursor;
                if (cursor == digitsStart)
                    throw Diagnostic("E_NUMBER", "Faltan dígitos hexadecimales tras 0x.", {start, cursor});
                kind = TokenKind::Digits;
            } else if (c == '0' && cursor < source.size() && (source[cursor] == 'b' || source[cursor] == 'B')) {
                ++cursor;
                const auto digitsStart = cursor;
                while (cursor < source.size() && binDigit(source[cursor])) ++cursor;
                if (cursor == digitsStart)
                    throw Diagnostic("E_NUMBER", "Faltan dígitos binarios tras 0b.", {start, cursor});
                kind = TokenKind::Digits;
            } else {
                while (cursor < source.size() && digit(source[cursor])) ++cursor;
                bool isFloat = false;
                if (cursor + 1 < source.size() && source[cursor] == '.' && digit(source[cursor + 1])) {
                    isFloat = true;
                    ++cursor;
                    while (cursor < source.size() && digit(source[cursor])) ++cursor;
                }
                if (cursor < source.size() && (source[cursor] == 'e' || source[cursor] == 'E')) {
                    auto expPos = cursor + 1;
                    if (expPos < source.size() && (source[expPos] == '+' || source[expPos] == '-')) ++expPos;
                    if (expPos < source.size() && digit(source[expPos])) {
                        isFloat = true;
                        cursor = expPos + 1;
                        while (cursor < source.size() && digit(source[cursor])) ++cursor;
                    }
                }
                kind = isFloat ? TokenKind::Float : TokenKind::Digits;
            }
        } else if (c == '\'' && cursor + 1 < source.size() && source[cursor] != '\\' && source[cursor] != '\'' &&
                   source[cursor + 1] == '\'' && static_cast<unsigned char>(source[cursor]) < 0x80) {
            // Literal de carácter 'a': el entero de su código ASCII, como en C, Rust, Go o Java.
            // Una etiqueta ('nombre) nunca va seguida de otra comilla, así que no hay ambigüedad.
            const int code = static_cast<unsigned char>(source[cursor]);
            cursor += 2;
            tokens.push_back({TokenKind::Digits, {start, cursor}, std::to_string(code)});
            tokens.back().isChar = true;
            continue;
        } else if (c == '\'' && cursor + 2 < source.size() && source[cursor] == '\\' && source[cursor + 2] == '\'' &&
                   std::string_view("nrt0\\'\"").find(source[cursor + 1]) != std::string_view::npos) {
            const char esc = source[cursor + 1];
            const int code = esc == 'n' ? 10 : esc == 'r' ? 13 : esc == 't' ? 9 : esc == '0' ? 0 : esc;
            cursor += 3;
            tokens.push_back({TokenKind::Digits, {start, cursor}, std::to_string(code)});
            tokens.back().isChar = true;
            continue;
        } else if (c == '\'') {
            if (cursor == source.size() || !lower(source[cursor]))
                throw Diagnostic("E_CHAR", "Etiqueta inválida; usa 'nombre.", {start, cursor});
            while (cursor < source.size() && wordChar(source[cursor])) ++cursor;
            auto lbl = std::string(source.substr(start + 1, cursor - start - 1));
            if (lbl.find('_') != std::string::npos)
                throw Diagnostic("E_NAME", "Las etiquetas siguen camelCase ASCII.", {start, cursor});
            tokens.push_back({TokenKind::Label, {start, cursor}, std::move(lbl)});
            continue;
        } else if (c == '"') {
            bool isInterp = false;
            std::string value;
            scanStringToken(source, cursor, start, isInterp, value);
            if (isInterp)
                tokens.push_back({TokenKind::Interp, {start, cursor}, std::string(source.substr(start, cursor - start))});
            else
                tokens.push_back({TokenKind::Quoted, {start, cursor}, std::move(value)});
            continue;
        } else {
            constexpr std::array three = {">>>", "<<=", ">>="};
            constexpr std::array two = {"->", "=>", "..", "==", "!=", "<=", ">=", "&&", "||",
                                        "+=", "-=", "*=", "/=", "%=", "<<", ">>", "&=", "|=", "^="};
            if (source.substr(start, 4) == ">>>=") cursor += 3;
            else if (contains(three, source.substr(start, 3))) cursor += 2;
            else if (contains(two, source.substr(start, 2))) ++cursor;
            else if (c == '`') {
                Diagnostic d("E_CHAR", "Turi usa comillas dobles con \"{x}\" en lugar de comillas invertidas.", {start, cursor});
                d.example = R"("valor = {x}")";
                throw d;
            }
            else if (std::string_view("{}[](),:!+-*/%<>=.;&|^~?@").find(c) == std::string_view::npos)
                throw Diagnostic("E_CHAR", "Carácter inesperado.", {start, cursor});
        }
        tokens.push_back({kind, {start, cursor}, std::string(source.substr(start, cursor - start))});
    }
    tokens.push_back({TokenKind::End, {source.size(), source.size()}, {}});
    for (auto& token : tokens) token.span.file = file;
    return tokens;
}
// Front-end tolerante. Un modelo entrenado con otros lenguajes escribe por costumbre grafías que
// Turi no usa (`;`, `elif`, `let mut`, `x++`, `println!`, `and`, `True`, `0..=n`). Cuando solo hay
// una lectura posible se convierten aquí en su forma canónica en vez de devolver un error que
// cuesta otra ronda de reparación. `veloz fmt` sigue escribiendo la forma canónica.
std::vector<Token> tolerate(std::vector<Token> in, bool force) {
    const bool strict = strictSyntax() && !force;
    std::vector<Token> out;
    out.reserve(in.size() + 8);
    const auto sym = [](const Token* t, std::string_view text) {
        return t && t->kind == TokenKind::Symbol && t->text == text;
    };
    const auto word = [](const Token* t, std::string_view text) {
        return t && t->kind == TokenKind::Word && t->text == text;
    };
    const auto endsLine = [&](const Token* t) {
        return !t || t->kind == TokenKind::Newline || t->kind == TokenKind::End || sym(t, "}") || sym(t, ";");
    };
    const auto endsOperand = [&](const Token* t) {
        if (!t) return false;
        if (t->kind == TokenKind::Digits || t->kind == TokenKind::Float || t->kind == TokenKind::Quoted ||
            t->kind == TokenKind::Interp) return true;
        if (t->kind == TokenKind::Symbol) return t->text == ")" || t->text == "]";
        constexpr std::array opens = {"if", "while", "return", "in", "assert", "match", "let", "var", "else",
                                      "and", "or", "not", "for", "requires", "ensures"};
        return t->kind == TokenKind::Word && !contains(opens, std::string_view(t->text));
    };
    for (std::size_t i = 0; i < in.size(); ++i) {
        Token& t = in[i];
        const Token* next = i + 1 < in.size() ? &in[i + 1] : nullptr;
        const Token* after = i + 2 < in.size() ? &in[i + 2] : nullptr;
        const Token* prev = out.empty() ? nullptr : &out.back();
        if (sym(&t, "..") && sym(next, "=") && next->span.start == t.span.end) {
            // Rango inclusivo `a..=b` (sintaxis oficial): el parser lo baja a `a..b + 1`.
            out.push_back({TokenKind::Symbol, {t.span.start, next->span.end, t.span.file}, "..="});
            ++i;
            continue;
        }
        if (strict) {
            out.push_back(std::move(t));
            continue;
        }
        if (sym(&t, ";")) {
            // `;` al final de una sentencia equivale al salto de línea.
            if (!prev || prev->kind == TokenKind::Newline || sym(prev, "{")) continue;
            if (next && (next->kind == TokenKind::Newline || next->kind == TokenKind::End || sym(next, "}"))) continue;
            out.push_back({TokenKind::Newline, t.span, "\n", true});
            continue;
        }
        if (word(&t, "elif")) {
            out.push_back({TokenKind::Word, t.span, "else", true});
            out.push_back({TokenKind::Word, t.span, "if", true});
            continue;
        }
        if (word(&t, "let") && word(next, "mut")) {
            out.push_back({TokenKind::Word, t.span, "var", true});
            ++i;
            continue;
        }
        if ((sym(&t, "+") || sym(&t, "-")) && next && next->kind == TokenKind::Symbol && next->text == t.text &&
            next->span.start == t.span.end && endsOperand(prev) && endsLine(after)) {
            // `x++` / `x--` como sentencia.
            out.push_back({TokenKind::Symbol, {t.span.start, next->span.end, t.span.file}, t.text + "=", true});
            out.push_back({TokenKind::Digits, next->span, "1", true});
            ++i;
            continue;
        }
        if ((word(&t, "println") || word(&t, "print") || word(&t, "eprintln") || word(&t, "eprint")) &&
            (!prev || !sym(prev, "."))) {
            const bool bang = sym(next, "!") && sym(after, "(");
            if (bang || (t.text.ends_with("ln") && sym(next, "("))) {
                out.push_back({TokenKind::Word, t.span, t.text.starts_with("e") ? "eprint" : "print", true});
                if (bang) ++i;
                continue;
            }
        }
        if ((word(&t, "and") || word(&t, "or")) && endsOperand(prev) && next &&
            !sym(next, "=") && !sym(next, ":") && !sym(next, ".") && !sym(next, "(") && !sym(next, ",") &&
            !sym(next, ")") && next->kind != TokenKind::Newline) {
            out.push_back({TokenKind::Symbol, t.span, t.text == "and" ? "&&" : "||", true});
            continue;
        }
        if (word(&t, "not") && !endsOperand(prev) && next &&
            (next->kind == TokenKind::Word || next->kind == TokenKind::Digits || sym(next, "("))) {
            out.push_back({TokenKind::Symbol, t.span, "!", true});
            continue;
        }
        if ((word(&t, "True") || word(&t, "False")) && !sym(next, ".") && !sym(next, "{") && !sym(next, "(") &&
            !sym(next, "<") && !sym(prev, ".") && !sym(prev, ":") && !sym(prev, "->")) {
            out.push_back({TokenKind::Word, t.span, t.text == "True" ? "true" : "false", true});
            continue;
        }
        if ((word(&t, "null") || word(&t, "nil")) && !sym(next, ".") && !sym(next, "(") && !sym(prev, ".") &&
            !word(prev, "let") && !word(prev, "var")) {
            out.push_back({TokenKind::Word, t.span, "None", true});
            continue;
        }
        out.push_back(std::move(t));
    }
    return out;
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
    if (op == "|") return 5;
    if (op == "^") return 6;
    if (op == "&") return 7;
    if (op == "<<" || op == ">>" || op == ">>>") return 8;
    if (op == "+" || op == "-") return 9;
    if (op == "*" || op == "/" || op == "%") return 10;
    return 0;
}

bool reserved(std::string_view word) {
    constexpr std::array words = {"fn", "let", "var", "const", "struct", "enum", "match", "if", "else",
        "for", "in", "while", "break", "continue", "return", "import", "test", "mut", "int", "bool",
        "str", "void", "float", "true", "false", "assert", "print", "interface", "impl", "dyn",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
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


struct ParsedAttrs {
    bool reprC = false;
    bool packed = false;
    std::size_t explicitAlign = 0;
    std::size_t start = 0;
    bool any = false;
    std::vector<std::string> linkLibs{};
    bool noCopy = false;
    bool arenaHint = false;
    bool compactHint = false;
};

class Parser {
    std::string_view source;
    std::vector<Token> tokens;
    std::size_t pos = 0;
    Program result;
    Program* shared = nullptr;
    std::string module;
    std::vector<std::string> genericScope;
    std::unordered_set<std::string> externCSymbols;

    static bool isCIdent(std::string_view w) {
        if (w.empty()) return false;
        const auto startOk = lower(w.front()) || upper(w.front()) || w.front() == '_';
        if (!startOk) return false;
        bool hasAlnum = false;
        for (char c : w) {
            if (!wordChar(c)) return false;
            if (lower(c) || upper(c) || digit(c)) hasAlnum = true;
        }
        return hasAlnum;
    }
    void collectExternSymbols() {
        for (std::size_t i = 0; i + 2 < tokens.size(); ++i) {
            if (tokens[i].kind == TokenKind::Word && tokens[i].text == "extern" &&
                tokens[i + 1].kind == TokenKind::Quoted && tokens[i + 1].text == "C") {
                std::size_t j = i + 2;
                while (j < tokens.size() && tokens[j].kind == TokenKind::Newline) ++j;
                if (j + 1 < tokens.size() && tokens[j].text == "fn" && tokens[j + 1].kind == TokenKind::Word) {
                    externCSymbols.insert(tokens[j + 1].text);
                } else if (j < tokens.size() && tokens[j].text == "{") {
                    int bdepth = 1;
                    ++j;
                    while (j < tokens.size() && bdepth > 0) {
                        if (tokens[j].text == "{") ++bdepth;
                        else if (tokens[j].text == "}") --bdepth;
                        else if (bdepth == 1 && tokens[j].text == "fn" && j + 1 < tokens.size() &&
                                 tokens[j + 1].kind == TokenKind::Word) {
                            externCSymbols.insert(tokens[j + 1].text);
                        }
                        ++j;
                    }
                }
            }
        }
    }
    bool isSharedExternSymbol(std::string_view sym) const {
        if (externCSymbols.contains(std::string(sym))) return true;
        if (!shared) return false;
        for (const auto& f : shared->functions) {
            if ((f.isExternC || f.isExternDecl) && f.cSymbol == sym) return true;
        }
        return false;
    }

    Type intern(std::string_view name) { return shared ? shared->intern(name) : result.intern(name); }
    Type arrayOf(Type elem) { return shared ? shared->arrayOf(elem) : result.arrayOf(elem); }
    Type typeApp(const std::string& base, const std::vector<Type>& args) {
        return shared ? shared->typeApp(base, args) : result.typeApp(base, args);
    }
    Type optionOf(Type inner) { return shared ? shared->optionOf(inner) : result.optionOf(inner); }
    Type resultOf(Type ok, Type err) { return shared ? shared->resultOf(ok, err) : result.resultOf(ok, err); }
    Type mapOf(Type key, Type val) { return shared ? shared->mapOf(key, val) : result.mapOf(key, val); }
    Type setOf(Type elem) { return shared ? shared->setOf(elem) : result.setOf(elem); }
    Type poolOf(Type elem) { return shared ? shared->poolOf(elem) : result.poolOf(elem); }
    Type handleOf(Type elem) { return shared ? shared->handleOf(elem) : result.handleOf(elem); }
    Type sharedOf(Type elem) { return shared ? shared->sharedOf(elem) : result.sharedOf(elem); }
    Type taskOf(Type elem) { return shared ? shared->taskOf(elem) : result.taskOf(elem); }
    Type frozenOf(Type elem) { return shared ? shared->frozenOf(elem) : result.frozenOf(elem); }
    Type dynOf(Type iface) { return shared ? shared->dynOf(iface) : result.dynOf(iface); }
    Type ptrOf(Type elem) { return shared ? shared->ptrOf(elem) : result.ptrOf(elem); }
    Type mutPtrOf(Type elem) { return shared ? shared->mutPtrOf(elem) : result.mutPtrOf(elem); }
    Type atomicOf(Type elem) { return shared ? shared->atomicOf(elem) : result.atomicOf(elem); }
    Type ringQueueOf(Type elem) { return shared ? shared->ringQueueOf(elem) : result.ringQueueOf(elem); }
    Type simdOf(Type elem, unsigned lanes) { return shared ? shared->simdOf(elem, lanes) : result.simdOf(elem, lanes); }
    Type fnOf(Type ret, const std::vector<Type>& params) {
        return shared ? shared->fnOf(ret, params) : result.fnOf(ret, params);
    }
    std::string qualifyType(std::string_view name) const {
        if (module.empty() || name == "Self" || contains(genericScope, name)) return std::string(name);
        return module + "." + std::string(name);
    }
    std::string qualifyFunction(std::string_view name) const {
        return module.empty() ? std::string(name) : module + "." + std::string(name);
    }
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
        // camelCase es la forma canónica; snake_case se acepta porque es lo que escriben los modelos
        // que vienen de Python, Rust o C. Los nombres de valores empiezan en minúscula o `_`.
        const char head = current().text.front();
        if (strictSyntax() && (!lower(head) || current().text.find('_') != std::string::npos))
            throw Diagnostic("E_NAME", "Los nombres de valores siguen camelCase ASCII.", current().span);
        if (!(lower(head) || (head == '_' && current().text.size() > 1)) || current().text.starts_with("__"))
            throw Diagnostic("E_NAME", "Los nombres de valores empiezan en minúscula (camelCase o snake_case).", current().span);
        return tokens[pos++];
    }
    Token bindingName() {
        if (current().kind == TokenKind::Word && current().text == "_") return tokens[pos++];
        return name();
    }
    bool looksLikeGenericCall() const {
        if (!at("<")) return false;
        std::size_t i = pos + 1;
        while (i < tokens.size() && tokens[i].kind == TokenKind::Newline) ++i;
        if (i >= tokens.size()) return false;
        auto isTypeStarter = [&](std::size_t idx) -> bool {
            const auto& t = tokens[idx];
            if (t.text == "[" || t.text == "dyn" || t.text == "fn" ||
                t.text == "int" || t.text == "float" || t.text == "bool" ||
                t.text == "str" || t.text == "void" || t.text == "StringBuilder" ||
                t.text == "File" || t.text == "TcpListener" || t.text == "Socket" ||
                t.text == "Mutex" || t.text == "Lock" || t.text == "Scope" || t.text == "Region" ||
                t.text == "i8" || t.text == "i16" || t.text == "i32" || t.text == "i64" ||
                t.text == "u8" || t.text == "u16" || t.text == "u32" || t.text == "u64" ||
                t.text == "f32" || t.text == "f64")
                return true;
            if (t.kind == TokenKind::Word && !t.text.empty() && upper(t.text.front()) &&
                t.text.find('_') == std::string::npos)
                return true;
            if (t.kind == TokenKind::Word && !t.text.empty() && lower(t.text.front()) &&
                idx + 2 < tokens.size() && tokens[idx + 1].text == "." &&
                tokens[idx + 2].kind == TokenKind::Word && upper(tokens[idx + 2].text.front()))
                return true;
            return false;
        };
        if (!isTypeStarter(i)) return false;
        int angleDepth = 1;
        int parenDepth = 0;
        int bracketDepth = 0;
        while (i < tokens.size() && angleDepth > 0) {
            const auto& t = tokens[i++];
            if (t.kind == TokenKind::Newline) continue;
            if (t.text == "<") { ++angleDepth; continue; }
            if (t.text == ">") { --angleDepth; continue; }
            if (t.text == ">>") { angleDepth -= 2; if (angleDepth < 0) return false; continue; }
            if (t.text == ">>>") { angleDepth -= 3; if (angleDepth < 0) return false; continue; }
            if (t.text == "(") { ++parenDepth; continue; }
            if (t.text == ")") { if (--parenDepth < 0) return false; continue; }
            if (t.text == "[") { ++bracketDepth; continue; }
            if (t.text == "]") { if (--bracketDepth < 0) return false; continue; }
            if (t.text == "," || t.text == "." || t.text == "?" || t.text == "->") continue;
            if (t.kind == TokenKind::Digits && angleDepth >= 2) continue;
            if (t.kind == TokenKind::Word && t.text.find('_') == std::string::npos) continue;
            return false;
        }
        if (angleDepth != 0 || parenDepth != 0 || bracketDepth != 0) return false;
        return i < tokens.size() && tokens[i].text == "(";
    }
    bool atTypeName() const {
        return current().kind == TokenKind::Word && upper(current().text.front()) &&
               current().text.find('_') == std::string::npos;
    }
    Token typeNameToken() {
        if (!atTypeName()) expected("nombre de tipo PascalCase");
        return tokens[pos++];
    }
    bool atQualifiedType() const {
        return current().kind == TokenKind::Word && !current().text.empty() && lower(current().text.front()) &&
               pos + 2 < tokens.size() && tokens[pos + 1].text == "." &&
               tokens[pos + 2].kind == TokenKind::Word && upper(tokens[pos + 2].text.front());
    }
    std::string nominal() {
        std::string name;
        if (atTypeName()) name = typeNameToken().text;
        else {
            auto prefix = this->name();
            need(".");
            name = prefix.text + "." + typeNameToken().text;
        }
        if (name.find('.') == std::string::npos) name = qualifyType(name);
        return name;
    }
    // Las lecturas especulativas retroceden con mark()/rewind(). Cerrar un genérico divide el token
    // `>>` en el propio vector, así que cada división se anota para poder deshacerla.
    struct Mark { std::size_t pos, edits; };
    std::vector<std::pair<std::size_t, Token>> tokenEdits;
    Mark mark() const { return {pos, tokenEdits.size()}; }
    void rewind(const Mark& m) {
        while (tokenEdits.size() > m.edits) {
            tokens[tokenEdits.back().first] = std::move(tokenEdits.back().second);
            tokenEdits.pop_back();
        }
        pos = m.pos;
    }
    void consumeCloseAngle() {
        if (take(">")) return;
        auto split = [&](std::string_view rem) {
            tokenEdits.emplace_back(pos, tokens[pos]);
            tokens[pos].text = std::string(rem);
            tokens[pos].span.start += 1;
        };
        if (at(">>")) { split(">"); return; }
        if (at(">>>")) { split(">>"); return; }
        if (at(">=")) { split("="); return; }
        if (at(">>=")) { split(">="); return; }
        if (at(">>>=")) { split(">>="); return; }
        expected(">");
    }
    std::vector<Type> parseTypeArgs() {
        need("<");
        newlines();
        std::vector<Type> list;
        if (!at(">") && !at(">>") && !at(">>>") && !at(">=") && !at(">>=") && !at(">>>=")) {
            do {
                newlines();
                list.push_back(type());
                newlines();
            } while (take(","));
        }
        newlines();
        consumeCloseAngle();
        if (list.empty())
            throw Diagnostic("E_GENERIC", "Se esperaba al menos un argumento de tipo en <...>.", previous().span);
        return list;
    }
    Type baseType() {
        if (take("(")) {
            // (A, B) es una tupla: la estructura genérica Tuple2<A, B> de la biblioteca base.
            newlines();
            std::vector<Type> parts;
            if (!at(")")) {
                do {
                    newlines();
                    if (at(")")) break;
                    parts.push_back(type());
                    newlines();
                } while (take(","));
            }
            const Span close = current().span;
            need(")");
            if (parts.size() == 1) return parts[0];
            if (parts.size() < 2 || parts.size() > 4)
                throw Diagnostic("E_TUPLE", "Una tupla tiene de 2 a 4 elementos; usa un struct para más.", close);
            return typeApp("Tuple" + std::to_string(parts.size()), parts);
        }
        if (take("fn")) {
            newlines();
            need("(");
            newlines();
            std::vector<Type> params;
            if (!take(")")) {
                do {
                    newlines();
                    params.push_back(type());
                    newlines();
                } while (take(","));
                newlines();
                need(")");
            }
            Type ret = Type::Void;
            if (take("->")) {
                newlines();
                ret = type();
            }
            return fnOf(ret, params);
        }
        if (take("dyn")) {
            newlines();
            if (!atTypeName() && !atQualifiedType())
                throw Diagnostic("E_DYN_TYPE", "dyn requiere el nombre de una interfaz.", current().span);
            std::string iface = nominal();
            if (at("<"))
                throw Diagnostic("E_DYN_TYPE", "Las interfaces en dyn no admiten parámetros de tipo.", current().span);
            return dynOf(intern(iface));
        }
        if (take("[")) {
            newlines();
            Type elem = type();
            newlines();
            need("]");
            return arrayOf(elem);
        }
        if (take("int") || take("i64")) return Type::Int;
        if (take("float") || take("f64")) return Type::Float;
        if (take("i8")) return Type::I8;
        if (take("i16")) return Type::I16;
        if (take("i32")) return Type::I32;
        if (take("u8")) return Type::U8;
        if (take("u16")) return Type::U16;
        if (take("u32")) return Type::U32;
        if (take("u64")) return Type::U64;
        if (take("f32")) return Type::F32;
        if (take("bool")) return Type::Bool;
        if (take("str")) return Type::Str;
        if (take("void")) return Type::Void;
        if (take("StringBuilder")) return Type::StringBuilder;
        if (take("File")) return Type::File;
        if (take("TcpListener")) return Type::TcpListener;
        if (take("Socket")) return Type::Socket;
        if (take("Mutex")) return Type::Mutex;
        if (take("Lock")) return Type::Lock;
        if (take("Scope")) return Type::Scope;
        if (take("Region")) return Type::Region;
        if (!atQualifiedType() &&
            (at("Option") || at("Result") || at("Map") || at("Set") ||
             at("Pool") || at("Handle") || at("Shared") || at("Task") || at("Frozen") ||
             at("Ptr") || at("MutPtr") || at("Atomic") || at("RingQueue") || at("Simd")) &&
            !contains(genericScope, current().text)) {
            auto tok = typeNameToken();
            auto exampleFor = [](std::string_view name) -> const char* {
                if (name == "Option") return "Option<int>";
                if (name == "Result") return "Result<int, str>";
                if (name == "Map") return "Map<str, int>";
                if (name == "Set") return "Set<int>";
                if (name == "Pool") return "Pool<Node>";
                if (name == "Handle") return "Handle<Node>";
                if (name == "Task") return "Task<int>";
                if (name == "Frozen") return "Frozen<[int]>";
                if (name == "Ptr") return "Ptr<u8>";
                if (name == "MutPtr") return "MutPtr<u8>";
                if (name == "Atomic") return "Atomic<int>";
                if (name == "RingQueue") return "RingQueue<int>";
                if (name == "Simd") return "Simd<f32, 4>";
                return "Shared<int>";
            };
            if (tok.text == "Simd") {
                if (!at("<")) {
                    Diagnostic d("E_GENERIC", "Simd<T, N> requiere argumentos entre <...>.", tok.span);
                    d.example = "Simd<f32, 4>";
                    throw d;
                }
                need("<");
                newlines();
                const Type elemT = type();
                newlines();
                need(",");
                newlines();
                if (current().kind != TokenKind::Digits) {
                    Diagnostic d("E_SIMD_TYPE", "Simd<T, N> requiere N entero literal en {2, 4, 8, 16, 32}.", current().span);
                    d.example = "Simd<f32, 4>";
                    throw d;
                }
                const auto laneTok = tokens[pos++];
                std::uint64_t lanes = 0;
                const auto endPtr = laneTok.text.data() + laneTok.text.size();
                const auto [ptr, ec] = std::from_chars(laneTok.text.data(), endPtr, lanes, 10);
                if (ec != std::errc{} || ptr != endPtr ||
                    (lanes != 2 && lanes != 4 && lanes != 8 && lanes != 16 && lanes != 32)) {
                    Diagnostic d("E_SIMD_TYPE", "Simd<T, N> requiere N en {2, 4, 8, 16, 32}.", laneTok.span);
                    d.example = "Simd<f32, 4>";
                    throw d;
                }
                newlines();
                consumeCloseAngle();
                return simdOf(elemT, static_cast<unsigned>(lanes));
            }
            if (!at("<")) {
                Diagnostic d("E_GENERIC", tok.text + " requiere argumentos de tipo entre <...>.", tok.span);
                d.example = exampleFor(tok.text);
                throw d;
            }
            auto targs = parseTypeArgs();
            const bool unary = (tok.text == "Option" || tok.text == "Set" ||
                                tok.text == "Pool" || tok.text == "Handle" ||
                                tok.text == "Shared" || tok.text == "Task" || tok.text == "Frozen" ||
                                tok.text == "Ptr" || tok.text == "MutPtr" ||
                                tok.text == "Atomic" || tok.text == "RingQueue");
            if (unary) {
                if (targs.size() != 1) {
                    Diagnostic d("E_GENERIC", tok.text + "<T> requiere exactamente 1 argumento de tipo.", tok.span);
                    d.example = exampleFor(tok.text);
                    throw d;
                }
                if (tok.text == "Option") return optionOf(targs[0]);
                if (tok.text == "Set") return setOf(targs[0]);
                if (tok.text == "Pool") return poolOf(targs[0]);
                if (tok.text == "Handle") return handleOf(targs[0]);
                if (tok.text == "Task") return taskOf(targs[0]);
                if (tok.text == "Frozen") return frozenOf(targs[0]);
                if (tok.text == "Ptr") return ptrOf(targs[0]);
                if (tok.text == "MutPtr") return mutPtrOf(targs[0]);
                if (tok.text == "Atomic") return atomicOf(targs[0]);
                if (tok.text == "RingQueue") return ringQueueOf(targs[0]);
                return sharedOf(targs[0]);
            }
            if (targs.size() != 2) {
                Diagnostic d("E_GENERIC", tok.text + "<...> requiere exactamente 2 argumentos de tipo.", tok.span);
                d.example = exampleFor(tok.text);
                throw d;
            }
            return tok.text == "Result" ? resultOf(targs[0], targs[1]) : mapOf(targs[0], targs[1]);
        }
        if (atTypeName() || atQualifiedType()) {
            std::string base = nominal();
            if (at("<")) {
                auto targs = parseTypeArgs();
                return typeApp(base, targs);
            }
            return intern(base);
        }
        expected("tipo");
    }
    Type type() {
        Type t = baseType();
        while (take("?")) t = optionOf(t);
        return t;
    }

    Param parameter() {
        auto id = name();
        need(":");
        auto annotation = type();
        return {id.text, annotation, {id.span.start, previous().span.end}};
    }

    std::vector<Param> parameters(bool functionParams = false, bool allowSelf = false) {
        std::vector<Param> fields;
        newlines();
        if (!take(")")) {
            do {
                newlines();
                if (at(")")) break;
                if (functionParams && allowSelf && fields.empty()) {
                    const auto pStart = current().span.start;
                    if (at("&")) {
                        if (pos + 2 < tokens.size() && tokens[pos + 1].text == "mut" && tokens[pos + 2].text == "self") {
                            Diagnostic d("E_SELF_MUT", "En Turi usa mut self sin &.", {pStart, tokens[pos + 2].span.end});
                            d.fix = Edit{{pStart, tokens[pos + 2].span.end}, "mut self"};
                            d.example = "fn grow(mut self, n: int) { ... }";
                            throw d;
                        }
                        if (pos + 1 < tokens.size() && tokens[pos + 1].text == "self") {
                            Diagnostic d("E_SELF_MUT", "En Turi usa self sin &.", {pStart, tokens[pos + 1].span.end});
                            d.fix = Edit{{pStart, tokens[pos + 1].span.end}, "self"};
                            d.example = "fn area(self) -> int { ... }";
                            throw d;
                        }
                    }
                    if (at("mut") && pos + 1 < tokens.size() && tokens[pos + 1].text == "self") {
                        pos += 2;
                        if (at(":"))
                            throw Diagnostic("E_PARAM_MUT", "Escribe mut self o self: mut Tipo.", {pStart, current().span.end});
                        fields.push_back({"self", intern("Self"), {pStart, previous().span.end}, true, true});
                        newlines();
                        continue;
                    }
                    if (at("self") && (pos + 1 >= tokens.size() || tokens[pos + 1].text != ":")) {
                        ++pos;
                        fields.push_back({"self", intern("Self"), previous().span, false, false});
                        newlines();
                        continue;
                    }
                }
                if (functionParams && at("mut"))
                    throw Diagnostic("E_PARAM_MUT", "Escribe mut después de los dos puntos: nombre: mut Tipo.", current().span);
                if (!TURI_PARAM_VAR && functionParams && at("var"))
                    throw Diagnostic("E_PARAM_VAR", "La variante A exige una copia local; no admite var en la firma.", current().span);
                const bool writable = functionParams && take("var");
                auto id = name();
                need(":");
                newlines();
                const bool byRef = functionParams && take("mut");
                if (writable && byRef)
                    throw Diagnostic("E_PARAM_MUT", "No combines var y mut en el mismo parámetro; usa mut.", current().span);
                if (byRef) newlines();
                auto annotation = type();
                Param p{id.text, annotation, {id.span.start, previous().span.end}, writable || byRef, byRef};
                fields.push_back(std::move(p));
                newlines();
            } while (take(","));
            newlines();
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
                if (at(")")) break;
                if (at("&") && pos + 1 < tokens.size() && tokens[pos + 1].text == "mut")
                    suggest("E_REF_MUT", "En Turi pasa argumentos mutables sin &: mut nombre.",
                            {current().span.start, tokens[pos + 1].span.end}, "mut", "bump(mut x)");
                if (at("&"))
                    suggest("E_REF_MUT", "En Turi pasa argumentos mutables sin &: mut nombre.",
                            current().span, "mut ", "bump(mut x)");
                const bool isMut = take("mut");
                if (isMut) newlines();
                Tree arg = expression(1, depth + 1);
                arg.value.isMutArg = isMut;
                height = std::max(height, arg.height + 1);
                list.push_back(std::move(arg.value));
                newlines();
            } while (take(","));
            newlines();
            need(")");
        }
        return list;
    }

    Tree constructor(unsigned depth) {
        const auto start = current().span;
        if (!atQualifiedType() && (at("None") || at("Some") || at("Ok") || at("Err"))) {
            if (at("None")) {
                const bool hasDelim = pos + 1 < tokens.size() &&
                    (tokens[pos + 1].text == "." || tokens[pos + 1].text == "<" || tokens[pos + 1].text == "{");
                if (!hasDelim) {
                    auto tok = typeNameToken();
                    if (take("(")) {
                        unsigned h = 1;
                        (void)args(depth, h);
                        throw Diagnostic("E_ARITY", "None no recibe argumentos.", {tok.span.start, previous().span.end});
                    }
                    return {{ExprKind::Construct, tok.span, "None", {}, Type::Unknown, {}, {},
                             ArrayAllocation::None, false, false, {}, true}, 1};
                }
            } else if (pos + 1 < tokens.size() && tokens[pos + 1].text == "(") {
                auto tok = typeNameToken();
                need("(");
                unsigned height = 1;
                auto arguments = args(depth, height);
                return {{ExprKind::Construct, {tok.span.start, previous().span.end}, tok.text, std::move(arguments),
                         Type::Unknown, {}, {}, ArrayAllocation::None, false, false, {}, true}, height};
            }
        }
        std::string baseName;
        if (!atQualifiedType() && (at("Option") || at("Result"))) {
            baseName = "@" + typeNameToken().text;
        } else {
            baseName = nominal();
        }
        Expr value{ExprKind::Construct, start, std::move(baseName), {}};
        unsigned height = 1;
        if (at("<")) value.typeArgs = parseTypeArgs();
        if (take(".")) {
            if (current().kind == TokenKind::Word && !atTypeName()) {
                auto method = name();
                need("(");
                auto arguments = args(depth, height);
                Type targetT = value.typeArgs.empty() ? intern(value.text) : typeApp(value.text, value.typeArgs);
                Expr assoc{ExprKind::AssocCall, {start.start, previous().span.end}, std::move(method.text), std::move(arguments)};
                assoc.assocType = targetT;
                return {std::move(assoc), height};
            }
            value.isEnumConstruct = true;
            value.text += "." + typeNameToken().text;
            if (take("(")) value.args = args(depth, height);
        } else {
            need("{");
            newlines();
            if (!take("}")) {
                do {
                    newlines();
                    if (at("}")) break;
                    value.labels.push_back(name().text);
                    need(":");
                    newlines();
                    Tree field = expression(1, depth + 1);
                    height = std::max(height, field.height + 1);
                    value.args.push_back(std::move(field.value));
                    newlines();
                } while (take(","));
                newlines();
                need("}");
            }
        }
        value.span.end = previous().span.end;
        return {std::move(value), height};
    }

    static void shiftSpan(Span& sp, std::size_t delta, std::size_t file) {
        sp.start += delta;
        sp.end += delta;
        sp.file = file;
    }
    static void shiftStmt(Stmt& s, std::size_t delta, std::size_t file) {
        shiftSpan(s.span, delta, file);
        for (auto& e : s.exprs) shiftExpr(e, delta, file);
        for (auto& b : s.body) shiftStmt(b, delta, file);
        for (auto& o : s.otherwise) shiftStmt(o, delta, file);
        for (auto& a : s.arms) {
            shiftSpan(a.span, delta, file);
            for (auto& b : a.body) shiftStmt(b, delta, file);
            if (a.resultExpr) shiftExpr(*a.resultExpr, delta, file);
        }
    }
    static void shiftExpr(Expr& e, std::size_t delta, std::size_t file) {
        shiftSpan(e.span, delta, file);
        for (auto& arg : e.args) shiftExpr(arg, delta, file);
        if (e.closure) {
            for (auto& p : e.closure->params) shiftSpan(p.span, delta, file);
            for (auto& s : e.closure->body) shiftStmt(s, delta, file);
        }
        if (e.matchExpr) {
            shiftExpr(e.matchExpr->scrutinee, delta, file);
            for (auto& a : e.matchExpr->arms) {
                shiftSpan(a.span, delta, file);
                for (auto& b : a.body) shiftStmt(b, delta, file);
                if (a.resultExpr) shiftExpr(*a.resultExpr, delta, file);
            }
        }
        if (e.ifExpr) {
            shiftExpr(e.ifExpr->condition, delta, file);
            for (auto& s : e.ifExpr->thenBody) shiftStmt(s, delta, file);
            if (e.ifExpr->thenResult) shiftExpr(*e.ifExpr->thenResult, delta, file);
            for (auto& s : e.ifExpr->elseBody) shiftStmt(s, delta, file);
            if (e.ifExpr->elseResult) shiftExpr(*e.ifExpr->elseResult, delta, file);
        }
    }

    Tree parseInterp(const Token& tok, unsigned depth) {
        guardDepth(depth + 1, tok.span);
        Expr interp{ExprKind::Interp, tok.span, {}, {}};
        unsigned height = 1;
        std::size_t i = tok.span.start + 1;
        const std::size_t end = tok.span.end - 1;
        std::size_t chunkStart = i;
        std::string chunk;
        while (i < end) {
            const char ch = source[i++];
            if (ch == '\\') {
                char esc = source[i++];
                switch (esc) {
                    case 'n': esc = '\n'; break;
                    case 'r': esc = '\r'; break;
                    case 't': esc = '\t'; break;
                    default: break;
                }
                chunk += esc;
            } else if (ch == '{') {
                if (i < end && source[i] == '{') {
                    ++i;
                    chunk += '{';
                    continue;
                }
                if (!chunk.empty()) {
                    interp.args.push_back({ExprKind::String, {chunkStart, i - 1, tok.span.file}, std::move(chunk), {}});
                    chunk.clear();
                }
                const std::size_t exprStart = i;
                int bDepth = 1;
                while (i < end && bDepth > 0) {
                    char ec = source[i++];
                    if (ec == '{') ++bDepth;
                    else if (ec == '}') --bDepth;
                    else if (ec == '"') {
                        bool dummyI = false;
                        std::string dummyP;
                        scanStringToken(source, i, i - 1, dummyI, dummyP);
                    }
                }
                const std::size_t exprEnd = i - 1;
                std::string_view exprSrc = source.substr(exprStart, exprEnd - exprStart);
                try {
                    Parser sub(exprSrc, shared ? shared : &result, module, tok.span.file, genericScope);
                    Tree part = sub.parseSubExpr(depth + 1);
                    shiftExpr(part.value, exprStart, tok.span.file);
                    height = std::max(height, part.height + 1);
                    interp.args.push_back(std::move(part.value));
                } catch (Diagnostic d) {
                    shiftSpan(d.span, exprStart, tok.span.file);
                    for (auto& r : d.related) shiftSpan(r, exprStart, tok.span.file);
                    if (d.fix) shiftSpan(d.fix->span, exprStart, tok.span.file);
                    throw d;
                }
                chunkStart = i;
            } else if (ch == '}' && i < end && source[i] == '}') {
                ++i;
                chunk += '}';
            } else {
                chunk += ch;
            }
        }
        if (!chunk.empty()) {
            interp.args.push_back({ExprKind::String, {chunkStart, end, tok.span.file}, std::move(chunk), {}});
        }
        return {std::move(interp), height};
    }

    Tree primary(unsigned depth) {
        const Token first = current();
        if (take("-") || take("!") || take("~")) {
            newlines();
            if (first.text == "-" && current().kind == TokenKind::Digits) {
                auto digits = std::string_view(current().text);
                const auto nonzero = digits.find_first_not_of('0');
                if (nonzero != digits.npos && digits.substr(nonzero) == "9223372036854775808") {
                    ++pos;
                    return {{ExprKind::Integer, {first.span.start, previous().span.end}, "-9223372036854775808", {}}, 1};
                }
            }
            Tree child = expression(11, depth + 1);
            Expr node{ExprKind::Unary, {first.span.start, child.value.span.end}, first.text, {}};
            node.args.push_back(std::move(child.value));
            return {std::move(node), child.height + 1};
        }
        if (take("||") || take("|")) {
            guardDepth(depth + 1, first.span);
            ClosurePtr cl(new ClosureExpr{});
            unsigned height = 1;
            if (first.text == "|") {
                newlines();
                if (!take("|")) {
                    do {
                        newlines();
                        if (take("mut"))
                            throw Diagnostic("E_CLOSURE_PARAM",
                                             "Los parámetros de closure no admiten mut.",
                                             previous().span);
                        auto id = name();
                        Type pType = Type::Unknown;
                        if (take(":")) {
                            newlines();
                            pType = type();
                        }
                        cl->params.push_back({id.text, pType, {id.span.start, previous().span.end}, false, false});
                        newlines();
                    } while (take(","));
                    newlines();
                    need("|");
                }
            }
            if (take("->")) {
                newlines();
                cl->result = type();
            }
            if (at("{")) {
                cl->body = block(depth + 1);
                cl->isExprBody = false;
                height = 2;
            } else {
                newlines();
                Tree bodyExpr = expression(1, depth + 1);
                height = std::max(height, bodyExpr.height + 1);
                Stmt retStmt{};
                retStmt.kind = StmtKind::Return;
                retStmt.span = bodyExpr.value.span;
                retStmt.exprs.push_back(std::move(bodyExpr.value));
                cl->body.push_back(std::move(retStmt));
                cl->isExprBody = true;
            }
            Expr node{ExprKind::Closure, {first.span.start, previous().span.end}, {}, {}};
            node.closure = std::move(cl);
            return {std::move(node), height};
        }
        if (at("fn")) {
            Diagnostic d("E_CLOSURE_SYNTAX",
                         "En Turi las closures usan |x: int| -> int { ... }, no fn(...) { ... }.",
                         first.span);
            d.example = "|x: int| x + 1";
            throw d;
        }
        if (first.kind == TokenKind::Word && first.text == "lambda" &&
            pos + 1 < tokens.size() && (tokens[pos + 1].kind == TokenKind::Word || tokens[pos + 1].text == ":")) {
            Diagnostic d("E_CLOSURE_SYNTAX",
                         "En Turi las closures usan |x| x + 1, no lambda.",
                         first.span);
            d.example = "|x| x + 1";
            throw d;
        }
        if (first.kind == TokenKind::Word && first.text == "await" &&
            pos + 1 < tokens.size() && tokens[pos + 1].kind == TokenKind::Word) {
            Diagnostic d("E_CONC_NAME",
                         "Turi no usa await; espera el resultado de una tarea con taskJoin(t).",
                         first.span);
            d.example = "let r = taskJoin(t)";
            throw d;
        }
        if (take("(")) {
            newlines();
            if (at(")") && pos + 1 < tokens.size() && tokens[pos + 1].text == "=>") {
                Diagnostic d("E_CLOSURE_SYNTAX",
                             "En Turi las closures usan || expr o |x| expr, no =>.",
                             first.span);
                d.example = "|| 42";
                throw d;
            }
            Tree inside = expression(1, depth + 1);
            newlines();
            if (at(",")) {
                // (a, b) construye una tupla con tuple2(a, b) de la biblioteca base.
                Expr tuple{ExprKind::Call, first.span, {}, {}};
                unsigned height = inside.height;
                tuple.args.push_back(std::move(inside.value));
                while (take(",")) {
                    newlines();
                    if (at(")")) break;
                    Tree element = expression(1, depth + 1);
                    height = std::max(height, element.height);
                    tuple.args.push_back(std::move(element.value));
                    newlines();
                }
                need(")");
                tuple.span = {first.span.start, previous().span.end, first.span.file};
                if (tuple.args.size() < 2 || tuple.args.size() > 4)
                    throw Diagnostic("E_TUPLE", "Una tupla tiene de 2 a 4 elementos; usa un struct para más.", tuple.span);
                tuple.text = "tuple" + std::to_string(tuple.args.size());
                return {std::move(tuple), height + 1};
            }
            need(")");
            inside.value.span = {first.span.start, previous().span.end};
            return inside;
        }
        if (take("[")) {
            Expr arr{ExprKind::ArrayLit, first.span, {}, {}};
            unsigned height = 1;
            newlines();
            if (!take("]")) {
                do {
                    newlines();
                    if (at("]")) break;
                    Tree elem = expression(1, depth + 1);
                    height = std::max(height, elem.height + 1);
                    arr.args.push_back(std::move(elem.value));
                    newlines();
                } while (take(","));
                newlines();
                need("]");
            }
            arr.span.end = previous().span.end;
            return {std::move(arr), height};
        }
        if (first.kind == TokenKind::Interp) {
            ++pos;
            return parseInterp(first, depth);
        }
        if (first.kind == TokenKind::Digits || first.kind == TokenKind::Float ||
            first.kind == TokenKind::Quoted || at("true") || at("false")) {
            ++pos;
            auto kind = first.kind == TokenKind::Digits ? ExprKind::Integer :
                        first.kind == TokenKind::Float ? ExprKind::Float :
                        first.kind == TokenKind::Quoted ? ExprKind::String : ExprKind::Boolean;
            Expr literal{kind, first.span, first.text, {}};
            // Un literal de carácter recuerda su carácter: donde se espera un str vale como str.
            if (first.isChar) literal.labels.push_back(std::string(1, static_cast<char>(std::stoi(first.text))));
            return {std::move(literal), 1};
        }
        if (take("if")) {
            return parseIfExpr(depth, first.span.start);
        }
        if (take("match")) {
            guardDepth(depth + 1, first.span);
            Tree scrutinee = expression(1, depth + 1);
            MatchPtr mp(new MatchExpr{});
            mp->scrutinee = std::move(scrutinee.value);
            unsigned height = scrutinee.height + 1;
            need("{");
            newlines();
            while (!take("}")) {
                const auto armStart = current().span;
                MatchArm arm = matchArmHeader();
                newlines();
                if (at("{")) {
                    arm.body = exprBlock(depth + 1, arm.resultExpr, height);
                } else if (at("return") || at("break") || at("continue")) {
                    arm.body.push_back(statement(depth + 1));
                } else {
                    Tree val = expression(1, depth + 1);
                    height = std::max(height, val.height + 1);
                    arm.resultExpr = std::move(val.value);
                }
                arm.span = {armStart.start, previous().span.end};
                mp->arms.push_back(std::move(arm));
                const bool sepComma = take(",");
                const bool sepNl = newlines();
                if (!at("}") && !sepComma && !sepNl) expected("salto de línea, , o }");
            }
            Expr node{ExprKind::Match, {first.span.start, previous().span.end}, {}, {}};
            node.matchExpr = std::move(mp);
            return {std::move(node), height};
        }
        if (first.kind == TokenKind::Word && !reserved(first.text) && externCSymbols.contains(first.text)) {
            auto id = tokens[pos++];
            if (!take("(")) return {{ExprKind::Name, id.span, id.text, {}}, 1};
            unsigned height = 1;
            auto arguments = args(depth, height);
            return {{ExprKind::Call, {id.span.start, previous().span.end}, id.text, std::move(arguments)}, height};
        }
        if (atTypeName() || (atQualifiedType() && !(pos + 3 < tokens.size() && tokens[pos + 3].text == "(")))
            return constructor(depth);
        if ((at("int") || at("float") || at("str") ||
             at("i8") || at("i16") || at("i32") || at("i64") ||
             at("u8") || at("u16") || at("u32") || at("u64") ||
             at("f32") || at("f64")) &&
            pos + 1 < tokens.size() && tokens[pos + 1].text == "(") {
            auto id = tokens[pos++];
            need("(");
            unsigned height = 1;
            auto arguments = args(depth, height);
            return {{ExprKind::Call, {id.span.start, previous().span.end}, id.text, std::move(arguments)}, height};
        }
        if (first.kind == TokenKind::Word && first.text == "f" && pos + 1 < tokens.size() &&
            (tokens[pos + 1].kind == TokenKind::Quoted || tokens[pos + 1].kind == TokenKind::Interp) &&
            first.span.end == tokens[pos + 1].span.start) {
            suggest("E_FSTRING", "En Turi las cadenas con comillas dobles ya interpolan sin prefijo f.",
                    first.span, "", "\"x = {x}\"");
        }
        auto id = name();
        std::vector<Type> callTargs;
        if (at("<") && looksLikeGenericCall()) callTargs = parseTypeArgs();
        if (!take("("))
            return {{ExprKind::Name, id.span, id.text, {}}, 1};
        unsigned height = 1;
        auto arguments = args(depth, height);
        Expr callExpr{ExprKind::Call, {id.span.start, previous().span.end}, id.text, std::move(arguments)};
        callExpr.typeArgs = std::move(callTargs);
        return {std::move(callExpr), height};
    }

    // `a..=b` se baja a `a..b + 1`.
    static Tree plusOne(Tree upper) {
        const Span span = upper.value.span;
        Expr sum{ExprKind::Binary, span, "+", {}};
        sum.args.push_back(std::move(upper.value));
        sum.args.push_back(Expr{ExprKind::Integer, {span.end, span.end, span.file}, "1", {}});
        return {std::move(sum), upper.height + 1};
    }
    // Tras `?`, ¿empieza otro operando en la misma línea? Entonces es el ternario `c ? a : b` de
    // C/JS/Java; el operador de propagación `x?` nunca va seguido de un operando.
    bool ternaryFollows() const {
        if (!at("?") || pos + 1 >= tokens.size()) return false;
        const Token& n = tokens[pos + 1];
        if (n.kind == TokenKind::Digits || n.kind == TokenKind::Float || n.kind == TokenKind::Quoted ||
            n.kind == TokenKind::Interp) return true;
        if (n.kind == TokenKind::Word) return n.text != "else" && n.text != "in";
        return n.kind == TokenKind::Symbol && (n.text == "(" || n.text == "-" || n.text == "!" || n.text == "[");
    }
    Tree expression(int minimum = 1, unsigned depth = 1) {
        guardDepth(depth, current().span);
        auto left = primary(depth);
        while (true) {
            guardDepth(left.height, left.value.span);
            if (take("[")) {
                newlines();
                Tree index = expression(1, depth + 1);
                newlines();
                if (take("..") || take("..=")) {
                    const bool inclusive = previous().text == "..=";
                    newlines();
                    Tree hi = expression(1, depth + 1);
                    if (inclusive) hi = plusOne(std::move(hi));
                    newlines();
                    need("]");
                    Expr sl{ExprKind::Slice, {left.value.span.start, previous().span.end}, {}, {}};
                    sl.args.push_back(std::move(left.value));
                    sl.args.push_back(std::move(index.value));
                    sl.args.push_back(std::move(hi.value));
                    left = {std::move(sl), 1 + std::max(left.height, std::max(index.height, hi.height))};
                    continue;
                }
                need("]");
                Expr access{ExprKind::Index, {left.value.span.start, previous().span.end}, {}, {}};
                access.args.push_back(std::move(left.value));
                access.args.push_back(std::move(index.value));
                left = {std::move(access), 1 + std::max(left.height, index.height)};
                continue;
            }
            if (at(".") && pos + 1 < tokens.size() && tokens[pos + 1].kind == TokenKind::Digits &&
                !tokens[pos + 1].isChar) {
                // t.0, t.1: elementos de una tupla.
                pos += 2;
                Expr field{ExprKind::Field, {left.value.span.start, previous().span.end}, "v" + previous().text, {}};
                field.args.push_back(std::move(left.value));
                left = {std::move(field), left.height + 1};
                continue;
            }
            if (take(".")) {
                auto attr = (current().kind == TokenKind::Word &&
                             ((current().text == "str" && pos + 1 < tokens.size() && tokens[pos + 1].text == "(") ||
                              (!reserved(current().text) && isCIdent(current().text) &&
                               (isSharedExternSymbol(current().text) ||
                                (pos + 1 < tokens.size() && tokens[pos + 1].text == "(")))))
                    ? tokens[pos++]
                    : name();
                auto base = left.value.span;
                std::vector<Type> mTargs;
                if (at("<") && looksLikeGenericCall()) mTargs = parseTypeArgs();
                if (take("(")) {
                    unsigned callHeight = left.height;
                    auto callArgs = args(depth, callHeight);
                    Expr mc{ExprKind::MethodCall, {base.start, previous().span.end}, std::move(attr.text), {}};
                    mc.typeArgs = std::move(mTargs);
                    mc.args.reserve(1 + callArgs.size());
                    mc.args.push_back(std::move(left.value));
                    for (auto& a : callArgs) mc.args.push_back(std::move(a));
                    left = {std::move(mc), std::max(left.height + 1, callHeight)};
                } else {
                    Expr field{ExprKind::Field, {base.start, attr.span.end}, std::move(attr.text), {}};
                    field.args.push_back(std::move(left.value));
                    left = {std::move(field), left.height + 1};
                }
                continue;
            }
            if (take("(")) {
                unsigned callHeight = left.height;
                auto callArgs = args(depth, callHeight);
                Expr cc{ExprKind::ClosureCall, {left.value.span.start, previous().span.end}, {}, {}};
                cc.args.reserve(1 + callArgs.size());
                cc.args.push_back(std::move(left.value));
                for (auto& a : callArgs) cc.args.push_back(std::move(a));
                left = {std::move(cc), std::max(left.height + 1, callHeight)};
                continue;
            }
            if (ternaryFollows()) {
                // Lectura especulativa: solo es ternario si tras el operando aparece `:`.
                const auto saved = mark();
                std::optional<std::pair<Tree, Tree>> arms;
                try {
                    ++pos;
                    Tree yes = expression(1, depth + 1);
                    newlines();
                    if (take(":")) {
                        newlines();
                        Tree no = expression(1, depth + 1);
                        arms.emplace(std::move(yes), std::move(no));
                    }
                } catch (const Diagnostic&) {
                    arms.reset();
                }
                if (arms && minimum > 1) {
                    rewind(saved); // lo construye el nivel exterior, con la condición completa
                    return left;
                }
                if (arms) {
                    auto& [yes, no] = *arms;
                    const Span span{left.value.span.start, no.value.span.end, left.value.span.file};
                    const unsigned height = 1 + std::max(left.height, std::max(yes.height, no.height));
                    IfPtr node(new IfExpr{std::move(left.value), {}, std::move(yes.value), {}, std::move(no.value)});
                    Expr res{ExprKind::If, span, {}, {}};
                    res.ifExpr = std::move(node);
                    left = {std::move(res), height};
                    continue;
                }
                rewind(saved);
            }
            if (take("?")) {
                Expr tr{ExprKind::Try, {left.value.span.start, previous().span.end}, {}, {}};
                tr.args.push_back(std::move(left.value));
                left = {std::move(tr), left.height + 1};
                continue;
            }
            if (at("=>")) {
                Diagnostic d("E_CLOSURE_SYNTAX",
                             "En Turi las closures usan |x| expr, no =>.",
                             current().span);
                d.example = "|x| x + 1";
                throw d;
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

    // `let (a, b) = e` se parte en varias sentencias: la primera la devuelve statement() y el resto
    // espera aquí hasta que el bloque las añade.
    std::vector<Stmt> pendingStmts;
    void addStatement(std::vector<Stmt>& body, unsigned depth) {
        body.push_back(statement(depth));
        for (auto& extra : pendingStmts) body.push_back(std::move(extra));
        pendingStmts.clear();
    }
    std::vector<Stmt> block(unsigned depth) {
        guardDepth(depth, current().span);
        need("{");
        newlines();
        std::vector<Stmt> body;
        while (!take("}")) {
            addStatement(body, depth);
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
        return body;
    }

    MatchArm matchArmHeader() {
        MatchArm arm;
        if (current().kind == TokenKind::Word && current().text == "_" &&
            (pos + 1 >= tokens.size() || tokens[pos + 1].text != ".")) {
            arm.variant = tokens[pos++].text;
            need("=>");
            return arm;
        }
        if (!atQualifiedType() && (at("Some") || at("None") || at("Ok") || at("Err")) &&
            (pos + 1 >= tokens.size() || tokens[pos + 1].text != ".")) {
            arm.variant = typeNameToken().text;
        } else {
            std::string type;
            if (!atQualifiedType() && (at("Option") || at("Result"))) {
                type = "@" + typeNameToken().text;
            } else {
                type = nominal();
            }
            if (at("<")) arm.typeArgs = parseTypeArgs();
            need(".");
            arm.variant = std::move(type) + "." + typeNameToken().text;
        }
        if (take("(")) {
            newlines();
            if (!take(")")) {
                do {
                    newlines();
                    if (at(")")) break;
                    arm.names.push_back(bindingName().text);
                    newlines();
                } while (take(","));
                newlines();
                need(")");
            }
        }
        need("=>");
        return arm;
    }

    Tree parseIfExpr(unsigned depth, std::size_t startByte) {
        guardDepth(depth + 1, {startByte, startByte + 2});
        Tree cond = expression(1, depth + 1);
        unsigned height = cond.height + 1;
        IfPtr node(new IfExpr{std::move(cond.value), {}, {}, {}, {}});
        node->thenBody = exprBlock(depth + 1, node->thenResult, height);
        const auto savedElse = pos;
        newlines();
        if (!take("else")) {
            pos = savedElse;
            Diagnostic d("E_IF_EXPR", "Una expresión if requiere una rama else para devolver un valor.", {startByte, previous().span.end});
            d.example = "let x = if cond { 1 } else { 2 }";
            throw d;
        }
        newlines();
        if (take("if")) {
            Tree nestedIf = parseIfExpr(depth + 1, previous().span.start);
            height = std::max(height, nestedIf.height + 1);
            node->elseResult = std::move(nestedIf.value);
        } else {
            node->elseBody = exprBlock(depth + 1, node->elseResult, height);
        }
        Expr res{ExprKind::If, {startByte, previous().span.end}, {}, {}};
        res.ifExpr = std::move(node);
        return {std::move(res), height};
    }

    std::vector<Stmt> exprBlock(unsigned depth, std::optional<Expr>& outResult, unsigned& height) {
        guardDepth(depth, current().span);
        need("{");
        newlines();
        std::vector<Stmt> body;
        auto isKeywordStmtStart = [&]() -> bool {
            return current().kind == TokenKind::Label || at("let") || at("var") ||
                   at("while") || at("for") || at("break") || at("continue") || at("goto") ||
                   at("try") || at("throw") || at("return") || at("assert") || at("print") ||
                   ((at("trusted") || at("raw")) && pos + 1 < tokens.size() && tokens[pos + 1].text == "{");
        };
        while (!take("}")) {
            if (isKeywordStmtStart()) {
                addStatement(body, depth);
                if (!at("}") && !newlines()) expected("salto de línea o }");
                continue;
            }
            const auto savedPos = mark();
            if (at("match") || at("if")) {
                try {
                    Tree cand = expression(1, depth);
                    const auto afterExpr = pos;
                    newlines();
                    if (take("}")) {
                        outResult = std::move(cand.value);
                        height = std::max(height, cand.height + 1);
                        return body;
                    }
                    rewind(savedPos);
                    (void)afterExpr;
                } catch (const Diagnostic&) {
                    rewind(savedPos);
                }
                addStatement(body, depth);
                if (!at("}") && !newlines()) expected("salto de línea o }");
                continue;
            }
            Tree cand = expression(1, depth);
            constexpr std::array assign = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>="};
            if (current().kind == TokenKind::Symbol && contains(assign, current().text)) {
                rewind(savedPos);
                addStatement(body, depth);
                if (!at("}") && !newlines()) expected("salto de línea o }");
                continue;
            }
            newlines();
            if (take("}")) {
                outResult = std::move(cand.value);
                height = std::max(height, cand.height + 1);
                return body;
            }
            rewind(savedPos);
            addStatement(body, depth);
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
        return body;
    }

    // ¿La expresión produce un valor en todas sus ramas? (un `if`/`match` usado como sentencia no).
    static bool yieldsValue(const Expr& e) {
        if (e.kind == ExprKind::If) {
            if (!e.ifExpr || !e.ifExpr->thenResult || !e.ifExpr->elseResult) return false;
            return yieldsValue(*e.ifExpr->thenResult) && yieldsValue(*e.ifExpr->elseResult);
        }
        if (e.kind == ExprKind::Match) {
            if (!e.matchExpr || e.matchExpr->arms.empty()) return false;
            for (const auto& arm : e.matchExpr->arms)
                if (!arm.resultExpr || !yieldsValue(*arm.resultExpr)) return false;
            return true;
        }
        return true;
    }
    // Cuerpo de función. Si la función devuelve un valor y el cuerpo termina en una expresión suelta,
    // esa expresión es el resultado (retorno implícito de Rust, Kotlin, Ruby...): equivale a `return expr`.
    std::vector<Stmt> functionBody(bool returnsValue) {
        if (returnsValue) {
            const auto saved = mark();
            try {
                std::optional<Expr> tail;
                unsigned height = 1;
                auto body = exprBlock(1, tail, height);
                if (tail && yieldsValue(*tail)) {
                    Stmt ret{};
                    ret.kind = StmtKind::Return;
                    ret.span = tail->span;
                    ret.exprs.push_back(std::move(*tail));
                    body.push_back(std::move(ret));
                    return body;
                }
                if (!tail) return body;
            } catch (const Diagnostic&) {
            }
            rewind(saved);
        }
        return block(1);
    }

    std::vector<MatchArm> matchArms(unsigned depth) {
        need("{");
        newlines();
        std::vector<MatchArm> arms;
        while (!take("}")) {
            const auto start = current().span;
            MatchArm arm = matchArmHeader();
            newlines();
            if (at("{")) arm.body = block(depth);
            else addStatement(arm.body, depth);
            arm.span = {start.start, previous().span.end};
            arms.push_back(std::move(arm));
            const bool sepComma = take(",");
            const bool sepNl = newlines();
            if (!at("}") && !sepComma && !sepNl) expected("salto de línea, , o }");
        }
        return arms;
    }

    Stmt forStatement(unsigned depth, std::size_t start, std::string label) {
        Stmt stmt{};
        stmt.label = std::move(label);
        stmt.name = bindingName().text;
        need("in");
        stmt.exprs.push_back(expression().value);
        if (take("..") || take("..=")) {
            const bool inclusive = previous().text == "..=";
            stmt.kind = StmtKind::For;
            newlines();
            Tree upper = expression();
            if (inclusive) upper = plusOne(std::move(upper));
            stmt.exprs.push_back(std::move(upper.value));
        } else {
            stmt.kind = StmtKind::ForEach;
        }
        stmt.body = block(depth + 1);
        stmt.span = {start, previous().span.end};
        return stmt;
    }

    Stmt statement(unsigned depth) {
        guardDepth(depth, current().span);
        const auto start = current().span.start;
        Stmt stmt{};
        if (current().kind == TokenKind::Label) {
            auto lbl = tokens[pos++].text;
            need(":");
            newlines();
            if (take("while")) {
                stmt.label = std::move(lbl);
                stmt.kind = StmtKind::While;
                stmt.exprs.push_back(expression().value);
                stmt.body = block(depth + 1);
                stmt.span = {start, previous().span.end};
                return stmt;
            } else if (take("for")) {
                return forStatement(depth, start, std::move(lbl));
            } else {
                expected("while o for después de la etiqueta");
            }
        }
        if (take("let") || take("var")) {
            stmt.kind = previous().text == "let" ? StmtKind::Let : StmtKind::Var;
            if (stmt.kind == StmtKind::Let && at("mut"))
                suggest("E_LET_MUT", "Declara una variable mutable con var.", {start, current().span.end},
                        "var", "var n = 1");
            if (at("(")) {
                // let (a, b) = e   ->   let tup = e; let a = tup.v0; let b = tup.v1
                ++pos;
                std::vector<Token> names;
                do {
                    newlines();
                    if (at(")")) break;
                    names.push_back(bindingName());
                    newlines();
                } while (take(","));
                need(")");
                if (names.size() < 2 || names.size() > 4)
                    throw Diagnostic("E_TUPLE", "Una tupla tiene de 2 a 4 elementos; usa un struct para más.", previous().span);
                need("=");
                newlines();
                const StmtKind kind = stmt.kind;
                const std::string holder = "tup__" + std::to_string(start);
                stmt.kind = StmtKind::Let;
                stmt.name = holder;
                stmt.exprs.push_back(expression().value);
                stmt.span = {start, previous().span.end};
                for (std::size_t k = 0; k < names.size(); ++k) {
                    if (names[k].text == "_") continue;
                    Stmt part{};
                    part.kind = kind;
                    part.span = names[k].span;
                    part.name = names[k].text;
                    Expr field{ExprKind::Field, names[k].span, "v" + std::to_string(k), {}};
                    field.args.push_back(Expr{ExprKind::Name, names[k].span, holder, {}});
                    part.exprs.push_back(std::move(field));
                    pendingStmts.push_back(std::move(part));
                }
                return stmt;
            }
            stmt.name = (stmt.kind == StmtKind::Let ? bindingName() : name()).text;
            if (take(":")) stmt.annotation = type();
            need("=");
            newlines();
            stmt.exprs.push_back(expression().value);
        } else if (take("if") || take("while")) {
            stmt.kind = previous().text == "if" ? StmtKind::If : StmtKind::While;
            stmt.exprs.push_back(expression().value);
            stmt.body = block(depth + 1);
            if (stmt.kind == StmtKind::If) {
                const auto savedElse = pos;
                newlines();
                if (take("else")) {
                    if (at("if")) stmt.otherwise.push_back(statement(depth + 1));
                    else stmt.otherwise = block(depth + 1);
                } else {
                    pos = savedElse;
                }
            }
        } else if ((at("trusted") || at("raw")) && pos + 1 < tokens.size() && tokens[pos + 1].text == "{") {
            const auto kw = tokens[pos++].text;
            stmt.kind = StmtKind::SafetyBlock;
            stmt.safety = (kw == "raw") ? SafetyTier::Raw : SafetyTier::Trusted;
            stmt.body = block(depth + 1);
        } else if (take("match")) {
            stmt.kind = StmtKind::Match;
            stmt.exprs.push_back(expression().value);
            stmt.arms = matchArms(depth + 1);
        } else if (take("for")) {
            return forStatement(depth, start, {});
        } else if (take("break") || take("continue")) {
            stmt.kind = previous().text == "break" ? StmtKind::Break : StmtKind::Continue;
            if (current().kind == TokenKind::Label) stmt.label = tokens[pos++].text;
            else if (!at("}") && !at(",") && current().kind != TokenKind::Newline && current().kind != TokenKind::End)
                expected("'etiqueta o salto de línea");
        } else if (at("goto")) {
            Diagnostic d("E_GOTO", "Turi no usa goto; usa break 'etiqueta o continue 'etiqueta.", current().span);
            d.example = "'outer: for i in 0..n { if i == 3 { break 'outer } }";
            throw d;
        } else if (at("try") && pos + 1 < tokens.size() && tokens[pos + 1].text == "{") {
            Diagnostic d("E_EXCEPTIONS", "Turi no usa try/catch; usa Result<T, E>, match y el operador ?.", current().span);
            d.example = "let v = parse(s)?";
            throw d;
        } else if (at("throw") && pos + 1 < tokens.size() &&
                   tokens[pos + 1].text != "(" && tokens[pos + 1].text != "." && tokens[pos + 1].text != "[" &&
                   !contains(std::array{"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>="},
                             std::string_view(tokens[pos + 1].text))) {
            Diagnostic d("E_EXCEPTIONS", "Turi no usa excepciones; devuelve Result<T, E> con Err(...) y propaga con ?.", current().span);
            d.example = "return Err(miError)";
            throw d;
        } else if (take("return")) {
            stmt.kind = StmtKind::Return;
            if (!at("}") && !at(",") && current().kind != TokenKind::Newline && current().kind != TokenKind::End)
                stmt.exprs.push_back(expression().value);
        } else if (take("assert")) {
            stmt.kind = StmtKind::Assert;
            stmt.exprs.push_back(expression().value);
        } else if (take("print")) {
            stmt.kind = StmtKind::Print;
            need("(");
            newlines();
            stmt.exprs.push_back(expression().value);
            newlines();
            if (take(",")) newlines();
            need(")");
        } else {
            auto lhs = expression(1, 1);
            constexpr std::array assign = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=", ">>>="};
            if (current().kind == TokenKind::Symbol && contains(assign, current().text)) {
                auto op = tokens[pos++].text;
                newlines();
                auto rhs = expression();
                auto extractFieldChain = [](const Expr* node, std::vector<std::string>& outFields) -> std::optional<std::string> {
                    std::vector<std::string> rev;
                    while (node->kind == ExprKind::Field) {
                        rev.push_back(node->text);
                        node = &node->args.at(0);
                    }
                    if (node->kind != ExprKind::Name) return std::nullopt;
                    outFields.assign(rev.rbegin(), rev.rend());
                    return node->text;
                };
                auto repeatableIndex = [](auto& self, const Expr& node) -> bool {
                    switch (node.kind) {
                    case ExprKind::Integer:
                    case ExprKind::String:
                    case ExprKind::Boolean:
                    case ExprKind::Name:
                        return true;
                    case ExprKind::Unary:
                    case ExprKind::Binary:
                    case ExprKind::Field:
                    case ExprKind::Index:
                    case ExprKind::Slice:
                        for (const auto& arg : node.args)
                            if (!self(self, arg)) return false;
                        return true;
                    default:
                        return false;
                    }
                };
                auto extractPlaceChain = [](auto& self, const Expr* node, std::string& outRoot,
                                            std::vector<LValueStep>& outSteps, std::vector<Expr>& outIndices) -> bool {
                    if (node->kind == ExprKind::Name) {
                        outRoot = node->text;
                        return true;
                    }
                    if (node->kind == ExprKind::Field) {
                        if (!self(self, &node->args.at(0), outRoot, outSteps, outIndices)) return false;
                        outSteps.push_back({false, node->text, 0, Type::Unknown});
                        return true;
                    }
                    if (node->kind == ExprKind::Index) {
                        if (!self(self, &node->args.at(0), outRoot, outSteps, outIndices)) return false;
                        outSteps.push_back({true, {}, 0, Type::Unknown});
                        outIndices.push_back(node->args.at(1));
                        return true;
                    }
                    return false;
                };
                if (lhs.value.kind == ExprKind::Name) {
                    if (op.size() >= 2) {
                        guardDepth(rhs.height + 1, lhs.value.span);
                        Expr binary{ExprKind::Binary, {start, rhs.value.span.end}, op.substr(0, op.size() - 1), {}};
                        binary.args.push_back(std::move(lhs.value));
                        binary.args.push_back(std::move(rhs.value));
                        rhs.value = std::move(binary);
                    } else {
                        stmt.name = lhs.value.text;
                    }
                    if (stmt.name.empty()) stmt.name = rhs.value.args[0].text;
                    stmt.kind = StmtKind::Assign;
                    stmt.exprs.push_back(std::move(rhs.value));
                } else if (lhs.value.kind == ExprKind::Index && op == "=") {
                    std::vector<std::string> fields;
                    if (auto root = extractFieldChain(&lhs.value.args[0], fields)) {
                        if (fields.empty()) {
                            stmt.kind = StmtKind::IndexSet;
                            stmt.name = *root;
                            stmt.exprs.push_back(std::move(lhs.value.args[1]));
                            stmt.exprs.push_back(std::move(rhs.value));
                        } else {
                            stmt.kind = StmtKind::FieldSet;
                            stmt.name = *root;
                            stmt.labels = std::move(fields);
                            stmt.exprs.push_back(std::move(lhs.value.args[1]));
                            stmt.exprs.push_back(std::move(rhs.value));
                        }
                    } else {
                        std::string rootName;
                        std::vector<LValueStep> steps;
                        std::vector<Expr> idxExprs;
                        if (!extractPlaceChain(extractPlaceChain, &lhs.value, rootName, steps, idxExprs))
                            expected("objetivo de asignación válido");
                        stmt.kind = StmtKind::PlaceSet;
                        stmt.name = std::move(rootName);
                        stmt.placeSteps = std::move(steps);
                        stmt.exprs = std::move(idxExprs);
                        stmt.exprs.push_back(std::move(rhs.value));
                    }
                } else if (std::vector<std::string> chain;
                           lhs.value.kind == ExprKind::Index && op.size() >= 2 &&
                           repeatableIndex(repeatableIndex, lhs.value.args[1]) &&
                           extractFieldChain(&lhs.value.args[0], chain)) {
                    // `a[i] op= v` con índice sin efectos es exactamente `a[i] = a[i] op v`. Se baja por
                    // IndexSet/FieldSet para que las pruebas de rango y la vectorización lo cubran.
                    guardDepth(std::max(lhs.height, rhs.height) + 1, lhs.value.span);
                    std::string rootName = *extractFieldChain(&lhs.value.args[0], chain);
                    Expr index = lhs.value.args[1];
                    Expr binary{ExprKind::Binary, {start, rhs.value.span.end}, op.substr(0, op.size() - 1), {}};
                    binary.args.push_back(std::move(lhs.value));
                    binary.args.push_back(std::move(rhs.value));
                    stmt.kind = chain.empty() ? StmtKind::IndexSet : StmtKind::FieldSet;
                    stmt.name = std::move(rootName);
                    if (!chain.empty()) stmt.labels = std::move(chain);
                    stmt.exprs.push_back(std::move(index));
                    stmt.exprs.push_back(std::move(binary));
                } else {
                    std::vector<std::string> fields;
                    if (auto root = extractFieldChain(&lhs.value, fields); root && !fields.empty()) {
                        stmt.kind = StmtKind::FieldSet;
                        stmt.name = *root;
                        stmt.labels = std::move(fields);
                        if (op.size() >= 2) {
                            guardDepth(std::max(lhs.height, rhs.height) + 1, lhs.value.span);
                            Expr binary{ExprKind::Binary, {start, rhs.value.span.end}, op.substr(0, op.size() - 1), {}};
                            binary.args.push_back(std::move(lhs.value));
                            binary.args.push_back(std::move(rhs.value));
                            rhs.value = std::move(binary);
                        }
                        stmt.exprs.push_back(std::move(rhs.value));
                    } else {
                        std::string rootName;
                        std::vector<LValueStep> steps;
                        std::vector<Expr> idxExprs;
                        if (!extractPlaceChain(extractPlaceChain, &lhs.value, rootName, steps, idxExprs) || steps.empty())
                            expected("objetivo de asignación válido");
                        if (op.size() >= 2) {
                            guardDepth(std::max(lhs.height, rhs.height) + 1, lhs.value.span);
                            Expr binary{ExprKind::Binary, {start, rhs.value.span.end}, op.substr(0, op.size() - 1), {}};
                            binary.args.push_back(std::move(lhs.value));
                            binary.args.push_back(std::move(rhs.value));
                            rhs.value = std::move(binary);
                        }
                        stmt.kind = StmtKind::PlaceSet;
                        stmt.name = std::move(rootName);
                        stmt.placeSteps = std::move(steps);
                        stmt.exprs = std::move(idxExprs);
                        stmt.exprs.push_back(std::move(rhs.value));
                    }
                }
            } else {
                const Expr* core = &lhs.value;
                while (core->kind == ExprKind::Try && !core->args.empty()) core = &core->args[0];
                if (core->kind == ExprKind::Call || core->kind == ExprKind::MethodCall || core->kind == ExprKind::AssocCall) {
                    stmt.kind = StmtKind::Call;
                    stmt.exprs.push_back(std::move(lhs.value));
                } else {
                    expected("asignación o llamada");
                }
            }
        }
        stmt.span = {start, previous().span.end};
        return stmt;
    }

    ParsedAttrs parseAttributes() {
        ParsedAttrs attrs;
        while (at("@")) {
            const auto attrStart = current().span.start;
            if (!attrs.any) attrs.start = attrStart;
            attrs.any = true;
            ++pos;
            if (current().kind != TokenKind::Word) {
                Diagnostic d("E_ATTR", "Se esperaba un nombre de atributo tras '@'.", current().span);
                d.example = "@repr(C)";
                throw d;
            }
            const auto attrTok = tokens[pos++];
            if (attrTok.text == "repr") {
                need("(");
                newlines();
                if (current().kind != TokenKind::Word || current().text != "C") {
                    Diagnostic d("E_ATTR", "Solo se admite @repr(C).", current().span);
                    d.example = "@repr(C)";
                    throw d;
                }
                ++pos;
                newlines();
                need(")");
                if (attrs.reprC) {
                    Diagnostic d("E_ATTR", "Atributo @repr(C) duplicado.", attrTok.span);
                    d.example = "@repr(C)";
                    throw d;
                }
                attrs.reprC = true;
            } else if (attrTok.text == "packed") {
                if (at("(")) {
                    Diagnostic d("E_ATTR", "@packed no admite argumentos.", current().span);
                    d.example = "@packed";
                    throw d;
                }
                if (attrs.packed) {
                    Diagnostic d("E_ATTR", "Atributo @packed duplicado.", attrTok.span);
                    d.example = "@packed";
                    throw d;
                }
                attrs.packed = true;
            } else if (attrTok.text == "align") {
                need("(");
                newlines();
                if (current().kind != TokenKind::Digits) {
                    Diagnostic d("E_ATTR", "@align requiere una potencia de 2 entera entre 1 y 4096.", current().span);
                    d.example = "@align(16)";
                    throw d;
                }
                const auto numTok = tokens[pos++];
                std::uint64_t val = 0;
                const auto end = numTok.text.data() + numTok.text.size();
                const auto [ptr, ec] = std::from_chars(numTok.text.data(), end, val, 10);
                if (ec != std::errc{} || ptr != end || val < 1 || val > 4096 || (val & (val - 1)) != 0) {
                    Diagnostic d("E_ATTR", "@align requiere una potencia de 2 entre 1 y 4096.", numTok.span);
                    d.example = "@align(16)";
                    throw d;
                }
                newlines();
                need(")");
                if (attrs.explicitAlign != 0) {
                    Diagnostic d("E_ATTR", "Atributo @align duplicado.", attrTok.span);
                    d.example = "@align(16)";
                    throw d;
                }
                attrs.explicitAlign = static_cast<std::size_t>(val);
            } else if (attrTok.text == "link") {
                need("(");
                newlines();
                if (current().kind != TokenKind::Quoted || tokens[pos].text.empty() ||
                    tokens[pos].text.find('\0') != std::string::npos) {
                    Diagnostic d("E_ATTR", "@link requiere un nombre de biblioteca o ruta no vacía entre comillas.", current().span);
                    d.example = "@link(\"user32\")";
                    throw d;
                }
                attrs.linkLibs.push_back(tokens[pos++].text);
                newlines();
                need(")");
            } else if (attrTok.text == "noCopy" || attrTok.text == "arena" || attrTok.text == "compact") {
                if (at("(")) {
                    Diagnostic d("E_ATTR", "@" + attrTok.text + " no admite argumentos.", current().span);
                    d.example = "@" + attrTok.text;
                    throw d;
                }
                bool* flag = attrTok.text == "noCopy" ? &attrs.noCopy
                           : (attrTok.text == "arena" ? &attrs.arenaHint : &attrs.compactHint);
                if (*flag) {
                    Diagnostic d("E_ATTR", "Atributo @" + attrTok.text + " duplicado.", attrTok.span);
                    d.example = "@" + attrTok.text;
                    throw d;
                }
                *flag = true;
            } else {
                Diagnostic d("E_ATTR", "Atributo desconocido: @" + attrTok.text, attrTok.span);
                d.example = "@repr(C)";
                throw d;
            }
            if (!newlines()) expected("salto de línea después del atributo");
        }
        return attrs;
    }

    DataDecl dataDeclaration() { return dataDeclaration(ParsedAttrs{}); }
    DataDecl dataDeclaration(const ParsedAttrs& attrs) {
        const auto start = attrs.any ? attrs.start : current().span.start;
        bool enumeration = take("enum");
        if (!enumeration) need("struct");
        if (enumeration && (attrs.reprC || attrs.packed)) {
            Diagnostic d("E_ATTR", "@repr(C) y @packed solo aplican a struct.", current().span);
            d.example = "@repr(C)\nstruct Pair { a: u8, b: i64 }";
            throw d;
        }
        if (attrs.packed && attrs.explicitAlign > 1) {
            Diagnostic d("E_ATTR", "No se puede combinar @packed con @align mayor que 1.", current().span);
            d.example = "@packed\nstruct Header { a: u8, b: u32 }";
            throw d;
        }
        auto id = typeNameToken();
        std::vector<Type> declGenerics;
        if (take("<")) {
            do {
                newlines();
                auto genericId = typeNameToken();
                auto g = intern(genericId.text);
                if (std::find(declGenerics.begin(), declGenerics.end(), g) != declGenerics.end())
                    throw Diagnostic("E_DUP_GENERIC", "Parámetro genérico repetido.", genericId.span);
                declGenerics.push_back(g);
                genericScope.push_back(genericId.text);
            } while (take(","));
            consumeCloseAngle();
        }
        DataDecl data{intern(qualifyType(id.text)), enumeration, {}, {}, module};
        data.generics = std::move(declGenerics);
        data.reprC = attrs.reprC;
        data.packed = attrs.packed;
        data.explicitAlign = attrs.explicitAlign;
        if (!enumeration) data.variants.push_back({id.text, id.span, {}});
        need("{");
        newlines();
        while (!take("}")) {
            if (enumeration) {
                auto variant = typeNameToken();
                std::vector<Param> fields;
                std::optional<Expr> enumVal;
                if (take("(")) {
                    fields = parameters();
                    if (at("="))
                        throw Diagnostic("E_ENUM_VALUE", "Las variantes con campos no admiten valor entero.", current().span);
                } else if (take("=")) {
                    data.hasExplicitEnumValues = true;
                    newlines();
                    enumVal = expression().value;
                }
                data.variants.push_back({variant.text, {variant.span.start, previous().span.end},
                                         std::move(fields), std::move(enumVal)});
            } else {
                data.variants[0].fields.push_back(parameter());
            }
            if (take(",")) newlines();
            else if (!at("}") && !newlines()) expected("coma, salto de línea o }");
        }
        data.span = {start, previous().span.end};
        genericScope.clear();
        return data;
    }

    ConstDecl constDeclaration() {
        const auto start = current().span.start;
        need("const");
        auto id = name();
        if (!take(":")) {
            Diagnostic d("E_CONST_TYPE", "Las constantes globales requieren tipo explícito.", id.span);
            d.example = "const maxItems: int = 100";
            throw d;
        }
        newlines();
        const Type ty = type();
        need("=");
        newlines();
        auto init = expression().value;
        return {qualifyFunction(id.text), id.text, module, ty, std::move(init), {start, previous().span.end}};
    }

    static bool isEffectModifier(std::string_view w) {
        return w == "pure" || w == "noalloc" || w == "checkfree" || w == "io";
    }
    static bool isSafetyModifier(std::string_view w) {
        return w == "verified" || w == "trusted" || w == "raw";
    }
    Token externFnName() {
        if (current().kind != TokenKind::Word || reserved(current().text) || !isCIdent(current().text))
            expected("nombre de función C");
        return tokens[pos++];
    }
    void consumeExternAbi() {
        if (current().kind != TokenKind::Quoted || tokens[pos].text != "C") {
            Diagnostic d("E_FFI_ABI", "Solo se admite ABI \"C\" en declaraciones extern.", current().span);
            d.example = "extern \"C\" {\n    fn strlen(s: Ptr<u8>) -> u64\n}";
            throw d;
        }
        ++pos;
        newlines();
    }
    void externBlock(const ParsedAttrs& attrs) {
        for (const auto& lib : attrs.linkLibs) result.linkLibs.push_back(lib);
        need("extern");
        consumeExternAbi();
        need("{");
        newlines();
        while (!take("}")) {
            const auto start = current().span.start;
            if (isEffectModifier(current().text) || isSafetyModifier(current().text)) {
                Diagnostic d("E_FFI_MOD", "Las declaraciones dentro de extern \"C\" { ... } son raw por defecto y no admiten modificadores.", current().span);
                d.example = "extern \"C\" {\n    fn abs(x: i32) -> i32\n}";
                throw d;
            }
            need("fn");
            auto id = externFnName();
            if (at("<")) {
                Diagnostic d("E_FFI_GENERIC", "Las funciones extern \"C\" no admiten genéricos.", current().span);
                d.example = "extern \"C\" {\n    fn foo(x: i32) -> i32\n}";
                throw d;
            }
            need("(");
            Function fn;
            fn.isExternC = true;
            fn.isExternDecl = true;
            fn.safety = SafetyTier::Raw;
            fn.cSymbol = id.text;
            fn.sourceName = id.text;
            fn.name = qualifyFunction(id.text);
            fn.module = module;
            fn.params = parameters(true, false);
            for (const auto& p : fn.params) {
                if (p.byRef || p.writable) {
                    Diagnostic d("E_FFI_TYPE", "Los parámetros extern \"C\" no admiten mut ni var; usa Ptr<T> o MutPtr<T>.", p.span);
                    d.example = "fn fill(dst: MutPtr<u8>, n: u64)";
                    throw d;
                }
            }
            if (take("->")) {
                newlines();
                fn.result = type();
            }
            {
                const auto savedPos = pos;
                newlines();
                if (at("requires") || at("ensures")) {
                    Diagnostic d("E_CONTRACT", "Las declaraciones extern \"C\" sin cuerpo no admiten contratos requires/ensures.", current().span);
                    d.example = "extern \"C\" {\n    fn c_abs(x: i32) -> i32\n}";
                    throw d;
                }
                pos = savedPos;
            }
            if (at("{")) {
                Diagnostic d("E_FFI_BODY", "Las declaraciones dentro de extern \"C\" { ... } no llevan cuerpo; usa 'extern \"C\" fn ... { ... }' en nivel superior.", current().span);
                d.example = "extern \"C\" fn cb(x: i32) -> i32 { return x }";
                throw d;
            }
            fn.span = {start, previous().span.end};
            externCSymbols.insert(fn.cSymbol);
            result.functions.push_back(std::move(fn));
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
    }

    Function function(bool withBody = true, bool interfaceMethod = false, bool allowSelf = false, const ParsedAttrs& attrs = {}) {
        Function fn;
        const auto start = attrs.any ? attrs.start : current().span.start;
        fn.noCopy = attrs.noCopy;
        fn.arenaHint = attrs.arenaHint;
        fn.compactHint = attrs.compactHint;
        const auto outerScopeSize = genericScope.size();
        if (at("async") && pos + 1 < tokens.size() &&
            (tokens[pos + 1].text == "fn" || isEffectModifier(tokens[pos + 1].text))) {
            Diagnostic d("E_CONC_NAME",
                         "Turi no tiene funciones async/await ni funciones de color; usa taskScope(), taskSpawn(mut s, || ...) y taskJoin(t).",
                         current().span);
            d.example = "var s = taskScope()\nlet t = taskSpawn(mut s, || 42)\nlet r = taskJoin(t)";
            throw d;
        }
        Span modSpan{};
        bool hasMod = false;
        bool hasSafety = false;
        while (current().kind == TokenKind::Word &&
               (isEffectModifier(current().text) || isSafetyModifier(current().text))) {
            const auto tok = tokens[pos++];
            if (!hasMod) { modSpan = tok.span; hasMod = true; }
            else modSpan.end = tok.span.end;
            if (isSafetyModifier(tok.text)) {
                if (hasSafety) {
                    Diagnostic d("E_SAFETY", "Modificador de seguridad repetido o en conflicto: " + tok.text, tok.span);
                    d.example = "trusted fn f() {}";
                    throw d;
                }
                hasSafety = true;
                fn.safety = tok.text == "raw" ? SafetyTier::Raw
                          : (tok.text == "trusted" ? SafetyTier::Trusted : SafetyTier::Verified);
                continue;
            }
            bool* targetFlag = nullptr;
            if (tok.text == "pure") targetFlag = &fn.reqPure;
            else if (tok.text == "noalloc") targetFlag = &fn.reqNoAlloc;
            else if (tok.text == "checkfree") targetFlag = &fn.reqCheckFree;
            else if (tok.text == "io") targetFlag = &fn.reqIo;
            if (*targetFlag) {
                Diagnostic d("E_EFFECT", "Modificador de efecto repetido: " + tok.text, tok.span);
                d.example = "pure noalloc fn f(x: int) -> int { return x }";
                throw d;
            }
            *targetFlag = true;
        }
        if (fn.reqPure && fn.reqIo) {
            Diagnostic d("E_EFFECT", "Una función no puede declarar pure e io al mismo tiempo.", modSpan);
            d.example = "pure fn f(x: int) -> int { return x }";
            throw d;
        }
        if (take("extern")) {
            if (!withBody || interfaceMethod) {
                Diagnostic d("E_FFI_BODY", "extern \"C\" fn solo se permite como función de nivel superior con cuerpo.", current().span);
                throw d;
            }
            consumeExternAbi();
            fn.isExternC = true;
            fn.isExternDecl = false;
        }
        need("fn");
        auto id = fn.isExternC ? externFnName() : name();
        fn.sourceName = id.text;
        fn.name = qualifyFunction(id.text);
        if (fn.isExternC) {
            fn.cSymbol = id.text;
            externCSymbols.insert(fn.cSymbol);
        }
        fn.module = module;
        if (take("<")) {
            if (fn.isExternC) {
                Diagnostic d("E_FFI_GENERIC", "Las funciones extern \"C\" no admiten genéricos.", current().span);
                throw d;
            }
            if (interfaceMethod) throw Diagnostic("E_INTERFACE", "Los métodos de interfaz no son genéricos.", current().span);
            do {
                newlines();
                auto genericId = typeNameToken();
                auto generic = intern(genericId.text);
                if (std::find(fn.generics.begin(), fn.generics.end(), generic) != fn.generics.end())
                    throw Diagnostic("E_DUP_GENERIC", "Parámetro genérico repetido.", genericId.span);
                fn.generics.push_back(generic);
                genericScope.push_back(genericId.text);
                fn.constraints.push_back(take(":") ? type() : Type::Unknown);
            } while (take(","));
            consumeCloseAngle();
        }
        need("(");
        fn.params = parameters(true, allowSelf);
        if (fn.isExternC) {
            for (const auto& p : fn.params) {
                if (p.byRef || p.writable) {
                    Diagnostic d("E_FFI_TYPE", "Los parámetros extern \"C\" no admiten mut ni var; usa Ptr<T> o MutPtr<T>.", p.span);
                    d.example = "extern \"C\" fn cmp(a: Ptr<void>, b: Ptr<void>) -> i32 { return 0 }";
                    throw d;
                }
            }
        }
        if (take("->")) {
            newlines();
            fn.result = type();
        }
        while (true) {
            const auto savedPos = pos;
            newlines();
            if (take("requires")) {
                fn.requiresClauses.push_back(expression().value);
            } else if (take("ensures")) {
                fn.ensuresClauses.push_back(expression().value);
            } else {
                if (!withBody && !(interfaceMethod && at("{"))) pos = savedPos;
                break;
            }
        }
        fn.bodyStart = current().span.end;
        if (withBody) {
            fn.body = functionBody(fn.result != Type::Void);
        } else if (interfaceMethod && at("{")) {
            fn.hasDefaultBody = true;
            fn.body = functionBody(fn.result != Type::Void);
        }
        fn.span = {start, previous().span.end};
        genericScope.resize(outerScopeSize);
        return fn;
    }

    Function declaration() {
        if (!at("test")) return function();
        Function fn;
        const auto start = current().span.start;
        need("test");
        if (current().kind != TokenKind::Quoted) expected("nombre de prueba entre comillas");
        fn.sourceName = tokens[pos++].text;
        fn.name = qualifyFunction(fn.sourceName);
        fn.module = module;
        fn.isTest = true;
        fn.bodyStart = current().span.end;
        fn.body = block(1);
        fn.span = {start, previous().span.end};
        return fn;
    }

    InterfaceDecl interfaceDeclaration() {
        const auto start = current().span.start;
        need("interface");
        auto id = typeNameToken();
        InterfaceDecl declaration{intern(qualifyType(id.text)), {}, {}, module};
        need("{");
        newlines();
        while (!take("}")) {
            auto method = function(false, true, true);
            method.name = method.sourceName;
            declaration.methods.push_back(std::move(method));
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
        declaration.span = {start, previous().span.end};
        return declaration;
    }

    ImplDecl implDeclaration() {
        const auto start = current().span.start;
        need("impl");
        std::vector<Type> declGenerics;
        std::vector<Type> declConstraints;
        if (take("<")) {
            do {
                newlines();
                auto genericId = typeNameToken();
                auto generic = intern(genericId.text);
                if (std::find(declGenerics.begin(), declGenerics.end(), generic) != declGenerics.end())
                    throw Diagnostic("E_DUP_GENERIC", "Parámetro genérico repetido.", genericId.span);
                declGenerics.push_back(generic);
                genericScope.push_back(genericId.text);
                declConstraints.push_back(take(":") ? type() : Type::Unknown);
            } while (take(","));
            consumeCloseAngle();
        }
        Type firstType = type();
        ImplDecl declaration{Type::Unknown, firstType, {}, {}, module};
        declaration.generics = std::move(declGenerics);
        declaration.constraints = std::move(declConstraints);
        if (take("for")) {
            newlines();
            declaration.interface = firstType;
            declaration.target = type();
        }
        need("{");
        newlines();
        while (!take("}")) {
            ParsedAttrs mAttrs;
            if (at("@")) {
                mAttrs = parseAttributes();
                if (mAttrs.reprC || mAttrs.packed || mAttrs.explicitAlign != 0 || !mAttrs.linkLibs.empty()) {
                    Diagnostic d("E_ATTR", "Los métodos de impl solo admiten @noCopy, @arena y @compact.", current().span);
                    throw d;
                }
            }
            auto method = function(true, false, true, mAttrs);
            if (!method.generics.empty())
                throw Diagnostic("E_INTERFACE", "Los métodos de una impl no son genéricos.", method.span);
            method.name = method.sourceName;
            declaration.methods.push_back(std::move(method));
            if (!at("}") && !newlines()) expected("salto de línea o }");
        }
        declaration.span = {start, previous().span.end};
        genericScope.clear();
        return declaration;
    }
public:
    explicit Parser(std::string_view input) : source(input), tokens(tolerate(lex(input))) { collectExternSymbols(); }
    Parser(std::string_view input, Program& sharedProgram, std::string_view moduleName, std::size_t file)
        : source(input), tokens(tolerate(lex(input, false, file))), shared(&sharedProgram), module(moduleName) { collectExternSymbols(); }
    Parser(std::string_view input, Program* sharedProgram, std::string_view moduleName,
           std::size_t file, std::vector<std::string> generics)
        : source(input), tokens(tolerate(lex(input, false, file))), shared(sharedProgram),
          module(moduleName), genericScope(std::move(generics)) { collectExternSymbols(); }
    Tree parseSubExpr(unsigned depth) {
        newlines();
        if (current().kind == TokenKind::End)
            throw Diagnostic("E_STRING", "La interpolación '{}' no puede estar vacía.", {0, source.size()});
        Tree t = expression(1, depth);
        newlines();
        if (current().kind != TokenKind::End) expected("}");
        return t;
    }
    bool externBraceFollows() const {
        std::size_t k = pos + 1;
        if (k < tokens.size() && tokens[k].kind == TokenKind::Quoted) ++k;
        while (k < tokens.size() && tokens[k].kind == TokenKind::Newline) ++k;
        return k < tokens.size() && tokens[k].text == "{";
    }
    Program program() {
        if (!shared) result.source = source;
        newlines();
        while (at("import")) {
            const auto start = current().span.start;
            ++pos;
            auto id = name();
            result.imports.push_back({id.text, {start, id.span.end}});
            if (current().kind != TokenKind::End && !newlines()) expected("salto de línea");
        }
        while (current().kind != TokenKind::End) {
            if (at("def")) {
                Diagnostic d("E_PYTHON_DEF", "En Turi una función empieza con fn, lleva llaves y devuelve el resultado con return.", current().span);
                d.expected = "fn"; d.actual = "def";
                d.example = "fn next(n: int) -> int { return n + 1 }";
                throw d;
            }
            if (at("@")) {
                auto attrs = parseAttributes();
                const bool hasDataAttrs = attrs.reprC || attrs.packed || attrs.explicitAlign != 0;
                const bool hasLinkAttrs = !attrs.linkLibs.empty();
                const bool hasFnAttrs = attrs.noCopy || attrs.arenaHint || attrs.compactHint;
                if (hasFnAttrs) {
                    if (hasDataAttrs || hasLinkAttrs) {
                        Diagnostic d("E_ATTR", "No se pueden mezclar atributos de función (@noCopy/@arena/@compact) con atributos de datos o @link.", current().span);
                        throw d;
                    }
                    if (at("struct") || at("enum") || at("interface") || at("impl") || at("const") || at("test") ||
                        (at("extern") && externBraceFollows())) {
                        Diagnostic d("E_ATTR", "Los atributos @noCopy, @arena y @compact solo aplican antes de fn.", current().span);
                        d.example = "@noCopy\nfn sum(a: [int]) -> int { return a[0] }";
                        throw d;
                    }
                    result.functions.push_back(function(true, false, false, attrs));
                } else if (hasLinkAttrs) {
                    if (hasDataAttrs || !at("extern")) {
                        Diagnostic d("E_ATTR", "@link solo aplica antes de un bloque extern \"C\" { ... }.", current().span);
                        d.example = "@link(\"user32\")\nextern \"C\" {\n    fn GetCurrentProcessId() -> u32\n}";
                        throw d;
                    }
                    // Look ahead past extern "C" to forbid @link on extern "C" fn
                    if (!externBraceFollows()) {
                        Diagnostic d("E_ATTR", "@link solo aplica a bloques extern \"C\" { ... }, no a funciones extern \"C\" fn.", current().span);
                        throw d;
                    }
                    externBlock(attrs);
                } else {
                    if (!at("struct") && !at("enum")) {
                        Diagnostic d("E_ATTR", "Los atributos @repr, @packed y @align solo aplican antes de struct o enum.", current().span);
                        d.example = "@repr(C)\nstruct Vec2 { x: f32, y: f32 }";
                        throw d;
                    }
                    result.data.push_back(dataDeclaration(attrs));
                }
            } else if (at("extern")) {
                // Disambiguate `extern "C" { ... }` vs `extern "C" fn ... { ... }`
                if (externBraceFollows()) externBlock(ParsedAttrs{});
                else result.functions.push_back(function());
            }
            else if (at("struct") || at("enum")) result.data.push_back(dataDeclaration());
            else if (at("const")) result.consts.push_back(constDeclaration());
            else if (at("interface")) result.interfaces.push_back(interfaceDeclaration());
            else if (at("impl")) result.impls.push_back(implDeclaration());
            else if (at("import")) throw Diagnostic("E_IMPORT_ORDER", "Los imports preceden a todas las declaraciones.", current().span);
            else result.functions.push_back(declaration());
            if (current().kind != TokenKind::End && !newlines()) expected("salto de línea");
        }
        return std::move(result);
    }
};

void stampStatements(std::vector<Stmt>& statements, std::size_t file);
void stampExprTree(Expr& value, std::size_t file) {
    value.span.file = file;
    for (auto& arg : value.args) stampExprTree(arg, file);
    if (value.closure) stampStatements(value.closure->body, file);
    if (value.matchExpr) {
        stampExprTree(value.matchExpr->scrutinee, file);
        for (auto& arm : value.matchExpr->arms) {
            arm.span.file = file;
            stampStatements(arm.body, file);
            if (arm.resultExpr) stampExprTree(*arm.resultExpr, file);
        }
    }
    if (value.ifExpr) {
        stampExprTree(value.ifExpr->condition, file);
        stampStatements(value.ifExpr->thenBody, file);
        if (value.ifExpr->thenResult) stampExprTree(*value.ifExpr->thenResult, file);
        stampStatements(value.ifExpr->elseBody, file);
        if (value.ifExpr->elseResult) stampExprTree(*value.ifExpr->elseResult, file);
    }
}
void stampStatements(std::vector<Stmt>& statements, std::size_t file) {
    for (auto& statement : statements) {
        statement.span.file = file;
        for (auto& value : statement.exprs) stampExprTree(value, file);
        stampStatements(statement.body, file);
        stampStatements(statement.otherwise, file);
        for (auto& arm : statement.arms) {
            arm.span.file = file;
            stampStatements(arm.body, file);
            if (arm.resultExpr) stampExprTree(*arm.resultExpr, file);
        }
    }
}
void stampFunction(Function& fn, std::size_t file) {
    fn.span.file = file;
    for (auto& param : fn.params) param.span.file = file;
    stampStatements(fn.body, file);
}
template<class Range>
void appendMoved(Range& to, Range& from) {
    to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

} // namespace
Program parse(std::string_view source) { return Parser(source).program(); }
void parseInto(Program& program, std::string_view source, std::string_view module, std::size_t file) {
    auto error = captureDiagnostic([&] {
        auto parsed = Parser(source, program, module, file).program();
        auto stampExpr = [&](Expr& e) { stampExprTree(e, file); };
        for (auto& fn : parsed.functions) stampFunction(fn, file);
        for (auto& c : parsed.consts) {
            c.span.file = file;
            stampExpr(c.init);
        }
        for (auto& data : parsed.data) {
            data.span.file = file;
            for (auto& variant : data.variants) {
                variant.span.file = file;
                if (variant.enumValueExpr) stampExpr(*variant.enumValueExpr);
                for (auto& field : variant.fields) field.span.file = file;
            }
        }
        for (auto& declaration : parsed.interfaces) {
            declaration.span.file = file;
            for (auto& method : declaration.methods) stampFunction(method, file);
        }
        for (auto& declaration : parsed.impls) {
            declaration.span.file = file;
            for (auto& method : declaration.methods) stampFunction(method, file);
        }
        for (auto& import : parsed.imports) import.span.file = file;
        ModuleInfo info{std::string(module), {}, {}, file};
        for (const auto& import : parsed.imports) info.imports.push_back(import.name);
        program.modules.push_back(std::move(info));
        appendMoved(program.functions, parsed.functions);
        appendMoved(program.consts, parsed.consts);
        appendMoved(program.data, parsed.data);
        appendMoved(program.interfaces, parsed.interfaces);
        appendMoved(program.impls, parsed.impls);
        appendMoved(program.imports, parsed.imports);
        appendMoved(program.linkLibs, parsed.linkLibs);
    });
    if (error) {
        error->span.file = file;
        for (auto& related : error->related) related.file = file;
        if (error->fix) error->fix->span.file = file;
        throw *error;
    }
}
} // namespace veloz
