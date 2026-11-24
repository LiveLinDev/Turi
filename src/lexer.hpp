#pragma once
#include "veloz.hpp"

namespace veloz::syntax {
enum class TokenKind { Word, Digits, Float, Label, Quoted, Interp, Symbol, Comment, Newline, End };
struct Token {
    TokenKind kind;
    Span span;
    std::string text;
};
std::vector<Token> lex(std::string_view source, bool keepComments = false, std::size_t file = 0);
} // namespace veloz::syntax
