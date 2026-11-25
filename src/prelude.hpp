#pragma once

namespace veloz {

// Biblioteca base escrita en Turi. Solo se incorporan al programa las funciones que este nombra
// (y las que ellas usan), así que un programa que no la usa compila exactamente igual que antes.
// Una función del usuario con el mismo nombre sustituye a la de la biblioteca.
// Los nombres son los de Python y JavaScript, que son los que un modelo escribe por costumbre.
inline const char* turiPreludeSource() {
    return R"turi(struct Tuple2<A, B> {
    v0: A
    v1: B
}

struct Tuple3<A, B, C> {
    v0: A
    v1: B
    v2: C
}

struct Tuple4<A, B, C, D> {
    v0: A
    v1: B
    v2: C
    v3: D
}

fn tuple2<A, B>(a: A, b: B) -> Tuple2<A, B> {
    return Tuple2<A, B> { v0: a, v1: b }
}

fn tuple3<A, B, C>(a: A, b: B, c: C) -> Tuple3<A, B, C> {
    return Tuple3<A, B, C> { v0: a, v1: b, v2: c }
}

fn tuple4<A, B, C, D>(a: A, b: B, c: C, d: D) -> Tuple4<A, B, C, D> {
    return Tuple4<A, B, C, D> { v0: a, v1: b, v2: c, v3: d }
}

fn split(s: str, sep: str) -> [str] {
    var out: [str] = []
    let n = s.len()
    let m = sep.len()
    if m == 0 {
        for i in 0..n {
            out.push(s[i..i + 1])
        }
        return out
    }
    var start = 0
    var i = 0
    while i + m <= n {
        if s[i..i + m] == sep {
            out.push(s[start..i])
            i += m
            start = i
        } else {
            i += 1
        }
    }
    out.push(s[start..n])
    return out
}

fn words(s: str) -> [str] {
    var out: [str] = []
    let n = s.len()
    var i = 0
    while i < n {
        while i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            i += 1
        }
        let start = i
        while i < n && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') {
            i += 1
        }
        if i > start {
            out.push(s[start..i])
        }
    }
    return out
}

fn join(parts: [str], sep: str) -> str {
    var sb = sbNew()
    for i in 0..parts.len() {
        if i > 0 {
            sb.push(sep)
        }
        sb.push(parts[i])
    }
    return sb.str()
}

fn trim(s: str) -> str {
    var lo = 0
    var hi = s.len()
    while lo < hi && (s[lo] == ' ' || s[lo] == '\t' || s[lo] == '\n' || s[lo] == '\r') {
        lo += 1
    }
    while hi > lo && (s[hi - 1] == ' ' || s[hi - 1] == '\t' || s[hi - 1] == '\n' || s[hi - 1] == '\r') {
        hi -= 1
    }
    return s[lo..hi]
}

fn upper(s: str) -> str {
    var sb = sbWithCap(s.len())
    for i in 0..s.len() {
        let c = s[i]
        sb.pushByte(if c >= 'a' && c <= 'z' { c - 32 } else { c })
    }
    return sb.str()
}

fn lower(s: str) -> str {
    var sb = sbWithCap(s.len())
    for i in 0..s.len() {
        let c = s[i]
        sb.pushByte(if c >= 'A' && c <= 'Z' { c + 32 } else { c })
    }
    return sb.str()
}

fn replace(s: str, from: str, to: str) -> str {
    let n = s.len()
    let m = from.len()
    if m == 0 {
        return s
    }
    var sb = sbWithCap(n)
    var i = 0
    while i < n {
        if i + m <= n && s[i..i + m] == from {
            sb.push(to)
            i += m
        } else {
            sb.pushByte(s[i])
            i += 1
        }
    }
    return sb.str()
}

fn strRepeat(s: str, n: int) -> str {
    var sb = sbNew()
    for i in 0..n {
        sb.push(s)
    }
    return sb.str()
}

fn strReverse(s: str) -> str {
    var sb = sbWithCap(s.len())
    var i = s.len() - 1
    while i >= 0 {
        sb.pushByte(s[i])
        i -= 1
    }
    return sb.str()
}

fn chars(s: str) -> [str] {
    var out: [str] = []
    for i in 0..s.len() {
        out.push(s[i..i + 1])
    }
    return out
}

fn bytes(s: str) -> [int] {
    var out: [int] = []
    for i in 0..s.len() {
        out.push(s[i])
    }
    return out
}

fn countOf(s: str, t: str) -> int {
    let m = t.len()
    if m == 0 {
        return 0
    }
    var total = 0
    var i = 0
    while i + m <= s.len() {
        if s[i..i + m] == t {
            total += 1
            i += m
        } else {
            i += 1
        }
    }
    return total
}

fn ord(c: str) -> int {
    return c[0]
}

fn chr(code: int) -> str {
    return strFromByte(code)
}

fn isDigit(c: int) -> bool {
    return c >= '0' && c <= '9'
}

fn isAlpha(c: int) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
}

fn isSpace(c: int) -> bool {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r'
}

fn sum(a: [int]) -> int {
    var total = 0
    for x in a {
        total += x
    }
    return total
}

fn sumFloat(a: [float]) -> float {
    var total = 0.0
    for x in a {
        total += x
    }
    return total
}

fn arrayMax(a: [int]) -> int {
    var best = a[0]
    for x in a {
        if x > best {
            best = x
        }
    }
    return best
}

fn arrayMin(a: [int]) -> int {
    var best = a[0]
    for x in a {
        if x < best {
            best = x
        }
    }
    return best
}

fn map<T, U>(a: [T], f: fn(T) -> U) -> [U] {
    var out: [U] = []
    for x in a {
        out.push(f(x))
    }
    return out
}

fn filter<T>(a: [T], f: fn(T) -> bool) -> [T] {
    var out: [T] = []
    for x in a {
        if f(x) {
            out.push(x)
        }
    }
    return out
}

fn any<T>(a: [T], f: fn(T) -> bool) -> bool {
    for x in a {
        if f(x) {
            return true
        }
    }
    return false
}

fn all<T>(a: [T], f: fn(T) -> bool) -> bool {
    for x in a {
        if !f(x) {
            return false
        }
    }
    return true
}

fn countIf<T>(a: [T], f: fn(T) -> bool) -> int {
    var total = 0
    for x in a {
        if f(x) {
            total += 1
        }
    }
    return total
}

fn reversed<T>(a: [T]) -> [T] {
    var out: [T] = []
    var i = a.len() - 1
    while i >= 0 {
        out.push(a[i])
        i -= 1
    }
    return out
}

fn concat<T>(a: [T], b: [T]) -> [T] {
    var out: [T] = []
    for x in a {
        out.push(x)
    }
    for x in b {
        out.push(x)
    }
    return out
}

fn indexOfInt(a: [int], x: int) -> int {
    for i in 0..a.len() {
        if a[i] == x {
            return i
        }
    }
    return -1
}

fn indexOfStr(a: [str], x: str) -> int {
    for i in 0..a.len() {
        if a[i] == x {
            return i
        }
    }
    return -1
}

)turi";
}

} // namespace veloz
