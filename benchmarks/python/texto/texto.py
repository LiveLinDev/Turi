"""Procesa un archivo de texto: líneas, palabras y caracteres, la línea más larga y las palabras más
frecuentes (sin distinguir mayúsculas)."""
import sys


def main():
    path = sys.argv[1]
    lines = words = chars = 0
    longest = ""
    freq = {}
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            lines += 1
            chars += len(line)
            text = line.rstrip("\n")
            if len(text) > len(longest):
                longest = text
            for word in text.lower().split():
                word = word.strip(".,;:!?")
                if word:
                    words += 1
                    freq[word] = freq.get(word, 0) + 1
    print(lines, words, chars)
    print(len(longest), longest[:40])
    for word, count in sorted(freq.items(), key=lambda kv: (-kv[1], kv[0]))[:10]:
        print(f"{word:>12} {count}")


main()
