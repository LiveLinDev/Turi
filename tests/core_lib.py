"""Biblioteca base (cadenas y arreglos), UFCS general y azúcar de mapas.

Cada programa se ejecuta en el intérprete y en el binario nativo y debe dar la salida esperada.
Además se comprueba que un programa que no nombra la biblioteca no la incorpora, que una función
del usuario con el mismo nombre sustituye a la de la biblioteca y que el modo estricto no la usa.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

CLI = sys.argv[1]

CASES = [
    ("strings", """fn main() {
    let ws = "the quick brown fox".split(" ")
    print(ws.len())
    print(ws.reverse().join(" "))
    print("hola Mundo".upper() + "|" + "hola Mundo".toLowerCase() + "|" + "  x y  ".trim() + "|")
    print("ab".repeat(3) + "abc".reverse() + "banana".count("an"))
    print("a,b,,c".split(",").len() + "  dos  palabras ".words().len())
    print("x-y".replace("-", "+") + "abc".chars().join("."))
    print("hey".length() + "A".bytes()[0])
    print("abc".split("").join("|"))
    print(isDigit('7') && isAlpha('q') && isSpace(' ') && !isDigit('x'))
}
""", "4\nfox brown quick the\nHOLA MUNDO|hola mundo|x y|\nabababcba2\n6\nx+ya.b.c\n68\na|b|c\ntrue\n"),
    ("arrays", """fn main() {
    let nums = [1, 2, 3, 4, 5]
    print(nums.map(|x| x * x).filter(|x| x % 2 == 1).sum())
    print(nums.max() + nums.min() + nums.indexOf(4))
    print(nums.contains(3) && !nums.contains(9) && ["a", "fox"].contains("fox"))
    print(nums.any(|x| x > 4) && nums.all(|x| x > 0))
    print(nums.concat([6, 7]).len() + nums.count(|x| x > 2))
    print([1.5, 2.5].sum())
    print(nums.reverse()[0] + nums[0])
    var acc = [1]
    acc.append(2)
    print(acc.size() + ["x", "yy"].map(|w| w.len()).sum())
    print(["b", "a"].indexOf("a") + ["b", "a"].indexOf("z"))
}
""", "35\n9\ntrue\ntrue\n10\n4\n6\n5\n0\n"),
    ("maps", """fn main() {
    var counts: Map<str, int> = mapNew()
    for w in "a b a c a b".split(" ") {
        counts[w] += 1
    }
    counts["z"] = 40
    counts["z"] -= 4
    counts["a"] *= 5
    print(counts["a"] + counts["b"] + counts["z"] + counts["missing"])
    print(counts.len())
    var total: Map<int, float> = mapNew()
    for i in 0..10 {
        total[i % 3] += float(i) * 0.5
    }
    print(total[0] + total[1] + total[2])
    var names: Map<int, str> = mapNew()
    names[1] = "uno"
    print(names[1] + "|" + names[2] + "|")
    counts.set("b", counts.get("b").unwrapOr(0) + 10)
    print(counts["b"])
}
""", "53\n4\n22.5\nuno||\n12\n"),
    ("chars", """fn countVowels(s: str) -> int {
    var n = 0
    for c in s.chars() {
        if c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' { n += 1 }
    }
    n
}
fn balanced(s: str) -> bool {
    var depth = 0
    for c in s {
        if c == '(' {
            depth += 1
        } else if ')' == c {
            depth -= 1
            if depth < 0 { return false }
        }
    }
    depth == 0
}
fn bytesVowels(s: str) -> int {
    var n = 0
    for i in 0..s.len() {
        if s[i] == 'a' || s[i] == 'e' { n += 1 }
    }
    n
}
fn main() {
    print(countVowels("education"))
    print(balanced("(a(b))") && !balanced(")("))
    print(bytesVowels("aeiou"))
    print("x" + 'y' + "z")
    print("a,b".split(',').len() + 'a')
    var parts: [str] = []
    parts.push('q')
    print(parts.join("") + "!")
}
""", "5\ntrue\n2\nxyz\n99\nq!\n"),
    ("charCodes", """fn caesarShift(s: str, k: int) -> str {
    var out = ""
    for c in s {
        if c >= 'a' && c <= 'z' {
            var code = ord(c) - ord('a') + k
            if code >= 26 { code -= 26 }
            out += chr(code + ord('a'))
        } else {
            out += c
        }
    }
    out
}
fn capitalizeWords(s: str) -> str {
    var out = ""
    var first = true
    for c in s {
        if c == ' ' {
            out += " "
            first = true
        } else {
            if first && c >= 'a' && c <= 'z' {
                out += strFromByte(c - 'a' + 'A')
            } else {
                out += c
            }
            first = false
        }
    }
    out
}
fn main() {
    print(caesarShift("xyz-abc", 3))
    print(capitalizeWords("hola mundo cruel"))
    print(int("41") + 1)
    print(float("2.5") * 2.0)
    print('z' - "a" + 0)
}
""", "abc-def\nHola Mundo Cruel\n42\n5\n25\n"),
    ("ordOfByte", """fn toHex(n: int) -> str {
    if n == 0 { return "0" }
    var digits = "0123456789abcdef"
    var x = n
    var out = ""
    while x > 0 {
        out = strFromByte(ord(digits[x % 16])) + out
        x = x / 16
    }
    out
}
fn main() {
    print(toHex(48879) + toHex(0) + ord("a") + ord('b'))
}
""", "beef09798\n"),
    ("tuples", """fn minMax(a: [int]) -> (int, int) {
    var lo = a[0]
    var hi = a[0]
    for x in a {
        if x < lo { lo = x }
        if x > hi { hi = x }
    }
    (lo, hi)
}
fn mergeCount(a: [int], b: [int]) -> (int, [int]) {
    var count = 0
    var out: [int] = []
    var i = 0
    var j = 0
    while i < a.len() && j < b.len() {
        if a[i] <= b[j] {
            out.push(a[i])
            i += 1
        } else {
            out.push(b[j])
            j += 1
            count += a.len() - i
        }
    }
    while i < a.len() { out.push(a[i]); i += 1 }
    while j < b.len() { out.push(b[j]); j += 1 }
    (count, out)
}
fn sortCount(a: [int]) -> (int, [int]) {
    if a.len() <= 1 {
        return (0, a)
    }
    let mid = a.len() / 2
    let (c1, left) = sortCount(a[0..mid])
    let (c2, right) = sortCount(a[mid..a.len()])
    let (c3, merged) = mergeCount(left, right)
    (c1 + c2 + c3, merged)
}
fn triple() -> (str, int, bool) {
    return ("x", 7, true)
}
fn main() {
    let (lo, hi) = minMax([4, -2, 9, 3])
    print(lo + hi)
    let r = sortCount([3, 1, 2, 5, 4])
    print(r.0 + r.1.len() + r[1][0])
    let (name, n, ok) = triple()
    print(name + n + ok)
    let pair = (1.5, "z")
    print(pair.1 + pair.0)
    var (a, _) = (10, 20)
    a += 1
    print(a)
}
""", "7\n9\nx7true\nz1.5\n11\n"),
    ("userOverride", """fn upper(s: str) -> str {
    return "mine:" + s
}
fn sum(a: [int]) -> int {
    return 42
}
fn main() {
    print("x".upper())
    print([1, 2].sum())
    print("a b".split(" ").len())
}
""", "mine:x\n42\n2\n"),
    ("ufcsOwnFunctions", """struct P { x: int }
fn doubled(n: int) -> int {
    return n * 2
}
fn shout(s: str, times: int) -> str {
    return s.repeat(times) + "!"
}
fn bump(a: mut [int], by: int) {
    for i in 0..a.len() {
        a[i] += by
    }
}
fn main() {
    print(21.doubled())
    print("eh".shout(2))
    var v = [1, 2]
    v.bump(10)
    print(v[0] + v[1])
}
""", "42\neheh!\n23\n"),
    # Regresión (25 nov 2026): una cadena que se acumula dentro de un `if` en un bucle. El pase que
    # convierte un `if` en selección ejecutaba sin condición el Drop del valor anterior y el nativo
    # devolvía la cadena vacía.
    ("condAccum", """fn rev(words: [str]) -> str {
    var result = ""
    var i = 2
    while i >= 0 {
        result = result + words[i]
        if i > 0 {
            result = result + " "
        }
        i = i - 1
    }
    return result
}

fn tags(n: int) -> [str] {
    var out: [str] = []
    var label = "n"
    for i in 0..n {
        if i % 2 == 0 {
            label = label + str(i)
        }
        out.push(label)
    }
    return out
}

fn main() {
    print(rev(["one", "two", "three"]))
    print(tags(5).join(","))
}
""", "three two one\nn0,n0,n02,n02,n024\n"),
]


def run(args):
    return subprocess.run(args, capture_output=True, text=True, encoding="utf-8", timeout=180)


def main():
    with tempfile.TemporaryDirectory(prefix="turi-corelib-") as tmp:
        tmp = Path(tmp)
        for name, source, expected in CASES:
            path = tmp / f"{name}.turi"
            path.write_text(source, encoding="utf-8", newline="\n")
            interp = run([CLI, "run", str(path)])
            assert interp.returncode == 0 and interp.stdout == expected, (name, "run", interp.stdout, interp.stderr)
            exe = tmp / (f"{name}_bin.exe" if sys.platform == "win32" else f"{name}_bin")
            for flags in ([], ["--release"]):
                built = run([CLI, "build", str(path), "-o", str(exe), *flags])
                assert built.returncode == 0, (name, flags, built.stderr)
                native = run([str(exe)])
                assert native.returncode == 0 and native.stdout == expected, (name, flags, native.stdout, native.stderr)
        # Un programa que no nombra la biblioteca no la incorpora.
        plain = tmp / "plain.turi"
        plain.write_text("fn main() {\n    var a = [1, 2]\n    a.push(3)\n    print(a.len())\n}\n", encoding="utf-8", newline="\n")
        ir = run([CLI, "emit-ir", str(plain)])
        assert ir.returncode == 0, ir.stderr
        for symbol in ('@"turi.split"', '@"turi.join"', '@"turi.map', '@"turi.sum"', '@"turi.upper"'):
            assert symbol not in ir.stdout, symbol
        # El modo estricto no conoce la biblioteca ni el azúcar de mapas.
        strict = run([CLI, "check", str(tmp / "strings.turi"), "--strict"])
        assert strict.returncode != 0, "strict debía rechazar la biblioteca base"
        strict_maps = run([CLI, "check", str(tmp / "maps.turi"), "--strict"])
        assert strict_maps.returncode != 0, "strict debía rechazar m[k]"
    print(f"{len(CASES)} programas de la biblioteca base comprobados.")


if __name__ == "__main__":
    main()
