"""Pruebas funcionales, de memoria y diferenciales del hito 2.2B de Turi 0.3."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True):
    return subprocess.run(args, capture_output=True, text=text, encoding="utf-8" if text else None, env=env)


def run_both(cli: Path, source: str, workdir: Path, run_args=(), env=None, expect_trap=False, check_fmt=True):
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], env=env)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], env=env)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    interp_cmd = [str(cli), "run", str(src)]
    if run_args:
        interp_cmd.append("--")
        interp_cmd.extend(run_args)
    interp = run_cmd(interp_cmd, env=env)

    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], env=env)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"

    nat = run_cmd([str(exe), *run_args], env=env)

    assert interp.returncode == nat.returncode, (
        f"rc mismatch: interp={interp.returncode} native={nat.returncode}\n"
        f"interp stderr: {interp.stderr}\nnative stderr: {nat.stderr}\n{source}"
    )
    if expect_trap:
        assert interp.returncode != 0, f"expected trap, got rc=0\n{source}"
        i_err = json.loads(interp.stderr.strip().splitlines()[-1])
        n_err = json.loads(nat.stderr.strip().splitlines()[-1])
        assert i_err["id"] == n_err["id"], f"trap id mismatch: {i_err} vs {n_err}\n{source}"
        return i_err["id"], ""
    assert interp.returncode == 0, f"unexpected error rc={interp.returncode}\n{interp.stderr}\n{source}"
    assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnative={nat.stdout!r}\n{source}"
    assert interp.stderr == nat.stderr, f"stderr mismatch:\ninterp={interp.stderr!r}\nnative={nat.stderr!r}\n{source}"
    return interp.stdout, interp.stderr


def expect_diag(cli: Path, source: str, workdir: Path, expected_id: str):
    src = workdir / "diag.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    r = run_cmd([str(cli), "check", str(src)])
    assert r.returncode != 0, f"expected compile error {expected_id}, got OK\n{source}"
    line = r.stderr.strip().splitlines()[-1]
    d = json.loads(line)
    assert d["id"] == expected_id, f"expected {expected_id}, got {d}\n{source}"
    return d


def test_german_strings_cmp_concat(cli: Path, workdir: Path):
    src = """
const prefixStr: str = "const-str-123456"

fn makeShort() -> str {
    return "ab" + "cd"
}

fn makeTwelve() -> str {
    return "123456" + "789012"
}

fn makeThirteen() -> str {
    return "123456" + "7890123"
}

fn main() {
    let empty = ""
    let one = "a"
    let four = makeShort()
    let twelve = makeTwelve()
    let thirteen = makeThirteen()
    let utf8 = "áéíóúñ Perú 日本"

    print(len(empty))
    print(len(one))
    print(len(four))
    print(len(twelve))
    print(len(thirteen))
    print(len(utf8) > 12)

    print(four == "abcd")
    print(four != "abce")
    print(twelve == "123456789012")
    print(thirteen == "1234567890123")
    print("abcd" + "" == four)
    print("" + thirteenthFallback(thirteen) == thirteen)

    print("a" < "b")
    print("abc" < "abcd")
    print("abcd1" < "abcd2")
    print("abcdefghijkl1" < "abcdefghijkl2")
    print("abcdefghijkl2" > "abcdefghijkl1")
    print("abcdefghijkl1" <= "abcdefghijkl1")
    print("abcdefghijkl1" >= "abcdefghijkl1")
    print("z" < "á")
    print(prefixStr)

    var s = "hi"
    s += " there"
    s += " from turi03"
    print(s)
    print(len(s))
}

fn thirteenthFallback(x: str) -> str {
    return x
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "0\n1\n4\n12\n13\ntrue\n"
        "true\ntrue\ntrue\ntrue\ntrue\ntrue\n"
        "true\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\ntrue\n"
        "const-str-123456\n"
        "hi there from turi03\n20\n"
    ), out


def test_bytes_and_slices_lifetime(cli: Path, workdir: Path):
    src = """
fn makeOwned() -> str {
    return "alpha-" + "beta-gamma-delta"
}

fn takeLongSlice() -> str {
    let tmp = makeOwned()
    let cut = tmp[2..18]
    return cut
}

fn takeShortSlice(s: str) -> str {
    return s[1..5]
}

fn main() {
    let inline = "Hola"
    print(inline[0])
    print(inline[3])

    let utf8 = "ñ"
    print(utf8[0])
    print(utf8[1])

    let longLit = "0123456789abcdef"
    print(longLit[0])
    print(longLit[15])
    print(longLit[4..12])
    print(longLit[4..12] == "456789ab")
    print(longLit[2..16])
    print(longLit[3..3] == "")

    let slicedOwned = takeLongSlice()
    print(slicedOwned)
    print(takeShortSlice("abcdef"))
    print(takeShortSlice(longLit))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "72\n97\n195\n177\n48\n102\n"
        "456789ab\ntrue\n23456789abcdef\ntrue\n"
        "pha-beta-gamma-d\nbcde\n1234\n"
    ), out

    for bad_prog in [
        'fn main() {\n    let s = "abc"\n    print(s[3])\n}',
        'fn main() {\n    let s = "abc"\n    print(s[-1])\n}',
        'fn main() {\n    let s = ""\n    print(s[0])\n}',
        'fn main() {\n    let s = "0123456789abcdef"\n    print(s[16])\n}',
        'fn main() {\n    let s = "abcdef"\n    print(s[4..2])\n}',
        'fn main() {\n    let s = "abcdef"\n    print(s[0..7])\n}',
        'fn main() {\n    let s = "abcdef"\n    print(s[-1..2])\n}',
    ]:
        trap_id, _ = run_both(cli, bad_prog, workdir, expect_trap=True)
        assert trap_id == "R_BOUNDS", bad_prog


def test_interpolation(cli: Path, workdir: Path):
    src = """
fn mkOwned(n: int) -> str {
    return "owned-value-" + str(n)
}

fn minInt() -> int {
    return -9223372036854775807 - 1
}

fn main() {
    let a = 6
    let b = 7
    let flag = true
    let xs = ["cero", "uno", "dos"]
    let x = 2.5
    let m = minInt()

    print("sum={a + b * 2}, bool={flag}, item={xs[1]}, f={x}")
    print("min={m}, neg={-a}, fbool={false}")
    print("esc=\\{x\\} {{raw}} \\"q\\"\\t\\nline2")
    print("nested={"inner=" + str(a) + "-{b * 3}"}")
    print("owned={mkOwned(99)}!")
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "sum=20, bool=true, item=uno, f=2.5\n"
        "min=-9223372036854775808, neg=-6, fbool=false\n"
        "esc={x} {raw} \"q\"\t\nline2\n"
        "nested=inner=6-21\n"
        "owned=owned-value-99!\n"
    ), out

    expect_diag(cli, 'test "interp {1}" {}', workdir, "E_PARSE")
    expect_diag(cli, 'fn main() { let s = "bad {1" }', workdir, "E_STRING")
    expect_diag(cli, 'fn main() { let s = "bad }" }', workdir, "E_STRING")
    expect_diag(cli, 'struct P { x: int }\nfn main() {\n    let p = P { x: 1 }\n    print("{p}")\n}', workdir, "E_TYPE")


def test_parse_and_search(cli: Path, workdir: Path):
    src = """
fn dynNum() -> str {
    return "1000000" + "000000000"
}

fn dynText() -> str {
    return "prefix-needle-" + "suffix-end"
}

fn main() {
    print(str(0))
    print(str(-42))
    print(str(1.25))
    print(str(true))
    print(str("keep-same-string"))
    print(strFromByte(65))
    print(strFromByte(321))

    print(parseInt("0"))
    print(parseInt("+19"))
    print(parseInt("-42"))
    print(parseInt(dynNum()))
    print(parseInt("-9223372036854775808"))
    print(tryParseInt(" 12", -1))
    print(tryParseInt("9223372036854775808", 77))
    print(tryParseInt("1234", -1))

    print(parseFloat("3.5") + parseFloat("-1.25"))
    print(parseFloat("1e3"))
    print(tryParseFloat("bad", 9.5))
    print(tryParseFloat("4.25", 0.0))

    let h = dynText()
    print(strContains(h, ""))
    print(strContains("", ""))
    print(strContains(h, "needle"))
    print(strContains(h, "missing"))
    print(strStartsWith(h, "prefix-"))
    print(strStartsWith("ab", "abcd"))
    print(strEndsWith(h, "-end"))
    print(strEndsWith("ab", "abcd"))
    print(strIndexOf(h, ""))
    print(strIndexOf(h, "needle"))
    print(strIndexOf(h, "end"))
    print(strIndexOf(h, "nope"))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "0\n-42\n1.25\ntrue\nkeep-same-string\nA\nA\n"
        "0\n19\n-42\n1000000000000000\n-9223372036854775808\n"
        "-1\n77\n1234\n"
        "2.25\n1000\n9.5\n4.25\n"
        "true\ntrue\ntrue\nfalse\ntrue\nfalse\ntrue\nfalse\n0\n7\n21\n-1\n"
    ), out

    for bad_prog in [
        'fn main() { print(parseInt("")) }',
        'fn main() { print(parseInt("-")) }',
        'fn main() { print(parseInt("12x")) }',
        'fn main() { print(parseInt("9223372036854775808")) }',
        'fn main() { print(parseInt("-9223372036854775809")) }',
        'fn main() { print(parseFloat("")) }',
        'fn main() { print(parseFloat(" 1.5")) }',
        'fn main() { print(parseFloat("1.5x")) }',
    ]:
        trap_id, _ = run_both(cli, bad_prog, workdir, expect_trap=True)
        assert trap_id == "R_PARSE", bad_prog


def test_string_builder(cli: Path, workdir: Path):
    src = """
struct BufWrap {
    name: str,
    buf: StringBuilder
}

fn appendKV(sb: mut StringBuilder, k: str, v: int) {
    sbPush(mut sb, k)
    sbPushByte(mut sb, 61)
    sbPushInt(mut sb, v)
}

fn main() {
    var sb: StringBuilder = sbNew()
    print(len(sb))
    print(cap(sb))
    sbPush(mut sb, "hi")
    print(sbStr(sb))
    print(len(sb))

    let takenShort = sbTake(mut sb)
    print(takenShort)
    print(len(sb))

    var sb2 = sbWithCap(4)
    print(cap(sb2) >= 4)
    appendKV(mut sb2, "alpha", 10)
    sbPushByte(mut sb2, 44)
    sbPushFloat(mut sb2, 3.5)
    sbPush(mut sb2, ",done-tail")
    let snap = sbStr(sb2)
    print(snap)
    print(len(sb2))
    let cBefore = cap(sb2)
    sbClear(mut sb2)
    print(len(sb2))
    print(cap(sb2) == cBefore)
    sbPush(mut sb2, "reused-long-builder-output")
    let takenLong = sbTake(mut sb2)
    print(takenLong)
    print(len(sb2))

    var initWrap = sbNew()
    sbPush(mut initWrap, "inside-struct")
    let wrap = BufWrap { name: "ok", buf: initWrap }
    let wrapCopy = wrap
    print(sbStr(wrapCopy.buf))

    var list: [StringBuilder] = []
    var item = sbNew()
    sbPush(mut item, "first-element-in-array")
    push(mut list, item)
    let copyList = list
    print(sbStr(copyList[0]))
    print(sbStr(list[0]))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "0\n0\nhi\n2\nhi\n0\n"
        "true\nalpha=10,3.5,done-tail\n22\n0\ntrue\n"
        "reused-long-builder-output\n0\n"
        "inside-struct\nfirst-element-in-array\nfirst-element-in-array\n"
    ), out

    trap_id, _ = run_both(cli, "fn main() { let s = sbWithCap(-1) }", workdir, expect_trap=True)
    assert trap_id == "R_ARRAY_SIZE", trap_id
    trap_id, _ = run_both(cli, "fn main() { let s = sbWithCap(2000000000) }", workdir, expect_trap=True)
    assert trap_id == "R_ALLOC", trap_id


def test_containers_ownership_and_diags(cli: Path, workdir: Path):
    src = """
struct Entry {
    key: str,
    vals: [str]
}

enum Node {
    Leaf(text: str),
    Group(title: str, items: [Node])
}

fn render(n: Node) -> str {
    match n {
        Node.Leaf(t) => return "L:" + t
        Node.Group(title, items) => {
            var out = sbNew()
            sbPush(mut out, title)
            for item in items {
                sbPushByte(mut out, 124)
                sbPush(mut out, render(item))
            }
            return sbTake(mut out)
        }
    }
}

fn cloneOwnedStr(n: int) -> str {
    let orig = "owned-str-value-" + str(n)
    let copy = str(orig)
    return copy
}

fn main() {
    var xs: [str] = []
    var i = 0
    while i < 5 {
        push(mut xs, "element-number-" + str(i))
        i += 1
    }
    var forAcc = 0
    for s in xs {
        forAcc += len(s)
    }
    print(forAcc)
    print(cloneOwnedStr(7))
    var ys = xs
    xs[0] = "mutated-element-zero"
    print(xs[0])
    print(ys[0])
    print(pop(mut ys))
    clear(mut xs)
    print(len(xs))
    print(len(ys))

    var e = Entry { key: "header-long-key", vals: ["val-long-one-123", "v2"] }
    let eCopy = e
    e.key = "updated-long-key"
    e.vals[0] = "changed-val-one"
    print(eCopy.key)
    print(eCopy.vals[0])

    let tree = Node.Group("root-group-name", [
        Node.Leaf("first-leaf-longer-than-12"),
        Node.Leaf("second")
    ])
    print(render(tree))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "80\nowned-str-value-7\n"
        "mutated-element-zero\nelement-number-0\nelement-number-4\n0\n4\n"
        "header-long-key\nval-long-one-123\n"
        "root-group-name|L:first-leaf-longer-than-12|L:second\n"
    ), out

    expect_diag(cli, 'fn main() {\n    let sb = sbNew()\n    sbPush(mut sb, "x")\n}', workdir, "E_IMMUTABLE")
    expect_diag(
        cli,
        'struct Wrap { buf: StringBuilder }\nfn main() {\n    let w = Wrap { buf: sbNew() }\n    sbPush(mut w.buf, "x")\n}',
        workdir,
        "E_IMMUTABLE",
    )
    # `str + escalar` convierte el escalar con str() (como JS, Java o Kotlin); otros tipos siguen siendo error.
    concat = workdir / "concat.turi"
    concat.write_text('fn main() {\n    print("a" + 1 + true)\n}\n', encoding="utf-8", newline="\n")
    concat_run = run_cmd([str(cli), "run", str(concat)])
    assert concat_run.returncode == 0 and concat_run.stdout.strip() == "a1true", concat_run.stderr
    expect_diag(cli, 'fn main() {\n    let x = "a" + [1]\n}', workdir, "E_TYPE")
    expect_diag(cli, 'fn main() {\n    print(sbNew() == sbNew())\n}', workdir, "E_EQUALITY")
    expect_diag(cli, 'fn main() {\n    print(parseInt(123))\n}', workdir, "E_TYPE")
    expect_diag(cli, 'struct S { x: int }\nfn main() {\n    print(str(S { x: 1 }))\n}', workdir, "E_TYPE")
    expect_diag(cli, 'struct StringBuilder { x: int }\nfn main() {}', workdir, "E_DUP_TYPE")
    d_fstr = expect_diag(cli, 'fn main() {\n    let x = 1\n    let s = f"x={x}"\n}', workdir, "E_FSTRING")
    assert d_fstr["fix"]["replacement"] == "", d_fstr
    expect_diag(cli, 'fn main() {\n    let x = 1\n    let s = `x=${x}`\n}', workdir, "E_CHAR")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Uso: python tests/turi03_m2b.py RUTA_A_VELOZ")
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m2b-") as tmp:
        workdir = Path(tmp)
        test_german_strings_cmp_concat(cli, workdir)
        test_bytes_and_slices_lifetime(cli, workdir)
        test_interpolation(cli, workdir)
        test_parse_and_search(cli, workdir)
        test_string_builder(cli, workdir)
        test_containers_ownership_and_diags(cli, workdir)


if __name__ == "__main__":
    main()
