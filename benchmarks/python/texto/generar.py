"""Genera la entrada de texto.py: N líneas deterministas de palabras, mayúsculas y puntuación."""
import sys


def main():
    out, lines = sys.argv[1], int(sys.argv[2])
    words = ("the quick brown fox jumps over lazy dog river mountain cloud stone light shadow garden window "
             "paper silver golden ocean forest bird song night day market city road bridge tower").split()
    seed = 11
    with open(out, "w", encoding="utf-8", newline="\n") as handle:
        for _ in range(lines):
            seed = (seed * 1103515245 + 12345) % 2147483648
            count = 4 + (seed >> 16) % 12
            parts = []
            for _ in range(count):
                seed = (seed * 1103515245 + 12345) % 2147483648
                word = words[(seed >> 16) % len(words)]
                if (seed >> 8) % 7 == 0:
                    word = word.capitalize()
                parts.append(word)
            seed = (seed * 1103515245 + 12345) % 2147483648
            handle.write(" ".join(parts) + ".,;!?"[(seed >> 16) % 5] + "\n")


main()
