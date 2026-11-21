#pragma once
#include "veloz.hpp"

namespace veloz::syntax {
enum class TokenKind { Word, Digits, Quoted, Symbol, Comment, Newline, End };
struct Token {
    TokenKind kind;
    Span span;
    std::string text;
};
std::vector<Token> lex(std::string_view source, bool keepComments = false);
} // namespace veloz::syntax
