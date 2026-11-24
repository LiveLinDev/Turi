"""Pruebas funcionales, de memoria, paquetes y diferenciales del Hito 2.3 de Turi 0.3."""
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
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
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


def test_optionals(cli: Path, workdir: Path):
    src = """
struct Holder {
    label: str,
    optNum: int?,
    optText: str?
}

fn findPos(n: int) -> int? {
    if n > 0 {
        return Some(n * 10)
    }
    return None
}

fn maybeOwned(flag: bool) -> str? {
    if flag {
        return Some("heap-owned-optional-" + str(42))
    }
    return None
}

fn mkFallback(tag: str) -> str {
    return "fallback-owned-" + tag
}

fn takeArrayOpt(flag: bool) -> [int]? {
    if flag {
        return Some([4, 5, 6])
    }
    return None
}

fn main() {
    let a: int? = findPos(3)
    let b: Option<int> = findPos(0)
    print(isSome(a))
    print(isNone(a))
    print(isSome(b))
    print(isNone(b))
    print(unwrapOr(a, 99))
    print(unwrapOr(b, 99))

    match a {
        Some(v) => print(v)
        None => print(-1)
    }
    match b {
        Option<int>.Some(v) => print(v)
        Option<int>.None => print(-2)
    }

    var sOpt: str? = Some("initial-long-owned-" + str(1))
    print(unwrapOr(sOpt, mkFallback("unused")))
    sOpt = None
    print(unwrapOr(sOpt, mkFallback("used")))
    print(unwrapOr(maybeOwned(true), mkFallback("skip")))
    print(unwrapOr(maybeOwned(false), mkFallback("take")))

    let h = Holder { label: "ok", optNum: Some(7), optText: None }
    print(unwrapOr(h.optNum, 0))
    print(unwrapOr(h.optText, "default"))

    let arr1 = unwrapOr(takeArrayOpt(true), [9])
    let arr2 = unwrapOr(takeArrayOpt(false), [9, 8])
    print(len(arr1))
    print(arr1[2])
    print(len(arr2))
    print(arr2[1])

    let listOpts = [Some(11), None, Some(22)]
    let emptyOpts: [int?] = [None]
    print(unwrapOr(listOpts[0], 0) + unwrapOr(listOpts[1], 5) + unwrapOr(emptyOpts[0], 3))

    let nested1: int?? = Some(Some(123))
    let nested2: int?? = Some(None)
    let nested3: int?? = None
    print(isSome(nested1))
    print(unwrapOr(unwrapOr(nested1, None), 0))
    print(isSome(nested2))
    print(isSome(unwrapOr(nested2, None)))
    print(isNone(nested3))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "true\nfalse\nfalse\ntrue\n30\n99\n"
        "30\n-2\n"
        "initial-long-owned-1\nfallback-owned-used\n"
        "heap-owned-optional-42\nfallback-owned-take\n"
        "7\ndefault\n"
        "3\n6\n2\n8\n"
        "19\n"
        "true\n123\ntrue\nfalse\ntrue\n"
    ), out


def test_result_and_try(cli: Path, workdir: Path):
    src = """
struct Config {
    port: int,
    host: str
}

fn parsePort(s: str) -> Result<int, str> {
    let v = tryParseInt(s, -1)
    if v <= 0 {
        return Err("invalid-long-error-msg-" + s)
    }
    return Ok(v)
}

fn parseHost(s: str) -> Result<str, str> {
    if len(s) == 0 {
        return Err("empty-host-long-error-msg")
    }
    return Ok("host-" + s)
}

fn loadConfig(portText: str, hostText: str) -> Result<Config, str> {
    let keepLocal = "owned-local-before-try-" + hostText
    let keepArr = [10, 20, 30]
    let p = parsePort(portText)?
    let h = parseHost(hostText)?
    return Ok(Config { port: p + keepArr[0] + len(keepLocal) - len(keepLocal), host: h })
}

fn sumPorts(a: str, b: str) -> Result<int, str> {
    return Ok(parsePort(a)? + parsePort(b)?)
}

fn findIndexOpt(ok: bool) -> int? {
    if ok {
        return Some(1)
    }
    return None
}

fn tryChainOpt(ok: bool) -> int? {
    let xs = [100, 200]
    let v = xs[findIndexOpt(ok)?]
    return Some(v + 5)
}

fn validatePositive(n: int) -> Result<int, str> {
    if n > 0 {
        return Ok(n)
    }
    return Err("must-be-positive-long-msg")
}

fn checkTwo(a: int, b: int) -> Result<int, str> {
    validatePositive(a)?
    let r = validatePositive(b)?
    return Ok(r)
}

fn nestedOpt(n: int) -> int?? {
    if n == 0 {
        return None
    }
    if n == 1 {
        return Some(None)
    }
    return Some(Some(n * 3))
}

fn unwrapNestedOpt(n: int) -> int? {
    return Some(nestedOpt(n)??)
}

fn mkLongResult(ok: bool) -> Result<str, [int]> {
    if ok {
        return Ok("success-owned-long-payload")
    }
    return Err([7, 8, 9])
}

fn unwrapViaTry(ok: bool) -> Result<str, [int]> {
    let s = unwrapOr(mkLongResult(ok), "fb-" + str(parseOk(ok)?))
    return Ok(s)
}

fn parseOk(ok: bool) -> Result<int, [int]> {
    if ok {
        return Ok(55)
    }
    return Err([99, 100])
}

fn main() {
    let c1 = loadConfig("8080", "localhost")
    print(isOk(c1))
    print(isErr(c1))
    match c1 {
        Ok(cfg) => print("{cfg.host}:{cfg.port}")
        Err(e) => print("ERR:{e}")
    }
    match loadConfig("0", "extra-long-host") {
        Result<Config, str>.Ok(cfg) => print(cfg.port)
        Result<Config, str>.Err(e) => print("ERR:{e}")
    }
    match loadConfig("80", "") {
        Ok(cfg) => print(cfg.port)
        Err(e) => print("ERR:{e}")
    }

    print(unwrapOr(sumPorts("12", "30"), -1))
    print(unwrapOr(sumPorts("12", "bad"), -1))

    print(unwrapOr(tryChainOpt(true), -9))
    print(unwrapOr(tryChainOpt(false), -9))

    print(unwrapOr(checkTwo(5, 9), -1))
    print(unwrapOr(checkTwo(-1, 9), -1))

    print(unwrapOr(unwrapNestedOpt(0), -10))
    print(unwrapOr(unwrapNestedOpt(1), -10))
    print(unwrapOr(unwrapNestedOpt(4), -10))

    print(unwrapOr(mkLongResult(true), "fallback-unused-long-text"))
    print(unwrapOr(mkLongResult(false), "fallback-used-long-text"))
    print(unwrapOr(unwrapViaTry(true), "none"))
    match unwrapViaTry(false) {
        Ok(v) => print(v)
        Err(arr) => print("{len(arr)}:{arr[0]}:{arr[1]}")
    }
}

test "? funciona dentro de test" {
    let c = loadConfig("80", "local")?
    assert c.port == 90
    assert c.host == "host-local"
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "true\nfalse\nhost-localhost:8090\n"
        "ERR:invalid-long-error-msg-0\n"
        "ERR:empty-host-long-error-msg\n"
        "42\n-1\n"
        "205\n-9\n"
        "9\n-1\n"
        "-10\n-10\n12\n"
        "success-owned-long-payload\n"
        "fallback-used-long-text\n"
        "success-owned-long-payload\n"
        "2:99:100\n"
    ), out

    chk = run_cmd([str(cli), "check", str(workdir / "main.turi")])
    assert chk.returncode == 0, chk.stderr

    bad_test = """
fn failOpt() -> int? {
    return None
}

test "falla ?" {
    let x = failOpt()?
    assert x == 1
}
"""
    test_file = workdir / "fail_test.turi"
    test_file.write_text(bad_test, encoding="utf-8", newline="\n")
    tr = run_cmd([str(cli), "check", str(test_file)])
    assert tr.returncode != 0
    assert json.loads(tr.stderr.strip().splitlines()[-1])["id"] == "R_ASSERT"


def test_generic_structs_enums(cli: Path, workdir: Path):
    src = """
struct Stack<T> {
    items: [T]
}

struct Pair<A, B> {
    first: A
    second: B
}

struct Box<T> {
    val: T
}

enum Tree<T> {
    Empty
    Node(val: T, kids: [Tree<T>])
}

enum Side<T> {
    Left
    Right
}

fn sumTree(t: Tree<int>) -> int {
    match t {
        Tree.Empty => return 0
        Tree.Node(v, kids) => {
            var acc = v
            for k in kids {
                acc += sumTree(k)
            }
            return acc
        }
    }
}

fn joinTree(t: Tree<str>) -> str {
    match t {
        Tree<str>.Empty => return ""
        Tree<str>.Node(v, kids) => {
            var out = sbNew()
            sbPush(mut out, v)
            for k in kids {
                sbPushByte(mut out, 47)
                sbPush(mut out, joinTree(k))
            }
            return sbTake(mut out)
        }
    }
}

fn wrapStack<T>(x: T) -> Stack<T> {
    return Stack<T> { items: [x] }
}

fn pushStackStr(s: mut Stack<str>, item: str) {
    var items = s.items
    push(mut items, item)
    s.items = items
}

fn firstPair<A, B>(p: Pair<A, B>) -> A {
    return p.first
}

fn nestedClose(flag: bool) -> Result<Stack<int>, Box<Box<Box<str>>>> {
    if flag {
        return Ok(Stack<int> { items: [1, 2, 3] })
    }
    return Err(Box { val: Box { val: Box { val: "deep-triple-angle-msg" } } })
}

fn main() {
    var st = Stack { items: ["alpha-long-item-1"] }
    pushStackStr(mut st, "beta-long-item-2")
    let stCopy = st
    var stItems = st.items
    push(mut stItems, "gamma-long-item-3")
    st.items = stItems
    print(len(st.items))
    print(len(stCopy.items))
    print(stCopy.items[1])

    let ws = wrapStack("wrapped-owned-string")
    print(ws.items[0])

    let p = Pair { first: 42, second: "pair-text" }
    print(firstPair(p))
    print(p.second)

    let treeInt = Tree.Node(10, [
        Tree.Node(3, []),
        Tree.Node(7, [Tree.Empty, Tree.Node(5, [])])
    ])
    print(sumTree(treeInt))

    let treeStr = Tree.Node("root-node-long", [
        Tree.Node("child-a-longer", []),
        Tree.Node("child-b-longer", [])
    ])
    print(joinTree(treeStr))

    print(Side<int>.Left == Side<int>.Left)
    print(Side<int>.Left != Side<int>.Right)

    match nestedClose(true) {
        Ok(s) => print(len(s.items))
        Err(b) => print(b.val.val.val)
    }
    match nestedClose(false) {
        Ok(s) => print(len(s.items))
        Err(b) => print(b.val.val.val)
    }
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out == (
        "3\n2\nbeta-long-item-2\n"
        "wrapped-owned-string\n"
        "42\npair-text\n"
        "25\n"
        "root-node-long/child-a-longer/child-b-longer\n"
        "true\ntrue\n"
        "3\n"
        "deep-triple-angle-msg\n"
    ), out

    sig = run_cmd([str(cli), "sig", str(workdir / "main.turi"), "Stack"])
    assert sig.returncode == 0, sig.stderr
    assert "struct Stack<T>" in sig.stdout, sig.stdout

    types_out = run_cmd([str(cli), "types", str(workdir / "main.turi"), "sumTree"])
    assert types_out.returncode == 0, types_out.stderr
    assert "Tree<int>" in types_out.stdout, types_out.stdout


def test_multi_module_package(cli: Path, workdir: Path):
    pkg_dir = workdir / "pkg_m23"
    src_dir = pkg_dir / "src"
    src_dir.mkdir(parents=True, exist_ok=True)
    (pkg_dir / "veloz.pkg").write_text("package app\nversion 1.0.0\n", encoding="utf-8", newline="\n")
    (src_dir / "repo.turi").write_text("""
struct Box<T> {
    val: T
}

enum Outcome<T, E> {
    Ready(value: T),
    Failed(reason: E)
}

fn wrapText(ok: bool) -> Box<str>? {
    if ok {
        return Some(Box { val: "module-box-owned-text" })
    }
    return None
}

fn load(ok: bool) -> Result<Box<str>, str> {
    if ok {
        return Ok(Box { val: "loaded-from-repo-module" })
    }
    return Err("repo-module-failure-msg")
}
""", encoding="utf-8", newline="\n")

    main_file = src_dir / "main.turi"
    main_file.write_text("""
import repo

fn readText(ok: bool) -> Result<str, str> {
    let b = repo.load(ok)?
    return Ok(b.val)
}

fn main() {
    let b = repo.Box { val: 77 }
    print(b.val)
    match repo.wrapText(true) {
        Some(bx) => print(bx.val)
        None => print("none")
    }
    match repo.Outcome<int, str>.Ready(19) {
        repo.Outcome.Ready(v) => print(v)
        repo.Outcome.Failed(e) => print(e)
    }
    print(unwrapOr(readText(true), "bad"))
    print(unwrapOr(readText(false), "fallback-from-main"))
}
""", encoding="utf-8", newline="\n")

    chk = run_cmd([str(cli), "check", str(main_file)])
    assert chk.returncode == 0, chk.stderr
    interp = run_cmd([str(cli), "run", str(main_file)])
    assert interp.returncode == 0, interp.stderr
    exe = workdir / ("pkg_m23" + (".exe" if os.name == "nt" else ""))
    bld = run_cmd([str(cli), "build", str(main_file), "-o", str(exe)])
    assert bld.returncode == 0, bld.stderr
    nat = run_cmd([str(exe)])
    assert nat.returncode == 0 and nat.stdout == interp.stdout, (nat.stdout, interp.stdout, nat.stderr)
    assert interp.stdout == (
        "77\nmodule-box-owned-text\n19\nloaded-from-repo-module\nfallback-from-main\n"
    ), interp.stdout


def test_diagnostics_and_false_friends(cli: Path, workdir: Path):
    for bad in ("null", "nil", "undefined"):
        d = expect_diag(cli, f"fn main() {{\n    let x: int? = {bad}\n}}", workdir, "E_NULL")
        assert d["fix"]["replacement"] == "None", d

    expect_diag(cli, "fn main() {\n    throw 1\n}", workdir, "E_EXCEPTIONS")
    expect_diag(cli, "fn main() {\n    try {\n        print(1)\n    }\n}", workdir, "E_EXCEPTIONS")
    expect_diag(cli, "fn main() {\n    let x = 1 == 1 ?\n}", workdir, "E_TRY_TYPE")
    expect_diag(cli, "fn opt() -> int? { return Some(1) }\nfn f() -> int { return opt()? }\nfn main() {}", workdir, "E_TRY_RETURN")
    expect_diag(cli, "fn f() -> Result<int, int> { return Ok(1) }\nfn g() -> Result<int, str> { return Ok(f()?) }\nfn main() {}", workdir, "E_TRY_ERR")

    expect_diag(cli, "fn main() {\n    let x = None\n}", workdir, "E_INFER_TYPE")
    expect_diag(cli, "fn main() {\n    let x = Ok(1)\n}", workdir, "E_INFER_TYPE")
    expect_diag(cli, 'fn main() {\n    let x = Err("e")\n}', workdir, "E_INFER_TYPE")
    expect_diag(cli, "enum Tree<T> { Empty, Node(v: T) }\nfn main() {\n    let t = Tree.Empty\n}", workdir, "E_INFER_TYPE")

    expect_diag(cli, "fn main() {\n    let bad: void? = None\n}", workdir, "E_VALUE")
    expect_diag(cli, "struct Bad<T> { x: Bad<T> }\nfn f(b: Bad<int>) {}\nfn main() {}", workdir, "E_TYPE_CYCLE")
    expect_diag(cli, "struct Option { x: int }\nfn main() {}", workdir, "E_DUP_TYPE")
    expect_diag(cli, "enum Result { A }\nfn main() {}", workdir, "E_DUP_TYPE")
    expect_diag(cli, "struct Some { x: int }\nfn main() {}", workdir, "E_DUP_TYPE")
    expect_diag(cli, "fn main() {\n    let o = Some(1)\n    match o { Some(v) => print(v) }\n}", workdir, "E_EXHAUSTIVE")
    expect_diag(cli, "fn main() {\n    let r: Result<int, str> = Ok(1)\n    match r { Ok(v) => print(v) }\n}", workdir, "E_EXHAUSTIVE")
    expect_diag(cli, "fn main() {\n    let bad: Result<int> = Ok(1)\n}", workdir, "E_GENERIC")
    expect_diag(cli, "fn main() {\n    print(Some(1) == Some(1))\n}", workdir, "E_EQUALITY")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Uso: python tests/turi03_m23.py RUTA_A_VELOZ")
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m23-") as tmp:
        workdir = Path(tmp)
        test_optionals(cli, workdir)
        test_result_and_try(cli, workdir)
        test_generic_structs_enums(cli, workdir)
        test_multi_module_package(cli, workdir)
        test_diagnostics_and_false_friends(cli, workdir)


if __name__ == "__main__":
    main()
