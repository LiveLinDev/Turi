#pragma once
#include "veloz.hpp"

namespace veloz::syntax {
enum class TokenKind { Word, Digits, Float, Label, Quoted, Interp, Symbol, Comment, Newline, End };
struct Token {
    TokenKind kind;
    Span span;
    std::string text;
    bool rewritten = false; // creado por tolerate(): fmt escribe `text` en vez del texto fuente
    bool isChar = false;    // literal de carácter 'a': su texto es el código; vale también como str de un carácter
};
std::vector<Token> lex(std::string_view source, bool keepComments = false, std::size_t file = 0);
// Convierte grafías toleradas de otros lenguajes (`;`, `elif`, `let mut`, `x++`, `println!`, `and`,
// `True`, `null`) en su forma canónica. En modo estricto no toca nada salvo que se fuerce (fmt).
std::vector<Token> tolerate(std::vector<Token> tokens, bool force = false);
} // namespace veloz::syntax
