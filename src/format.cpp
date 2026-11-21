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
        "<="sv, ">"sv, ">="sv, "||"sv, "&&"sv, "="sv, "+="sv, "-="sv, "*="sv, "/="sv,
        "%="sv, "->"sv, "=>"sv, ".."sv};
    return oneOf(text, operators);
}

bool prefixPosition(std::string_view prev) {
    constexpr std::array starts = {""sv, "("sv, "["sv, ","sv, ":"sv, "!"sv, "{"sv, "return"sv,
        "if"sv, "while"sv, "in"sv, "assert"sv, "match"sv};
    return oneOf(prev, starts) || binary(prev);
}

class Formatter {
    std::string_view source;
    const std::vector<Token>& tokens;
    std::string out;
    std::vector<bool> literalBraces;
    unsigned parens = 0;
    bool generics = false;
    bool prefix = false;
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
    }
    bool needsSpace(std::string_view raw, bool genericOpen, bool genericClose) const {
        if (prev.empty() || raw == "," || raw == ":" || raw == ")" || raw == "]" || raw == "." ||
            raw == ".." || genericClose || genericOpen) return false;
        if (prev == "(" || prev == "[" || prev == "." || prev == ".." || prefix || (generics && prev == "<")) return false;
        if (raw == "[") return prev == ":" || prev == "->";
        if (raw == "}") return prev != "{";
        if (raw == "(") {
            constexpr std::array expressionWords = {"assert"sv, "return"sv, "if"sv, "while"sv, "in"sv, "match"sv};
            return oneOf(prev, expressionWords) || (prev != ">" && prevKind != TokenKind::Word);
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
            auto raw = source.substr(token.span.start, token.span.end - token.span.start);
            const bool genericOpen = raw == "<" && beforePrev == "fn";
            const bool genericClose = raw == ">" && generics;
            const bool unary = raw == "!" || (raw == "-" && prefixPosition(prev));
            if (raw == "}") {
                bool literal = literalBraces.back();
                literalBraces.pop_back();
                if (!literal && prev != "{") line();
            }
            if (raw == ")") --parens;
            if (lineStart()) {
                const bool continuation = raw != "}" && raw != ")" && prev != "{" &&
                    (binary(prev) || (prev == "," && !literalBraces.empty()));
                indent(continuation);
            } else if (needsSpace(raw, genericOpen, genericClose)) out += ' ';
            out += raw;
            if (raw == "(") ++parens;
            if (genericOpen) generics = true;
            if (genericClose) generics = false;
            if (raw == "{") {
                bool literal = pascal(prev) && beforePrev != "." && beforePrev != "enum" &&
                               beforePrev != "struct" && beforePrev != "->";
                literalBraces.push_back(literal);
                auto& next = tokens[i + 1];
                if (!literal && !(next.kind == TokenKind::Symbol && next.text == "}") &&
                    next.kind != TokenKind::Comment && next.kind != TokenKind::Newline) line();
            }
            prefix = unary;
            beforePrev = prev;
            prev = raw;
            prevKind = token.kind;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) out.pop_back();
        if (!out.empty()) out += '\n';
        return out;
    }
};
} // namespace

std::string formatSource(std::string_view source) {
    parse(source);
    auto tokens = syntax::lex(source, true);
    return Formatter(source, tokens).format();
}
} // namespace veloz
