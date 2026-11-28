"""Verifica ambos formatos con llguidance y la gramática Lark con el parser de Python."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

from lark import Lark, UnexpectedInput
from llguidance import LLMatcher, LLTokenizer, TokenizerWrapper, grammar_from

ROOT = Path(__file__).resolve().parents[1]
GRAMMAR = ROOT / "grammar"


class ByteVocabulary:
    """Un token por byte permite comprobar texto UTF-8 sin un modelo externo."""
    eos_token_id = 256
    bos_token_id = 256
    special_token_ids = [256]
    tokens = [bytes([i]) for i in range(256)] + [b"<eos>"]

    def __call__(self, text):
        return list(text.encode("utf-8") if isinstance(text, str) else text)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Uso: python tests/grammar.py RUTA_A_VELOZ")
    cli = Path(sys.argv[1]).resolve()
    # Las gramáticas de generación describen la sintaxis canónica.
    formatter = [cli, "fmt", "--strict"]
    subprocess.run([sys.executable, GRAMMAR / "sync.py", "--check"], check=True)
    examples = sorted((ROOT / "examples").glob("*.turi"))
    valid = [example.read_text(encoding="utf-8-sig") for example in examples] + [
        '',
        '\ufeff // comentario\r\nfn main() {\r\nprint("Lima y Perú")\r\n}',
        'fn main() { let a = 7 +\n 6\nprint(a) }',
        'fn main() { let returnValue = 1\nprint(returnValue) }',
        'fn main() {// un comentario\n} // fin',
        'fn f(x: int) -> int { return(x - 1) }',
        'test "alternativas" { if true {} else if false {} else {} }',
        'fn f(\n a: int,\n b: bool) {}',
        'fn bump(var n: int, var active: bool) -> int { if active { n += 1 }\nreturn n }',
        '''struct Point { x: int,\ny: int }
enum Search { Missing\nFound(p: Point) }
fn id<T>(x: T) -> T { return x }
fn main() {
    match Search.Found(Point { y: 1, x: 2 }) {
        Search.Missing => print("ninguno")
        Search.Found(p) => { print(id(p).x) }
    }
}''',
        'fn f<T, U>(a: T, b: U) -> T { return a }',
        'fn main() { print(Pair { x: 1 }.x) }',
        'fn main() { let strict = 18\nprint(strict) }',
        'fn main() { print(3 > (2) && true) }',
        'fn f(a: [int]) -> [int] { return a }\nfn main() { var a=f(repeat(3,2))\na[0]=1\nprint(a[1]) }',
        'fn main() { print(repeat(1, 2)[1 -\n 1]) }',
        '''import model
import contracts
interface Scored { fn scoreOf(value: Self) -> int }
impl contracts.Scored for model.Item {
    fn scoreOf(value: model.Item) -> int { return value.score }
}
fn score<T: contracts.Scored>(value: T) -> int { return scoreOf(value) }
fn main() {
    let item = model.Item { score: 3 }
    print(score(item))
    print(model.State.Ready == model.State.Ready)
    match model.State.Ready { model.State.Ready => print(1) }
    stats.evaluate(1)
}''',
        'fn f(a: [str]) {}',
        'fn g(a: [[int]]) -> [int] { return a[0] }',
        'fn main() {\n    var a = [1, 2, 3]\n    let s = a[1..3]\n    for x in s { print(x) }\n}',
        'struct S { x: int, items: [int] }\nfn main() {\n    var s = S { x: 0, items: [1, 2] }\n    s.x = 1\n    s.items[0] = 2\n}',
        'fn main() {\n    var sb: StringBuilder = sbNew()\n    let n = 42\n    let s = "x={n}, \\{ok\\}, {{raw}}, nested={"a={n}"}, s=" + str(n)\n    print(s[0..2])\n}',
        'test "texto {{escaped}} \\{ok\\}" { print("ok") }',
        '''struct Stack<T> { items: [T] }
enum Tree<T> { Empty, Node(val: T, kids: [Tree<T>]) }
fn find(n: int) -> int? { if n > 0 { return Some(n) } else { return None } }
fn parsePair(ok: bool) -> Result<int, str> { if ok { return Ok(1) } else { return Err("bad") } }
fn step(ok: bool) -> Result<int, str> {
    let v = find(3)? + parsePair(ok)?
    match find(v) {
        Some(x) => print(x)
        None => print(0)
    }
    return Ok(v)
}''',
        'fn main() {\n    let s = Stack<int> { items: [1] }\n    let t: Tree<int> = Tree.Empty\n    step(true)?\n}',
        'fn main() {\n    var x = 0\n    if true {\n        x = 1\n    }\n    else if false {\n        x = 2\n    } // comentario\n    else {\n        x = 3\n    }\n    print(x)\n}',
        '''enum Ev { NoneEv, One(a: int), Two(a: int, b: int) }
fn id<T>(x: T) -> T { return x }
fn main() {
    let _ = [1, 2]
    for _ in 0..3 { print(1) }
    for _ in [10, 20] { print(2) }
    let m = mapNew<str, int>()
    let s = setNew<int>()
    let p = poolWithCap<int>(4)
    let y = match Ev.Two(1, 2) { Ev.One(_) => 10, Ev.Two(_, b) => b, _ => 0 }
    let z = match Ev.NoneEv {
        Ev.One(v) => v,
        _ => {
            let tmp = id<int>(y)
            tmp + 1
        },
    }
    print(z)
}''',
        '''struct Holder { x: int, a: [int] }
fn bump(v: mut int) { v += 1 }
fn main() {
    var arr = [Holder { x: 1, a: [2] }]
    arr[0].x = 5
    arr[0].a[0] += 3
    var mat = [[1, 2], [3, 4]]
    mat[0][1] = 9
    bump(mut arr[0].x)
    bump(mut arr[0].a[0])
}''',
        '''@repr(C)
@align(16)
struct Packet { a: u8, b: u32, c: i64 }
@packed
struct Header { kind: i8, size: u16, bits: f32 }
@align(32)
enum Tag { A, B(v: i16) }
const kSz: int = sizeOf<Packet>()
fn conv(x: f64, y: u64) -> f32 {
    let u = bitcast<u32>(f32(x))
    let z = alignOf<Packet>() + i32(y)
    return f32(u8(1)) + bitcast<f32>(u)
}
fn main() {
    var xs: [u16] = [1, 2, 3]
    var mp = mapNew<u32, i8>()
    mapSet(mut mp, u32(7), i8(-5))
    let ok = Header { kind: i8(1), size: u16(4), bits: f32(2.5) }
    print(str(ok.size) + " " + str(conv(1.5, u64(0))))
}''',
        '''@link("kernel32")
extern "C" {
    fn GetCurrentProcessId() -> u32
    fn my_c_add(a: i32, b: i32) -> i32
}
trusted extern "C" fn cmp_i32(a: Ptr<void>, b: Ptr<void>) -> i32 {
    return i32(0)
}
raw fn fastRead(p: Ptr<u32>) -> u32 {
    trusted {
        if !ptrIsNull(p) { return ptrRead(p) }
    }
    return u32(0)
}
verified fn safeOps() {
    var x: i32 = 10
    let cp: Ptr<i32> = ptrOf(x)
    let cb = fnPtr(cmp_i32)
    let rawBytes = cstr("ok")
    raw {
        let p = mutPtrOf(mut x)
        ptrWrite(p, i32(20))
        let pid = GetCurrentProcessId()
        let sum = my_c_add(i32(pid), i32(1))
    }
}''',
        '''struct Simple {
    v: Simd<f32, 4>
}
fn useSimd(x: f32, y: u32) -> f32 {
    let a: Simd<f32, 4> = simdSplat<Simd<f32, 4>>(x)
    let bits = popcnt(rotl(clz(y), u32(1)))
    spinHint()
    return simdSum(a) + f32(bits)
}''',
        '''fn lockFreeAndRegion() -> int {
    let a: Atomic<int> = atomicNew(1)
    let q: RingQueue<int> = ringNew<int>(8)
    var r: Region = regionNew(256)
    let _ = ringPush(q, atomicAddOrd(a, 2, "acqRel"))
    atomicFence("seqCst")
    trusted {
        let p = regionAlloc<i32>(mut r, 4)
        ptrWrite(p, i32(9))
        regionReset(mut r)
    }
    return atomicLoad(a) + ringLen(q) + regionUsed(r)
}''',
        '''interface Bounded {
    fn at(self, i: int) -> int
        requires i >= 0
        ensures result >= 0
}
struct Counter { val: int }
impl Counter {
    fn inc(self, d: int) -> int requires d > 0 ensures result > self.val {
        return self.val + d
    }
}
raw fn fastDiv(a: int, b: int) -> int requires b > 0 { return a / b }
fn getAt(a: [int], i: int) -> int
    requires i >= 0 && i < len(a)
    ensures result == a[i]
{
    return a[i]
}''',
        '''@noCopy
@arena
fn aiExprAndUfcs(
    a: int,
    b: int,
) -> int {
    let pick = if a > b {
        let diff = a - b
        diff
    } else if a == b {
        1
    } else {
        0
    }
    var sb = sbNew()
    sb.push("v")
    print(
        sb.str(),
    )
    return pick
}''',
    ]
    invalid = [
        "fn u8() {}", "fn f32() {}", "@repr(Rust)\nstruct Bad { x: u8 }",
        "@link(\"foo\")\nstruct BadLink { x: int }",
        "@link(\"foo\")\nextern \"C\" fn badFn() {}",
        "extern \"Rust\" { fn nope() -> int }",
        "extern \"C\" { fn hasBody() { print(1) } }",
        "fn badSimd(v: Simd<f32, x>) {}",
        "fn badSimd2(v: Simd<f32, -4>) {}",
        "fn main() { let a = 7 let b = 4 }", "fn main() { let a = 2; }",
        "fn main() { print(7\n + 3) }", "fnmain() {}", "fn true() {}", "fn struct() {}",
        "fn elif() {}", "fn main() { let let = 3 }", "fn a() {} fn b() {}",
        "fn main() { let a_b = 3 }", "fn main() { whiletrue {} }",
        "fn main() { for xin 0..5 {} }", "fn main() { print(1//1) }",
        'fn main() { let x = "bad\\m" }', 'struct point {}', 'enum Tree { leaf }',
        'fn id<t>(x: t) -> t { return x }', 'fn f(x: Event) { match x { Event.A -> print(1) } }',
        'fn a() { print(Pair { x: 1 y: 2 }) }',
        'fn main() { var a=repeat(1,1)\na[]=0 }', 'fn f(a: [int) {}',
        'test "interp {x}" {}', 'fn main() { let s = "unclosed {x" }', 'fn main() { let s = "bad }" }',
    ]
    tokenizer = LLTokenizer(TokenizerWrapper(ByteVocabulary()))
    samples = [(s, True) for s in valid] + [(s, False) for s in invalid]
    grammar_text = (GRAMMAR / "turi.lark").read_text(encoding="utf-8")
    parser = Lark(grammar_text, parser="earley", lexer="dynamic")
    for source, expected in samples:
        try:
            parser.parse(source)
            accepted = True
        except UnexpectedInput:
            accepted = False
        assert accepted == expected, ("Python Lark", expected, source)
    for grammar_format in ("lark", "gbnf"):
        text = (GRAMMAR / f"turi.{grammar_format}").read_text(encoding="utf-8")
        grammar = grammar_from(grammar_format, text)
        for source, expected in samples:
            encoded = list(source.encode("utf-8")) + [tokenizer.eos_token]
            matcher = LLMatcher(tokenizer, grammar)
            assert not matcher.is_error(), matcher.get_error()
            accepted = matcher.validate_tokens(encoded) == len(encoded)
            assert accepted == expected, (grammar_format, expected, source)
    with tempfile.TemporaryDirectory(prefix="turi-gramatica-") as folder:
        file = Path(folder) / "edición.turi"
        for source, expected in samples:
            original = source.encode("utf-8")
            file.write_bytes(original)
            result = subprocess.run([*formatter, file], capture_output=True, timeout=5)
            assert (result.returncode == 0) == expected, (expected, source, result.stderr)
            if expected:
                pretty = file.read_bytes()
                fixed = subprocess.run([*formatter, file, "--check"], capture_output=True, timeout=5)
                assert fixed.returncode == 0 and pretty == file.read_bytes(), (source, fixed.stderr)
                parser.parse(pretty.decode("utf-8"))
            else:
                assert file.read_bytes() == original and json.loads(result.stderr)["id"]
        for raw in [b'fn main() { print("\xff") }', b'// \xc0\xaf', b'// \xed\xa0\x80', b'// \xf4\x90\x80\x80']:
            file.write_bytes(raw)
            result = subprocess.run([*formatter, file], capture_output=True, timeout=5)
            assert result.returncode and json.loads(result.stderr)["id"] == "E_UTF8", result.stderr
            assert file.read_bytes() == raw
    print(f"Parser, gramáticas y fmt verificados. {len(valid)} válidos, {len(invalid)} inválidos y 4 errores UTF-8.")


if __name__ == "__main__":
    main()
