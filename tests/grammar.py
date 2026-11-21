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
    ]
    invalid = [
        "fn main() { let a = 7 let b = 4 }", "fn main() { let a = 2; }",
        "fn main() { print(7\n + 3) }", "fnmain() {}", "fn true() {}", "fn struct() {}",
        "fn elif() {}", "fn main() { let let = 3 }", "fn a() {} fn b() {}",
        "fn main() { let a_b = 3 }", "fn main() { whiletrue {} }",
        "fn main() { for xin 0..5 {} }", "fn main() { print(1//1) }",
        'fn main() { let x = "bad\\m" }', 'struct point {}', 'enum Tree { leaf }',
        'fn id<t>(x: t) -> t { return x }', 'fn f(x: Event) { match x { Event.A -> print(1) } }',
        'fn a() { print(Pair { x: 1 y: 2 }) }',
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
            result = subprocess.run([cli, "fmt", file], capture_output=True, timeout=5)
            assert (result.returncode == 0) == expected, (expected, source, result.stderr)
            if expected:
                pretty = file.read_bytes()
                fixed = subprocess.run([cli, "fmt", file, "--check"], capture_output=True, timeout=5)
                assert fixed.returncode == 0 and pretty == file.read_bytes(), (source, fixed.stderr)
                parser.parse(pretty.decode("utf-8"))
            else:
                assert file.read_bytes() == original and json.loads(result.stderr)["id"]
        for raw in [b'fn main() { print("\xff") }', b'// \xc0\xaf', b'// \xed\xa0\x80', b'// \xf4\x90\x80\x80']:
            file.write_bytes(raw)
            result = subprocess.run([cli, "fmt", file], capture_output=True, timeout=5)
            assert result.returncode and json.loads(result.stderr)["id"] == "E_UTF8", result.stderr
            assert file.read_bytes() == raw
    print(f"Parser, gramáticas y fmt verificados. {len(valid)} válidos, {len(invalid)} inválidos y 4 errores UTF-8.")


if __name__ == "__main__":
    main()
