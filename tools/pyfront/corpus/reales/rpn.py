#!/usr/bin/env python3
"""Evaluate reverse Polish notation expressions, one per line, from stdin."""

import sys


class RPNError(Exception):
    """Raised when an RPN expression cannot be evaluated."""


def evaluate(tokens):
    stack = []
    for token in tokens:
        if token in ("+", "-", "*", "/"):
            if len(stack) < 2:
                raise RPNError("not enough operands for operator '%s'" % token)
            b = stack.pop()
            a = stack.pop()
            if token == "+":
                stack.append(a + b)
            elif token == "-":
                stack.append(a - b)
            elif token == "*":
                stack.append(a * b)
            else:  # "/"
                if b == 0:
                    raise ZeroDivisionError("division by zero")
                stack.append(a / b)
        else:
            try:
                stack.append(float(token))
            except ValueError:
                raise RPNError("unknown token '%s'" % token) from None
    if len(stack) != 1:
        raise RPNError("malformed expression: %d values left on stack" % len(stack))
    return stack[0]


def format_result(value):
    if value == int(value):
        return str(int(value))
    return repr(value)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        tokens = line.split()
        try:
            result = evaluate(tokens)
        except ZeroDivisionError:
            print("error: division by zero")
        except RPNError as exc:
            print("error: %s" % exc)
        else:
            print(format_result(result))


if __name__ == "__main__":
    main()
