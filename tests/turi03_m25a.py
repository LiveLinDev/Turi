"""Pruebas funcionales, de memoria, paquetes, dyn dispatch y diferenciales del Hito 2.5A de Turi 0.3."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True):
    return subprocess.run(args, capture_output=True, text=text, encoding="utf-8" if text else None, env=env)


def find_clang() -> str:
    if os.environ.get("VELOZ_CLANG"):
        return os.environ["VELOZ_CLANG"]
    win_default = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang.exe"
    if win_default.is_file():
        return str(win_default)
    return "clang"


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


def apply_fix(source: str, diag: dict) -> str:
    raw = source.encode("utf-8")
    fix = diag["fix"]
    s, e = fix["span"]["start"], fix["span"]["end"]
    return (raw[:s] + fix["replacement"].encode("utf-8") + raw[e:]).decode("utf-8")


def test_inherent_methods_and_generics(cli: Path, workdir: Path):
    src = """
struct Point {
    x: int
    y: int
}

struct Rect {
    origin: Point
    w: int
    h: int
}

impl Point {
    fn origin() -> Point {
        return Point { x: 0, y: 0 }
    }
    fn translate(mut self, dx: int, dy: int) {
        self.x += dx
        self.y += dy
    }
}

impl Rect {
    fn area(self) -> int {
        return self.w * self.h
    }
    fn scale(mut self, k: int) {
        self.w *= k
        self.h *= k
    }
}

impl Rect {
    fn scaled(self, k: int) -> Rect {
        return Rect { origin: self.origin, w: self.w * k, h: self.h * k }
    }
}

struct Stack<T> {
    items: [T]
}

impl<T> Stack<T> {
    fn new() -> Stack<T> {
        return Stack<T> { items: [] }
    }
    fn pushVal(mut self, x: T) {
        push(mut self.items, x)
    }
    fn popVal(mut self) -> T? {
        if len(self.items) == 0 {
            return None
        }
        return Some(pop(mut self.items))
    }
    fn lenVal(self) -> int {
        return len(self.items)
    }
}

fn main() {
    var r = Rect { origin: Point.origin(), w: 3, h: 4 }
    print(r.area())
    print(r.scaled(2).area())
    r.scale(3)
    print(r.area())
    r.origin.translate(5, 8)
    print(r.origin.x + r.origin.y)

    let arr = [Rect { origin: Point.origin(), w: 2, h: 5 }, Rect { origin: Point.origin(), w: 7, h: 6 }]
    print(arr[1].area())
    print(Rect { origin: Point.origin(), w: 3, h: 4 }.area())

    var si = Stack<int>.new()
    si.pushVal(10)
    si.pushVal(25)
    print(si.lenVal())
    match si.popVal() {
        Some(v) => print(v)
        None => print(-1)
    }
    print(si.lenVal())

    var ss = Stack<str>.new()
    ss.pushVal("uno-largo-12345")
    ss.pushVal("dos")
    match ss.popVal() {
        Some(s) => print(s)
        None => print("empty")
    }
    match ss.popVal() {
        Some(s) => print(s)
        None => print("empty")
    }
    match ss.popVal() {
        Some(s) => print(s)
        None => print("empty")
    }

    var sp = Stack<Point>.new()
    sp.pushVal(Point { x: 4, y: 9 })
    match sp.popVal() {
        Some(p) => print(p.x * 10 + p.y)
        None => print(0)
    }
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == [
        "12", "48", "108", "13", "42", "12",
        "2", "25", "1",
        "dos", "uno-largo-12345", "empty",
        "49",
    ], out


def test_mut_field_paths(cli: Path, workdir: Path):
    src = """
struct Inner {
    v: int
}

impl Inner {
    fn grow(mut self, d: int) {
        self.v += d
    }
}

struct Outer {
    child: Inner
}

impl Outer {
    fn bump(mut self, d: int) {
        self.child.grow(d)
    }
}

struct Wrap {
    buf: StringBuilder
    m: Map<str, int>
}

struct Pair {
    a: int
    b: int
}

fn swapInt(x: mut int, y: mut int) {
    let t = x
    x = y
    y = t
}

fn main() {
    var w = Wrap { buf: sbNew(), m: mapNew() }
    sbPush(mut w.buf, "hola")
    sbPush(mut w.buf, "-turi-03-largo")
    mapSet(mut w.m, "clave-larga-1234", 42)
    print(sbStr(w.buf))
    print(unwrapOr(mapGet(w.m, "clave-larga-1234"), -1))

    var o = Outer { child: Inner { v: 10 } }
    o.bump(7)
    o.child.grow(3)
    print(o.child.v)

    var p = Pair { a: 100, b: 200 }
    swapInt(mut p.a, mut p.b)
    print(p.a)
    print(p.b)
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == ["hola-turi-03-largo", "42", "20", "200", "100"], out


def test_interface_defaults_and_generic_impls(cli: Path, workdir: Path):
    src = """
interface Describable {
    fn title(self) -> str
    fn summary(self) -> str {
        return "item:" + self.title()
    }
    fn badge(self) -> str {
        return "[" + self.summary() + "]"
    }
}

struct Plain {
    label: str
}

impl Describable for Plain {
    fn title(self) -> str {
        return self.label
    }
}

struct Over {
    label: str
}

impl Describable for Over {
    fn title(self) -> str {
        return self.label
    }
    fn summary(self) -> str {
        return "custom:" + self.title()
    }
}

interface Tag {
    fn tagText(self) -> str {
        return "default-tag"
    }
}

struct EmptyTag {
}

impl Tag for EmptyTag {
}

interface MakeSame {
    fn combine(a: Self, b: Self) -> Self
    fn withVal(self, v: int) -> Self
    fn selfDouble(self) -> Self {
        return Self.combine(self, self)
    }
    fn doubledVal(self, v: int) -> Self {
        return self.withVal(v * 2)
    }
}

struct NumBox {
    n: int
}

impl MakeSame for NumBox {
    fn combine(a: NumBox, b: NumBox) -> NumBox {
        return NumBox { n: a.n + b.n }
    }
    fn withVal(self, v: int) -> NumBox {
        return NumBox { n: v }
    }
}

struct Box<T> {
    val: T
}

impl<T: Describable> Describable for Box<T> {
    fn title(self) -> str {
        return self.val.title()
    }
}

fn show<T: Describable>(x: T) -> str {
    return x.badge()
}

fn main() {
    let p = Plain { label: "alpha" }
    let o = Over { label: "beta" }
    let e = EmptyTag { }
    let nb = NumBox { n: 5 }
    let nb2 = nb.doubledVal(49).selfDouble()
    let boxed = Box<Plain> { val: Plain { label: "nested-box-label" } }

    print(p.badge())
    print(o.badge())
    print(e.tagText())
    print(nb2.n)
    print(show(boxed))
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == [
        "[item:alpha]",
        "[custom:beta]",
        "default-tag",
        "196",
        "[item:nested-box-label]",
    ], out


def test_dyn_dispatch_and_recursion(cli: Path, workdir: Path):
    src = """
interface Shape {
    fn area(self) -> int
    fn scale(mut self, k: int)
    fn kindName(self) -> str {
        return "shape"
    }
}

struct Pt {
    x: int
    y: int
}

struct Rect {
    w: int
    h: int
}

impl Shape for Rect {
    fn area(self) -> int {
        return self.w * self.h
    }
    fn scale(mut self, k: int) {
        self.w *= k
        self.h *= k
    }
    fn kindName(self) -> str {
        return "rect"
    }
}

struct Polygon {
    name: str
    pts: [Pt]
}

impl Shape for Polygon {
    fn area(self) -> int {
        var acc = len(self.name)
        for p in self.pts {
            acc += p.x + p.y
        }
        return acc
    }
    fn scale(mut self, k: int) {
        self.name = self.name + "-scaled-largo"
        for i in 0..len(self.pts) {
            var cur = self.pts[i]
            cur.x *= k
            cur.y *= k
            self.pts[i] = cur
        }
    }
}

struct EmptyShape {
}

impl Shape for EmptyShape {
    fn area(self) -> int {
        return 7
    }
    fn scale(mut self, k: int) {
    }
}

interface Eval {
    fn eval(self) -> int
}

struct Leaf {
    v: int
}

impl Eval for Leaf {
    fn eval(self) -> int {
        return self.v
    }
}

struct Node {
    val: int
    next: dyn Eval?
}

impl Eval for Node {
    fn eval(self) -> int {
        match self.next {
            Some(n) => return self.val + n.eval()
            None => return self.val
        }
    }
}

fn checkResult(flag: bool) -> Result<dyn Shape, str> {
    if flag {
        return Ok(Rect { w: 4, h: 5 })
    }
    return Err("fallo-largo-12345")
}

fn main() {
    var shapes: [dyn Shape] = [
        Rect { w: 2, h: 3 },
        Polygon { name: "poly-long-owned-name", pts: [Pt { x: 1, y: 2 }, Pt { x: 3, y: 4 }] },
        EmptyShape { }
    ]
    var total = 0
    for s in shapes {
        total += s.area()
    }
    print(total)
    print(shapes[0].kindName())
    print(shapes[1].kindName())
    print(shapes[2].kindName())

    var a: dyn Shape = Rect { w: 3, h: 4 }
    let b = a
    a.scale(2)
    print(a.area())
    print(b.area())

    var p1: dyn Shape = Polygon { name: "poly-clone-test", pts: [Pt { x: 2, y: 3 }] }
    let p2 = p1
    p1.scale(10)
    print(p1.area())
    print(p2.area())

    var opt: dyn Shape? = Some(EmptyShape { })
    match opt {
        Some(s) => print(s.area())
        None => print(0)
    }

    match checkResult(true) {
        Ok(s) => print(s.area())
        Err(msg) => print(-1)
    }

    var m: Map<str, dyn Shape> = mapNew()
    mapSet(mut m, "k-rect-owned-key", Rect { w: 5, h: 6 })
    mapSet(mut m, "k-empty", EmptyShape { })
    match mapGet(m, "k-rect-owned-key") {
        Some(s) => print(s.area())
        None => print(-1)
    }

    let root = Node {
        val: 10,
        next: Some(Node {
            val: 20,
            next: Some(Leaf { v: 12 })
        })
    }
    print(root.eval())
}
"""
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == [
        "43",
        "rect",
        "shape",
        "shape",
        "48",
        "12",
        "78",
        "20",
        "7",
        "20",
        "30",
        "42",
    ], out

    direct_rec = """
interface Eval {
    fn eval(self) -> int
}

struct LoopDirect {
    n: int
}

impl Eval for LoopDirect {
    fn eval(self) -> int {
        let nxt: dyn Eval = LoopDirect { n: self.n + 1 }
        return 1 + nxt.eval()
    }
}

fn main() {
    let start: dyn Eval = LoopDirect { n: 0 }
    print(start.eval())
}
"""
    tid, _ = run_both(cli, direct_rec, workdir, expect_trap=True)
    assert tid == "R_DEPTH", tid

    indirect_rec = """
interface Eval {
    fn eval(self) -> int
}

struct LoopIndirect {
    n: int
}

fn bounce(e: dyn Eval) -> int {
    return 1 + e.eval()
}

impl Eval for LoopIndirect {
    fn eval(self) -> int {
        let nxt: dyn Eval = LoopIndirect { n: self.n + 1 }
        return bounce(nxt)
    }
}

fn main() {
    let start: dyn Eval = LoopIndirect { n: 0 }
    print(start.eval())
}
"""
    tid2, _ = run_both(cli, indirect_rec, workdir, expect_trap=True)
    assert tid2 == "R_DEPTH", tid2


def test_multimodule_package_and_sig(cli: Path, workdir: Path):
    pkg_root = workdir / "pkg_dyn"
    (pkg_root / "contracts" / "src").mkdir(parents=True)
    (pkg_root / "model" / "src").mkdir(parents=True)
    (pkg_root / "app" / "src").mkdir(parents=True)

    (pkg_root / "contracts" / "veloz.pkg").write_text("package contracts\nversion 1.0.0\n", encoding="utf-8")
    (pkg_root / "contracts" / "src" / "contracts.turi").write_text("""
interface Reporter {
    fn report(self, msg: str)
}
""".lstrip(), encoding="utf-8")

    (pkg_root / "model" / "veloz.pkg").write_text(
        'package model\nversion 1.0.0\nrequire contracts 1.0.0 "../contracts"\n',
        encoding="utf-8",
    )
    (pkg_root / "model" / "src" / "model.turi").write_text("""
import contracts

struct ConsoleReporter {
    prefix: str
}

impl ConsoleReporter {
    fn withPrefix(p: str) -> ConsoleReporter {
        return ConsoleReporter { prefix: p }
    }
}

impl contracts.Reporter for ConsoleReporter {
    fn report(self, msg: str) {
        print(self.prefix + msg)
    }
}

struct SilentReporter {
}

impl contracts.Reporter for SilentReporter {
    fn report(self, msg: str) {
    }
}
""".lstrip(), encoding="utf-8")

    (pkg_root / "app" / "veloz.pkg").write_text(
        'package app\nversion 1.0.0\nrequire contracts 1.0.0 "../contracts"\nrequire model 1.0.0 "../model"\n',
        encoding="utf-8",
    )
    app_entry = pkg_root / "app" / "src" / "app.turi"
    app_entry.write_text("""
import contracts
import model

fn dispatch(r: dyn contracts.Reporter) {
    r.report("ok-dyn")
}

fn pureAdd(x: int) -> int {
    return x + 1
}

fn main() {
    let cr = model.ConsoleReporter.withPrefix("log:")
    let d1: dyn contracts.Reporter = cr
    let d2: dyn contracts.Reporter = model.SilentReporter { }
    dispatch(d1)
    dispatch(d2)
    print(pureAdd(41))
}
""".lstrip(), encoding="utf-8")

    interp = run_cmd([str(cli), "run", str(app_entry)])
    assert interp.returncode == 0, interp.stderr
    exe = workdir / ("pkg_dyn_bin.exe" if os.name == "nt" else "pkg_dyn_bin")
    bld = run_cmd([str(cli), "build", str(app_entry), "-o", str(exe)])
    assert bld.returncode == 0, bld.stderr
    nat = run_cmd([str(exe)])
    assert nat.returncode == 0 and nat.stdout == interp.stdout
    assert nat.stdout.splitlines() == ["log:ok-dyn", "42"], nat.stdout

    sig_r = run_cmd([str(cli), "sig", str(app_entry)])
    assert sig_r.returncode == 0, sig_r.stderr
    sigs = {item["name"]: item for item in (json.loads(line) for line in sig_r.stdout.splitlines())}
    assert sigs["app.dispatch"]["io"] is True, sigs["app.dispatch"]
    assert sigs["app.pureAdd"]["io"] is False, sigs["app.pureAdd"]

    # Cross-module inherent coherence error.
    orphan_src = workdir / "pkg_orphan"
    (orphan_src / "contracts" / "src").mkdir(parents=True)
    (orphan_src / "app" / "src").mkdir(parents=True)
    (orphan_src / "contracts" / "veloz.pkg").write_text("package contracts\nversion 1.0.0\n", encoding="utf-8")
    (orphan_src / "contracts" / "src" / "contracts.turi").write_text(
        "struct Foreign { v: int }\ninterface I { fn get(self) -> int }\n", encoding="utf-8"
    )
    (orphan_src / "app" / "veloz.pkg").write_text(
        'package app\nversion 1.0.0\nrequire contracts 1.0.0 "../contracts"\n', encoding="utf-8"
    )
    orphan_inherent = orphan_src / "app" / "src" / "app.turi"
    orphan_inherent.write_text(
        "import contracts\nimpl contracts.Foreign {\n    fn extra(self) -> int { return self.v }\n}\nfn main() {}\n",
        encoding="utf-8",
    )
    chk_inh = run_cmd([str(cli), "check", str(orphan_inherent)])
    assert chk_inh.returncode != 0
    assert json.loads(chk_inh.stderr.strip().splitlines()[-1])["id"] == "E_IMPL_COHERENCE"


def test_diagnostics_and_fixes(cli: Path, workdir: Path):
    # E_SELF_MUT with verified autofix.
    s_ref = "struct C { x: int }\nimpl C {\n    fn get(&self) -> int { return self.x }\n}\nfn main() {}\n"
    d1 = expect_diag(cli, s_ref, workdir, "E_SELF_MUT")
    fixed1 = apply_fix(s_ref, d1)
    assert "fn get(self)" in fixed1, fixed1
    out, _ = run_both(cli, fixed1, workdir)
    assert out == ""

    s_mut_ref = "struct C { x: int }\nimpl C {\n    fn inc(&mut self) { self.x += 1 }\n}\nfn main() {}\n"
    d2 = expect_diag(cli, s_mut_ref, workdir, "E_SELF_MUT")
    fixed2 = apply_fix(s_mut_ref, d2)
    assert "fn inc(mut self)" in fixed2, fixed2
    run_both(cli, fixed2, workdir)

    # E_DYN_SAFETY
    expect_diag(
        cli,
        "interface A {\n    fn mk(a: int, x: Self) -> int\n}\nfn f(x: dyn A) {}\nfn main() {}\n",
        workdir,
        "E_DYN_SAFETY",
    )
    expect_diag(
        cli,
        "interface B {\n    fn pair(self, other: Self) -> int\n}\nfn f(x: dyn B) {}\nfn main() {}\n",
        workdir,
        "E_DYN_SAFETY",
    )
    expect_diag(
        cli,
        "interface C {\n    fn cloneMe(self) -> Self\n}\nfn f(x: dyn C) {}\nfn main() {}\n",
        workdir,
        "E_DYN_SAFETY",
    )

    # E_DYN_TYPE
    expect_diag(cli, "fn f(x: dyn int) {}\nfn main() {}\n", workdir, "E_DYN_TYPE")
    expect_diag(cli, "struct S { x: int }\nfn f(x: dyn S) {}\nfn main() {}\n", workdir, "E_DYN_TYPE")

    # E_IMMUTABLE & E_ALIAS_MUT
    expect_diag(
        cli,
        'struct Wrap { buf: StringBuilder }\nfn main() {\n    let w = Wrap { buf: sbNew() }\n    sbPush(mut w.buf, "x")\n}\n',
        workdir,
        "E_IMMUTABLE",
    )
    expect_diag(
        cli,
        "struct Pair { a: int, b: int }\nfn sw(x: mut int, y: mut int) {}\nfn main() {\n    var p = Pair { a: 1, b: 2 }\n    sw(mut p.a, mut p.a)\n}\n",
        workdir,
        "E_ALIAS_MUT",
    )
    expect_diag(
        cli,
        "struct Pair { a: int, b: int }\nfn tw(p: mut Pair, x: mut int) {}\nfn main() {\n    var p = Pair { a: 1, b: 2 }\n    tw(mut p, mut p.a)\n}\n",
        workdir,
        "E_ALIAS_MUT",
    )

    # E_DUP_FUNCTION, E_OVERLOAD & E_IMPL_INHERENT
    expect_diag(
        cli,
        "struct S { x: int }\nimpl S {\n    fn f(self) -> int { return self.x }\n    fn f(self, y: int) -> int { return self.x + y }\n}\nfn main() {}\n",
        workdir,
        "E_DUP_FUNCTION",
    )
    expect_diag(
        cli,
        "struct S { x: int }\nimpl S { fn f(self) -> int { return self.x } }\nimpl S { fn f(self) -> int { return self.x } }\nfn main() {}\n",
        workdir,
        "E_DUP_FUNCTION",
    )
    expect_diag(
        cli,
        "impl int {\n    fn zero(self) -> int { return 0 }\n}\nfn main() {}\n",
        workdir,
        "E_IMPL_INHERENT",
    )
    expect_diag(
        cli,
        "interface I1 { fn ping(self) -> int }\ninterface I2 { fn ping(self) -> int }\nstruct S { x: int }\nimpl I1 for S { fn ping(self) -> int { return self.x } }\nimpl I2 for S { fn ping(self) -> int { return self.x } }\nfn main() {\n    let s = S { x: 1 }\n    let v = s.ping()\n}\n",
        workdir,
        "E_OVERLOAD",
    )

    # E_METHOD_STATIC
    expect_diag(
        cli,
        "struct S { x: int }\nimpl S { fn origin() -> S { return S { x: 0 } } }\nfn main() {\n    let s = S { x: 1 }\n    let o = s.origin()\n}\n",
        workdir,
        "E_METHOD_STATIC",
    )

    # E_IMPL_METHOD_MISSING, E_DUP_IMPL & E_IMPL_MISSING
    expect_diag(
        cli,
        "interface I { fn a(self) -> int\n fn b(self) -> int }\nstruct S { x: int }\nimpl I for S { fn a(self) -> int { return self.x } }\nfn main() {}\n",
        workdir,
        "E_IMPL_METHOD_MISSING",
    )
    expect_diag(
        cli,
        "interface I { fn a(self) -> int }\nstruct S { x: int }\nimpl I for S { fn a(self) -> int { return self.x } }\nimpl I for S { fn a(self) -> int { return self.x } }\nfn main() {}\n",
        workdir,
        "E_DUP_IMPL",
    )
    expect_diag(
        cli,
        "interface D { fn t(self) -> str }\nstruct Box<T> { val: T }\nstruct Plain { x: int }\nimpl<T: D> D for Box<T> { fn t(self) -> str { return self.val.t() } }\nfn main() {\n    let b = Box<Plain> { val: Plain { x: 1 } }\n    let s = b.t()\n}\n",
        workdir,
        "E_METHOD_UNKNOWN",
    )
    expect_diag(
        cli,
        "interface D { fn t(self) -> str }\nstruct Box<T> { val: T }\nstruct Plain { x: int }\nimpl<T: D> D for Box<T> { fn t(self) -> str { return self.val.t() } }\nfn need<U: D>(x: U) -> str { return x.t() }\nfn main() {\n    let b = Box<Plain> { val: Plain { x: 1 } }\n    let s = need(b)\n}\n",
        workdir,
        "E_IMPL_MISSING",
    )
    expect_diag(
        cli,
        "interface D { fn t(self) -> str }\nstruct Box<T> { val: T }\nstruct Plain { x: int }\nimpl<T: D> D for Box<T> { fn t(self) -> str { return self.val.t() } }\nfn main() {\n    let d: dyn D = Box<Plain> { val: Plain { x: 1 } }\n}\n",
        workdir,
        "E_IMPL_MISSING",
    )

    # Broken unused default method body still fails check.
    expect_diag(
        cli,
        'interface Bad {\n    fn ok(self) -> int\n    fn bad(self) -> int { return "wrong" }\n}\nfn main() {}\n',
        workdir,
        "E_TYPE",
    )

    # False-friend E_METHOD_UNKNOWN checks on built-in collections & strings (non-UFCS names).
    for expr, hint in [
        ("let a = [1, 2]\n    let n = a.size()", "len("),
        ("var a = [1]\n    a.append(2)", "push("),
        ('let s = "abc"\n    let n = s.size()', "len("),
        ('var m: Map<str, int> = mapNew()\n    m.put("a", 1)', "mapSet("),
        ('let m: Map<str, int> = mapNew()\n    let x = m.containsKey("a")', "mapHas("),
        ('let m: Map<str, int> = mapNew()\n    let n = m.size()', "len("),
        ("var st: Set<int> = setNew()\n    let added = st.insert(1)", "setAdd("),
    ]:
        d = expect_diag(cli, f"fn main() {{\n    {expr}\n}}\n", workdir, "E_METHOD_UNKNOWN")
        fix_text = (d.get("fix") or {}).get("replacement", "") + " " + d.get("example", "") + " " + d.get("message", "")
        assert hint in fix_text, (expr, d)


def test_native_dyn_leak_audit(cli: Path, workdir: Path):
    clang = find_clang()
    stress_src = """
interface Item {
    fn score(self) -> int
    fn bump(mut self, d: int)
}

struct Small {
    v: int
}

impl Item for Small {
    fn score(self) -> int { return self.v }
    fn bump(mut self, d: int) { self.v += d }
}

struct OwnedPoly {
    tag: str
    nums: [int]
}

impl Item for OwnedPoly {
    fn score(self) -> int {
        var acc = len(self.tag)
        for n in self.nums { acc += n }
        return acc
    }
    fn bump(mut self, d: int) {
        self.tag = self.tag + "-bumped-largo"
        push(mut self.nums, d)
    }
}

struct Zst {
}

impl Item for Zst {
    fn score(self) -> int { return 5 }
    fn bump(mut self, d: int) { }
}

struct Node {
    v: int
    next: dyn Item?
}

impl Item for Node {
    fn score(self) -> int {
        match self.next {
            Some(n) => return self.v + n.score()
            None => return self.v
        }
    }
    fn bump(mut self, d: int) {
        self.v += d
    }
}

fn takeDyn(x: dyn Item) -> int {
    return x.score()
}

fn main() {
    var total = 0
    for i in 0..80 {
        var a: dyn Item = OwnedPoly { tag: "owned-long-string-{i}", nums: [i, i + 1] }
        var b = a
        a.bump(3)
        b = Small { v: i }
        b.bump(1)
        var arr: [dyn Item] = [a, b, Zst { }]
        arr[0] = OwnedPoly { tag: "replaced-owned-long-{i}", nums: [9, 8] }
        let opt: dyn Item? = Some(Node { v: i, next: Some(OwnedPoly { tag: "node-long-string", nums: [1, 2] }) })
        match opt {
            Some(n) => total += takeDyn(n)
            None => total += 0
        }
        var mp: Map<str, dyn Item> = mapNew()
        mapSet(mut mp, "k-owned-key-{i}", arr[0])
        mapSet(mut mp, "k-zst", Zst { })
        match mapGet(mp, "k-owned-key-{i}") {
            Some(v) => total += v.score()
            None => total += 0
        }
        total += arr[2].score()
    }
    assert total > 0
}
"""
    src_file = workdir / "dyn_stress.turi"
    src_file.write_text(stress_src, encoding="utf-8")
    ir_p = run_cmd([str(cli), "emit-ir", str(src_file)])
    assert ir_p.returncode == 0, ir_p.stderr
    ir = ir_p.stdout.replace("@malloc(", "@audit_malloc(").replace("@free(", "@audit_free(")
    ir = ir.replace("declare noalias ptr @malloc(i64)", "declare noalias ptr @audit_malloc(i64)")
    ir = ir.replace("declare void @free(ptr)", "declare void @audit_free(ptr)")
    ir_wrapped = ir.replace("define i32 @main()", "define i32 @turi_main()")
    ir_file = workdir / "dyn_stress.ll"
    ir_file.write_text(ir_wrapped, encoding="utf-8")

    rt_hdr = Path(__file__).resolve().parents[1] / "src/native_collections_rt.hpp"
    hdr_txt = rt_hdr.read_text(encoding="utf-8")
    c_start = hdr_txt.index('R"c(\n') + 5
    c_end = hdr_txt.index('\n)c";')
    col_c = hdr_txt[c_start:c_end]
    inject_anchor = "typedef struct tz_gstr"
    audit_macros = (
        "void* audit_malloc(size_t n);\n"
        "void audit_free(void* p);\n"
        "#define malloc audit_malloc\n"
        "#define free audit_free\n"
    )
    col_file = workdir / "col_rt_audit.c"
    col_file.write_text(col_c.replace(inject_anchor, audit_macros + inject_anchor, 1), encoding="utf-8")

    shim_file = workdir / "audit_shim.c"
    shim_file.write_text("""
#include <stdio.h>
#include <stdlib.h>

static long long g_total_allocs = 0;
static long long g_live_allocs = 0;

void* audit_malloc(size_t n) {
    void* p = malloc(n > 0 ? n : 1);
    if (p) { ++g_total_allocs; ++g_live_allocs; }
    return p;
}
void audit_free(void* p) {
    if (p) { --g_live_allocs; free(p); }
}

int turi_main(void);

int main(void) {
    int rc = turi_main();
    printf("total=%lld live=%lld\\n", g_total_allocs, g_live_allocs);
    if (rc != 0 || g_live_allocs != 0) return 2;
    return 0;
}
""".lstrip(), encoding="utf-8")

    stress_exe = workdir / ("dyn_stress_audit.exe" if os.name == "nt" else "dyn_stress_audit")
    cc = run_cmd([clang, "-O2", "-Wno-override-module", str(shim_file), str(col_file), "-x", "ir", str(ir_file), "-o", str(stress_exe)])
    assert cc.returncode == 0, f"audit compile failed: {cc.stderr}"
    run_res = run_cmd([str(stress_exe)])
    assert run_res.returncode == 0, f"leak detected! stdout={run_res.stdout!r} stderr={run_res.stderr!r}"


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m25a-") as tmp:
        workdir = Path(tmp)
        test_inherent_methods_and_generics(cli, workdir)
        test_mut_field_paths(cli, workdir)
        test_interface_defaults_and_generic_impls(cli, workdir)
        test_dyn_dispatch_and_recursion(cli, workdir)
        test_multimodule_package_and_sig(cli, workdir)
        test_diagnostics_and_fixes(cli, workdir)
        test_native_dyn_leak_audit(cli, workdir)
    print("turi03_m25a: ok")


if __name__ == "__main__":
    main()
