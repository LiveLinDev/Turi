#!/usr/bin/env python3
"""Generate random arithmetic expressions, parse them with a recursive
descent parser, evaluate them, and print the sum of their values."""

import random
import sys


class Token:
    def __init__(self, kind, value):
        self.kind = kind
        self.value = value

    def __repr__(self):
        return f"Token({self.kind!r}, {self.value!r})"


class Lexer:
    def __init__(self, text):
        self.text = text
        self.pos = 0

    def tokens(self):
        toks = []
        while self.pos < len(self.text):
            ch = self.text[self.pos]
            if ch.isspace():
                self.pos += 1
            elif ch.isdigit():
                start = self.pos
                while self.pos < len(self.text) and self.text[self.pos].isdigit():
                    self.pos += 1
                toks.append(Token("INT", int(self.text[start:self.pos])))
            elif ch == "+":
                toks.append(Token("PLUS", ch))
                self.pos += 1
            elif ch == "-":
                toks.append(Token("MINUS", ch))
                self.pos += 1
            elif ch == "*":
                toks.append(Token("STAR", ch))
                self.pos += 1
            elif ch == "/":
                toks.append(Token("SLASH", ch))
                self.pos += 1
            elif ch == "(":
                toks.append(Token("LPAREN", ch))
                self.pos += 1
            elif ch == ")":
                toks.append(Token("RPAREN", ch))
                self.pos += 1
            else:
                raise SyntaxError(f"unexpected character {ch!r} at position {self.pos}")
        toks.append(Token("EOF", None))
        return toks


class Node:
    pass


class Int(Node):
    def __init__(self, value):
        self.value = value

    def eval(self):
        return self.value


class BinOp(Node):
    def __init__(self, op, left, right):
        self.op = op
        self.left = left
        self.right = right

    def eval(self):
        if self.op == "+":
            return self.left.eval() + self.right.eval()
        if self.op == "-":
            return self.left.eval() - self.right.eval()
        if self.op == "*":
            return self.left.eval() * self.right.eval()
        if self.op == "/":
            return self.left.eval() // self.right.eval()
        raise ValueError(f"unknown operator {self.op!r}")


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos]

    def advance(self):
        tok = self.tokens[self.pos]
        self.pos += 1
        return tok

    def expect(self, kind):
        tok = self.peek()
        if tok.kind != kind:
            raise SyntaxError(f"expected {kind}, got {tok.kind} ({tok.value!r})")
        return self.advance()

    def parse(self):
        node = self.expr()
        self.expect("EOF")
        return node

    def expr(self):
        node = self.term()
        while self.peek().kind in ("PLUS", "MINUS"):
            op = self.advance()
            node = BinOp(op.kind, node, self.term())
        return node

    def term(self):
        node = self.factor()
        while self.peek().kind in ("STAR", "SLASH"):
            op = self.advance()
            node = BinOp(op.kind, node, self.factor())
        return node

    def factor(self):
        tok = self.peek()
        if tok.kind == "INT":
            self.advance()
            return Int(tok.value)
        if tok.kind == "LPAREN":
            self.advance()
            node = self.expr()
            self.expect("RPAREN")
            return node
        raise SyntaxError(f"unexpected token {tok.kind} ({tok.value!r})")


def generate_expression(rng, depth=0):
    """Build a random expression string."""
    if depth > 3 or rng.random() < 0.4:
        return str(rng.randint(0, 99))
    op = rng.choice("+-*/")
    left = generate_expression(rng, depth + 1)
    right = generate_expression(rng, depth + 1)
    expr = f"{left} {op} {right}"
    if depth == 0:
        return expr
    return f"({expr})"


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    rng = random.Random(42)
    total = 0
    for _ in range(n):
        text = generate_expression(rng)
        ast = Parser(Lexer(text).tokens()).parse()
        total += ast.eval()
    print(total)


if __name__ == "__main__":
    main()
