#include "lexer.hpp"

#include <algorithm>
#include <array>

namespace veloz {
namespace {

using syntax::Token;
using syntax::TokenKind;
using namespace std::string_view_literals;

template<class Range>
bool oneOf(std::string_view word, const Range& set) {
    return std::find(set.begin(), set.end(), word) != set.end();
}

bool pascal(std::string_view text) {
    return !text.empty() && text.front() >= 'A' && text.front() <= 'Z';
}

bool binary(std::string_view text) {
    constexpr std::array operators = {"+"sv, "-"sv, "*"sv, "/"sv, "%"sv, "=="sv, "!="sv, "<"sv,
        "<="sv, ">"sv, ">="sv, "||"sv, "&&"sv, "&"sv, "|"sv, "^"sv, "<<"sv, ">>"sv, ">>>"sv,
        "="sv, "+="sv, "-="sv, "*="sv, "/="sv, "%="sv, "&="sv, "|="sv, "^="sv, "<<="sv, ">>="sv, ">>>="sv,
        "->"sv, "=>"sv, ".."sv, "..="sv};
    return oneOf(text, operators);
}

bool prefixPosition(std::string_view prev) {
    constexpr std::array starts = {""sv, "("sv, "["sv, ","sv, ":"sv, "!"sv, "~"sv, "{"sv, "return"sv,
        "if"sv, "while"sv, "in"sv, "assert"sv, "match"sv, "requires"sv, "ensures"sv};
    return oneOf(prev, starts) || binary(prev);
}

bool isGenericCallOpen(const std::vector<Token>& tokens, std::size_t openIdx) {
    constexpr std::array typeStarters = {"["sv, "dyn"sv, "fn"sv, "int"sv, "float"sv, "bool"sv, "str"sv,
        "void"sv, "StringBuilder"sv, "File"sv, "TcpListener"sv, "Socket"sv, "Mutex"sv, "Lock"sv, "Scope"sv, "Region"sv,
        "i8"sv, "i16"sv, "i32"sv, "i64"sv, "u8"sv, "u16"sv, "u32"sv, "u64"sv, "f32"sv, "f64"sv};
    std::size_t i = openIdx + 1;
    while (i < tokens.size() && tokens[i].kind == TokenKind::Newline) ++i;
    if (i >= tokens.size()) return false;
    const auto& first = tokens[i];
    const bool validStart = oneOf(first.text, typeStarters) ||
        (first.kind == TokenKind::Word && pascal(first.text) && first.text.find('_') == std::string::npos) ||
        (first.kind == TokenKind::Word && !first.text.empty() && first.text.front() >= 'a' && first.text.front() <= 'z' &&
         i + 2 < tokens.size() && tokens[i + 1].text == "." && tokens[i + 2].kind == TokenKind::Word && pascal(tokens[i + 2].text));
    if (!validStart) return false;
    int angleDepth = 1, parenDepth = 0, bracketDepth = 0;
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

class Formatter {
    std::string_view source;
    const std::vector<Token>& tokens;
    std::string out;
    std::vector<bool> literalBraces;
    unsigned parens = 0;
    int genericDepth = 0;
    bool topDeclHeader = false;
    bool justClosedGeneric = false;
    bool inControlCond = false;
    bool prefix = false;
    bool inClosureParams = false;
    bool closureJustOpened = false;
    bool justClosedClosure = false;
    bool prevWasComment = false;
    std::string_view prev, beforePrev;
    TokenKind prevKind = TokenKind::End;

    bool lineStart() const { return out.empty() || out.back() == '\n'; }
    void line() {
        if (!lineStart()) out += '\n';
    }
    void inputLine() {
        if (!out.empty() && !out.ends_with("\n\n")) out += '\n';
    }
    void indent(bool continuation) {
        auto level = literalBraces.size();
        if (parens || continuation) ++level;
        out.append(level * 4, ' ');
    }
    void appendComment(const Token& comment) {
        if (lineStart()) indent(false);
        else out += ' ';
        out += comment.text;
        prevWasComment = true;
    }
    bool needsSpace(std::string_view raw, bool genericOpen, bool genericClose, bool closureClose) const {
        if (prev.empty() || raw == "," || raw == ":" || raw == ")" || raw == "]" || raw == "." ||
            raw == ".." || raw == "..=" || raw == "?" || genericClose || genericOpen || closureClose || closureJustOpened) return false;
        if (prev == "(" || prev == "[" || prev == "." || prev == ".." || prev == "..=" || prev == "@" || prefix || (genericDepth > 0 && prev == "<")) return false;
        if (raw == "[") {
            constexpr std::array noSpaceBeforeIndex = {"return"sv, "in"sv, "assert"sv, "match"sv, "else"sv, "mut"sv, "requires"sv, "ensures"sv};
            const bool indexTarget = prev == ")" || prev == "]" || prev == "?" || prevKind == TokenKind::Quoted ||
                prevKind == TokenKind::Interp ||
                (prevKind == TokenKind::Word && !oneOf(prev, noSpaceBeforeIndex));
            return !indexTarget;
        }
        if (raw == "}") return prev != "{";
        if (raw == "(") {
            constexpr std::array expressionWords = {"assert"sv, "return"sv, "if"sv, "while"sv, "in"sv, "match"sv, "requires"sv, "ensures"sv,
                                                    "let"sv, "var"sv, "for"sv, "else"sv};
            return oneOf(prev, expressionWords) ||
                (prev != ">" && prev != ">>" && prev != ">>>" && prev != ")" && prev != "]" && prevKind != TokenKind::Word);
        }
        return true;
    }
public:
    Formatter(std::string_view input, const std::vector<Token>& stream) : source(input), tokens(stream) {}
    std::string format() {
        for (std::size_t i = 0; tokens[i].kind != TokenKind::End; ++i) {
            const auto& token = tokens[i];
            if (token.kind == TokenKind::Newline) { inputLine(); continue; }
            if (token.kind == TokenKind::Comment) { appendComment(token); continue; }
            auto raw = token.rewritten ? std::string_view(token.text)
                                       : source.substr(token.span.start, token.span.end - token.span.start);
            const bool genericOpen = raw == "<" &&
                (beforePrev == "fn" || prev == "impl" || (prevKind == TokenKind::Word && pascal(prev)) ||
                 isGenericCallOpen(tokens, i));
            int genericPop = 0;
            if (genericDepth > 0) {
                if (raw == ">") genericPop = 1;
                else if (raw == ">>" && genericDepth >= 2) genericPop = 2;
                else if (raw == ">>>" && genericDepth >= 3) genericPop = 3;
            }
            const bool genericClose = genericPop > 0;
            const bool closureStartPos = prefixPosition(prev) || justClosedClosure;
            const bool closureOpen = !inClosureParams && raw == "|" && closureStartPos;
            const bool closureClose = inClosureParams && raw == "|";
            const bool zeroClosure = !inClosureParams && raw == "||" && closureStartPos;
            const bool unary = raw == "!" || raw == "~" || (raw == "-" && prefixPosition(prev));
            const bool isDeclFn = raw == "fn" && !(i + 1 < tokens.size() && tokens[i + 1].text == "(");
            if ((literalBraces.empty() &&
                 (raw == "struct" || raw == "enum" || raw == "interface" || raw == "impl" ||
                  isDeclFn || raw == "test" || raw == "extern")) ||
                (literalBraces.size() <= 1 && isDeclFn) ||
                (justClosedClosure && raw == "->"))
                topDeclHeader = true;
            if (raw == "if" || raw == "while" || raw == "match") inControlCond = true;
            if (raw == "requires" || raw == "ensures") topDeclHeader = false;
            if (raw == "}") {
                topDeclHeader = false;
                bool literal = literalBraces.back();
                literalBraces.pop_back();
                if (!literal && prev != "{") line();
            }
            if (raw == ")") --parens;
            if (raw == "else" && lineStart() && !prevWasComment && prevKind == TokenKind::Symbol && prev == "}") {
                while (!out.empty() && out.back() == '\n') out.pop_back();
                out += ' ';
            } else if (lineStart()) {
                const bool continuation = raw != "}" && raw != ")" && prev != "{" &&
                    (binary(prev) || (prev == "," && !literalBraces.empty() && literalBraces.back()));
                indent(continuation);
            } else if (needsSpace(raw, genericOpen, genericClose, closureClose)) out += ' ';
            out += raw;
            if (raw == "(") ++parens;
            if (genericOpen) ++genericDepth;
            genericDepth -= genericPop;
            if (raw == "{") {
                bool isFieldInitOrEmpty = false;
                std::size_t k = i + 1;
                while (tokens[k].kind == TokenKind::Newline || tokens[k].kind == TokenKind::Comment) ++k;
                if (tokens[k].text == "}") isFieldInitOrEmpty = !inControlCond;
                else if (tokens[k].kind == TokenKind::Word) {
                    std::size_t k2 = k + 1;
                    while (tokens[k2].kind == TokenKind::Newline || tokens[k2].kind == TokenKind::Comment) ++k2;
                    if (tokens[k2].text == ":") isFieldInitOrEmpty = true;
                }
                bool literal = !topDeclHeader && isFieldInitOrEmpty &&
                    ((pascal(prev) && beforePrev != "for" && beforePrev != "->") || justClosedGeneric);
                topDeclHeader = false;
                inControlCond = false;
                literalBraces.push_back(literal);
                auto& next = tokens[i + 1];
                if (!literal && !(next.kind == TokenKind::Symbol && next.text == "}") &&
                    next.kind != TokenKind::Comment && next.kind != TokenKind::Newline) line();
            }
            justClosedGeneric = genericClose;
            prefix = unary;
            if (closureOpen) inClosureParams = true;
            else if (closureClose) inClosureParams = false;
            closureJustOpened = closureOpen;
            justClosedClosure = closureClose || zeroClosure;
            beforePrev = prev;
            prev = raw;
            prevKind = token.kind;
            prevWasComment = false;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) out.pop_back();
        if (!out.empty()) out += '\n';
        return out;
    }
};
} // namespace

std::string formatSource(std::string_view source) {
    parse(source);
    auto tokens = syntax::tolerate(syntax::lex(source, true), true);
    return Formatter(source, tokens).format();
}
} // namespace veloz
