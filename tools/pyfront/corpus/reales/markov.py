"""Generate text from a bigram Markov chain built from stdin input."""

import random
import sys
import textwrap


def build_chain(tokens):
    """Return a mapping of each word to the list of words that follow it."""
    chain = {}
    for current, nxt in zip(tokens, tokens[1:]):
        chain.setdefault(current, []).append(nxt)
    return chain


def generate(chain, start, count, rng):
    """Generate `count` words starting from `start` using the chain."""
    words = [start]
    vocabulary = list(chain)
    while len(words) < count:
        successors = chain.get(words[-1])
        if successors:
            words.append(rng.choice(successors))
        else:
            # Word never observed with a successor; fall back to any known word.
            words.append(rng.choice(vocabulary))
    return words[:count]


def main():
    tokens = sys.stdin.read().split()
    if not tokens:
        return

    rng = random.Random(1)
    chain = build_chain(tokens)
    words = generate(chain, tokens[0], 200, rng)
    print(textwrap.fill(" ".join(words), width=70))


if __name__ == "__main__":
    main()
