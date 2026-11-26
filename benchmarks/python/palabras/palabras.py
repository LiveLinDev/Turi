"""Frecuencia de palabras: genera N palabras con un generador congruencial, las cuenta en un
diccionario y escribe las diez más frecuentes (empates por orden alfabético)."""
import sys

SYLLABLES = ["ka", "lo", "mi", "ne", "ru", "sa", "to", "vi", "ze", "po", "an", "el"]


def words(n):
    seed = 42
    out = []
    for _ in range(n):
        seed = (seed * 1103515245 + 12345) % 2147483648
        length = 1 + (seed >> 16) % 3
        word = ""
        for _ in range(length):
            seed = (seed * 1103515245 + 12345) % 2147483648
            word += SYLLABLES[(seed >> 16) % len(SYLLABLES)]
        out.append(word)
    return out


def count(ws):
    freq = {}
    for w in ws:
        freq[w] = freq.get(w, 0) + 1
    return freq


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 100000
    freq = count(words(n))
    top = sorted(freq.items(), key=lambda kv: (-kv[1], kv[0]))[:10]
    for word, c in top:
        print(word, c)
    print(len(freq))


main()
