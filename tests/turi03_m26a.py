"""Pruebas de moves por último uso, Pool<T>/Handle<T>, Shared<T> Perceus y niveles veloz costes (Hito 2.6A)."""
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


def expect_diag(cli: Path, source: str, workdir: Path, expected_id: str, extra_args=()):
    src = workdir / "diag.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    r = run_cmd([str(cli), "check", str(src), *extra_args])
    assert r.returncode != 0, f"expected compile error {expected_id}, got OK\n{source}"
    line = r.stderr.strip().splitlines()[-1]
    d = json.loads(line)
    assert d["id"] == expected_id, f"expected {expected_id}, got {d}\n{source}"
    return d


def compile_and_audit(cli: Path, clang: str, src_text: str, workdir: Path, name: str):
    src_file = workdir / f"{name}.turi"
    src_file.write_text(src_text, encoding="utf-8", newline="\n")
    ir_p = run_cmd([str(cli), "emit-ir", str(src_file)])
    assert ir_p.returncode == 0, f"emit-ir failed: {ir_p.stderr}"
    ir = ir_p.stdout.replace("@malloc(", "@audit_malloc(").replace("@free(", "@audit_free(")
    ir = ir.replace("declare noalias ptr @malloc(i64)", "declare noalias ptr @audit_malloc(i64)")
    ir = ir.replace("declare void @free(ptr)", "declare void @audit_free(ptr)")
    ir_wrapped = ir.replace("define i32 @main()", "define i32 @turi_main()")
    ir_file = workdir / f"{name}.ll"
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
    col_file = workdir / f"{name}_rt.c"
    col_file.write_text(col_c.replace(inject_anchor, audit_macros + inject_anchor, 1), encoding="utf-8")

    shim_file = workdir / f"{name}_shim.c"
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
    printf("%lld %lld\\n", g_total_allocs, g_live_allocs);
    return rc;
}
""".lstrip(), encoding="utf-8")

    exe = workdir / (f"{name}_audit.exe" if os.name == "nt" else f"{name}_audit")
    cc = run_cmd([clang, "-O2", "-Wno-override-module", str(shim_file), str(col_file), "-x", "ir", str(ir_file), "-o", str(exe)])
    assert cc.returncode == 0, f"audit compile failed: {cc.stderr}"
    run_res = run_cmd([str(exe)])
    assert run_res.returncode == 0, f"audit run failed: {run_res.stdout!r} {run_res.stderr!r}"
    total_allocs, live_allocs = map(int, run_res.stdout.strip().splitlines()[-1].split())
    return total_allocs, live_allocs


def extract_llvm_fn(ir_text: str, fn_name: str) -> str:
    pos = -1
    for marker in (f'@"turi.{fn_name}"(', f"@turi.{fn_name}(", f'@"main.{fn_name}"(', f"@{fn_name}("):
        pos = ir_text.find(marker)
        if pos >= 0:
            break
    assert pos >= 0, f"missing LLVM function {fn_name}"
    start = ir_text.rfind("define ", 0, pos)
    end = ir_text.find("\n}\n", pos)
    assert start >= 0 and end >= 0
    body = ir_text[start:end + 3]
    # Una función con versión rápida protegida por guarda es un trampolín: el cuerpo real está en
    # sus variantes .fast y .slow.
    for variant in ("fast", "slow"):
        vpos = ir_text.find(f'@"turi.{fn_name}.{variant}"(')
        while vpos >= 0:
            vstart = ir_text.rfind("\n", 0, vpos) + 1
            if ir_text.startswith("define ", vstart):
                body += ir_text[vstart:ir_text.find("\n}\n", vpos) + 3]
                break
            vpos = ir_text.find(f'@"turi.{fn_name}.{variant}"(', vpos + 1)
    return body


def test_move_elision_and_audit(cli: Path, workdir: Path):
    diff_src = """
struct Inner {
    arr: [int]
    label: str
}

struct Outer {
    inner: Inner
    tail: [int]
}

struct Pair {
    a: [int]
    b: [int]
}

struct Wrap {
    v: [int]
}

enum Packet {
    Empty
    Payload(items: [int], tag: str)
}

fn sinkArr(xs: [int]) -> int {
    return xs[0] + len(xs)
}

fn borrowAndOwned(s: Shared<int>, fresh: Shared<int>) -> int {
    return sharedGet(s) + sharedGet(fresh)
}

fn borrowMatAndWrap(matrix: [[int]], w: Wrap) -> int {
    return matrix[0][0] + w.v[1]
}

fn main() {
    var localArr = [10, 20, 30]
    var movedArr = localArr
    push(mut movedArr, 40)
    print(len(movedArr))

    let srcPair = Pair { a: [1, 2], b: [3, 4] }
    let dstPair = Pair { a: srcPair.a, b: srcPair.b }
    print(dstPair.a[0] + dstPair.b[1])

    let out = Outer { inner: Inner { arr: [5, 6, 7], label: "ok" }, tail: [8, 9] }
    var stolen = out.inner.arr
    push(mut stolen, 10)
    print(len(stolen) + stolen[0])

    let mat = [[11, 12], [13, 14]]
    var row = mat[1]
    push(mut row, 15)
    print(row[0] + len(row))

    let pkt = Packet.Payload([21, 22], "pkt")
    match pkt {
        Packet.Empty => print(0)
        Packet.Payload(items, tag) => {
            var ownedItems = items
            push(mut ownedItems, 23)
            print("{tag}:{len(ownedItems)}:{ownedItems[0]}")
        }
    }

    let x = [100, 200]
    var dup = Pair { a: x, b: x }
    dup.a[0] = 999
    print("{dup.a[0]}:{dup.b[0]}")

    let s = sharedNew(7)
    print(borrowAndOwned(s, sharedNew(20)))

    let m2 = [[4, 5], [6, 7]]
    print(borrowMatAndWrap(m2, Wrap { v: m2[0] }))
}
"""
    out, _ = run_both(cli, diff_src, workdir)
    assert out.splitlines() == ["4", "5", "9", "16", "pkt:3:21", "999:100", "27", "9"], out

    clang = find_clang()
    # Audit costes & exact malloc count for move paths vs duplicate Pair { a: x, b: x }
    audit_src = """
struct Inner {
    arr: [int]
    label: str
}

struct Outer {
    inner: Inner
    tail: [int]
}

struct Pair {
    a: [int]
    b: [int]
}

enum Msg {
    NoneMsg
    Items(xs: [int])
}

fn stepMove(k: int) -> int {
    let a = [k, k + 1]
    let b = a
    let p = Pair { a: b, b: [k + 2, k + 3] }
    let c = p.a
    let d = p.b
    let out = Outer { inner: Inner { arr: c, label: "tag-{k}" }, tail: d }
    let ch = out.inner.arr
    let mat = [ch]
    let elem = mat[0]
    let m = Msg.Items(elem)
    match m {
        Msg.NoneMsg => return 0
        Msg.Items(xs) => return xs[0] + xs[1]
    }
}

fn stepDup(k: int) -> int {
    let x = [k, k + 1]
    var p = Pair { a: x, b: x }
    p.a[0] = k + 9
    return p.a[0] + p.b[0]
}

fn main() {
    var acc = 0
    for i in 0..500 {
        acc += stepMove(i)
    }
    assert acc > 0
}
"""
    audit_file = workdir / "move_costes.turi"
    audit_file.write_text(audit_src, encoding="utf-8", newline="\n")
    cost_move = run_cmd([str(cli), "costes", str(audit_file), "stepMove"])
    assert cost_move.returncode == 0, cost_move.stderr
    move_sites = [json.loads(line) for line in cost_move.stdout.splitlines() if line.strip()]
    assert all(s["kind"] != "clone" for s in move_sites), move_sites

    cost_dup = run_cmd([str(cli), "costes", str(audit_file), "stepDup"])
    assert cost_dup.returncode == 0, cost_dup.stderr
    dup_sites = [json.loads(line) for line in cost_dup.stdout.splitlines() if line.strip()]
    assert sum(1 for s in dup_sites if s["kind"] == "clone") == 1, dup_sites

    # Each stepMove allocates only for newly constructed values (no clones):
    # a (1) + [k+2, k+3] (1) + "tag-{k}" Interp (1) + [ch] (1) = 4 allocs/iter * 500 = 2000
    total_move, live_move = compile_and_audit(cli, clang, audit_src, workdir, "move_loop")
    assert total_move == 2000 and live_move == 0, (total_move, live_move)


def test_pool_and_handles(cli: Path, workdir: Path):
    diff_src = """
struct DNode {
    val: int
    prev: Handle<DNode>?
    next: Handle<DNode>?
}

struct Vertex {
    label: str
    edges: [Handle<Vertex>]
}

struct Unit {}

struct Graph<T> {
    nodes: Pool<T>
    roots: [Handle<T>]
}

fn monoLookup(n: int) -> int {
    var p: Pool<int> = poolWithCap(n)
    var hs: [Handle<int>] = []
    for i in 0..n {
        push(mut hs, poolAdd(mut p, i * 3 + 1))
    }
    var sum = 0
    for h in hs {
        if poolHas(p, h) {
            match poolGet(p, h) {
                Some(v) => { sum += v }
                None => {}
            }
        }
    }
    return sum
}

fn nonMonoLookup(n: int) -> int {
    var p: Pool<int> = poolNew()
    var hs: [Handle<int>] = []
    for i in 0..n {
        push(mut hs, poolAdd(mut p, i + 1))
    }
    let removed = poolRemove(mut p, hs[0])
    var sum = 0
    match removed {
        Some(v) => { sum += v }
        None => {}
    }
    for h in hs {
        if poolHas(p, h) {
            match poolGet(p, h) {
                Some(v) => { sum += v }
                None => {}
            }
        }
    }
    return sum
}

fn linkPrev(p: mut Pool<DNode>, h: Handle<DNode>, pr: Handle<DNode>) {
    match poolGet(p, h) {
        Some(n) => {
            let ok = poolSet(mut p, h, DNode { val: n.val, prev: Some(pr), next: n.next })
            assert ok
        }
        None => assert false
    }
}

fn linkNext(p: mut Pool<DNode>, h: Handle<DNode>, nx: Handle<DNode>) {
    match poolGet(p, h) {
        Some(n) => {
            let ok = poolSet(mut p, h, DNode { val: n.val, prev: n.prev, next: Some(nx) })
            assert ok
        }
        None => assert false
    }
}

fn main() {
    var dll: Pool<DNode> = poolWithCap(4)
    let h1 = poolAdd(mut dll, DNode { val: 10, prev: None, next: None })
    let h2 = poolAdd(mut dll, DNode { val: 20, prev: None, next: None })
    let h3 = poolAdd(mut dll, DNode { val: 30, prev: None, next: None })
    linkNext(mut dll, h1, h2)
    linkPrev(mut dll, h2, h1)
    linkNext(mut dll, h2, h3)
    linkPrev(mut dll, h3, h2)
    linkNext(mut dll, h3, h1)
    linkPrev(mut dll, h1, h3)

    // Forward & backward traversal across 6 steps
    var fcur: Handle<DNode>? = Some(h1)
    var fsum = 0
    for step in 0..4 {
        match fcur {
            Some(ch) => {
                let node = poolGet(dll, ch)?
                fsum += node.val
                fcur = node.next
            }
            None => {}
        }
    }
    print(fsum)

    // Pool clone independence
    var dllCopy = dll
    match poolRemove(mut dllCopy, h2) {
        Some(dead) => print(dead.val)
        None => assert false
    }
    assert !poolHas(dllCopy, h2)
    assert poolHas(dll, h2)

    // Freelist LIFO reuse & stale handle detection
    let hNew = poolAdd(mut dllCopy, DNode { val: 99, prev: None, next: None })
    assert hNew != h2
    assert !poolHas(dllCopy, h2)
    match poolGet(dllCopy, h2) {
        Some(deadNode) => assert deadNode.val < 0
        None => {}
    }
    assert poolHas(dllCopy, hNew)

    // Cyclic graph with Set<Handle<Vertex>> & Map<Handle<Vertex>, int>
    var g: Pool<Vertex> = poolNew()
    let v0 = poolAdd(mut g, Vertex { label: "a", edges: [] })
    let v1 = poolAdd(mut g, Vertex { label: "b", edges: [v0] })
    assert poolSet(mut g, v0, Vertex { label: "a", edges: [v1] })
    var seen: Set<Handle<Vertex>> = setNew()
    var dist: Map<Handle<Vertex>, int> = mapNew()
    var queue: [Handle<Vertex>] = [v0]
    assert setAdd(mut seen, v0)
    mapSet(mut dist, v0, 0)
    var qi = 0
    while qi < len(queue) {
        let u = queue[qi]
        qi += 1
        let d0 = mapGet(dist, u)?
        let node = poolGet(g, u)?
        for nx in node.edges {
            if !setHas(seen, nx) {
                assert setAdd(mut seen, nx)
                mapSet(mut dist, nx, d0 + 1)
                push(mut queue, nx)
            }
        }
    }
    print(mapGet(dist, v1)?)

    // Stale poolSet + clear on drop-requiring Pool<str> / Pool<[int]>
    var sp: Pool<str> = poolNew()
    reserve(mut sp, 10)
    assert cap(sp) >= 10
    let sa = poolAdd(mut sp, "alpha-" + "1")
    let sb = poolAdd(mut sp, "beta-" + "2")
    assert len(sp) == 2
    assert len(poolHandles(sp)) == 2
    match poolRemove(mut sp, sa) {
        Some(rv) => assert rv == "alpha-1"
        None => assert false
    }
    assert !poolSet(mut sp, sa, "stale-" + "value")
    assert poolSet(mut sp, sb, "beta-" + "updated")
    let sb2 = poolAdd(mut sp, "gamma")
    assert len(sp) == 2
    clear(mut sp)
    assert len(sp) == 0 && !poolHas(sp, sb) && !poolHas(sp, sb2)
    let sc = poolAdd(mut sp, "after-clear")
    assert sc != sb2 && poolHas(sp, sc)

    // Generic struct Graph<T> & zero-sized Unit
    var gp: Pool<[int]> = poolNew()
    let ga = poolAdd(mut gp, [1, 2])
    let gs = Graph<[int]> { nodes: gp, roots: [ga] }
    print(len(poolGet(gs.nodes, gs.roots[0])?))

    var up: Pool<Unit> = poolWithCap(2)
    let u0 = poolAdd(mut up, Unit {})
    assert poolHas(up, u0) && poolSet(mut up, u0, Unit {})
    match poolRemove(mut up, u0) {
        Some(deadUnit) => {
            let dummy = deadUnit
        }
        None => assert false
    }
    assert !poolHas(up, u0)

    print(monoLookup(5))
    print(nonMonoLookup(5))
}
"""
    # Need Option match or '?' inside main -> helper for '?' or match directly; main returns void, so replace `?` in main with helper!
    diff_src = diff_src.replace(
        "let node = poolGet(dll, ch)?",
        "var val = 0\n                var nxt: Handle<DNode>? = None\n                match poolGet(dll, ch) {\n                    Some(n) => {\n                        val = n.val\n                        nxt = n.next\n                    }\n                    None => assert false\n                }",
    ).replace(
        "fsum += node.val\n                fcur = node.next",
        "fsum += val\n                fcur = nxt",
    ).replace(
        "        let d0 = mapGet(dist, u)?\n        let node = poolGet(g, u)?",
        "        var d0 = 0\n        match mapGet(dist, u) {\n            Some(dv) => { d0 = dv }\n            None => assert false\n        }\n        var outEdges: [Handle<Vertex>] = []\n        match poolGet(g, u) {\n            Some(nv) => { outEdges = nv.edges }\n            None => assert false\n        }",
    ).replace(
        "        for nx in node.edges {",
        "        for nx in outEdges {",
    ).replace(
        "    print(mapGet(dist, v1)?)",
        "    match mapGet(dist, v1) {\n        Some(dv) => print(dv)\n        None => assert false\n    }",
    ).replace(
        "    print(len(poolGet(gs.nodes, gs.roots[0])?))",
        "    match poolGet(gs.nodes, gs.roots[0]) {\n        Some(arr) => print(len(arr))\n        None => assert false\n    }",
    )

    out, _ = run_both(cli, diff_src, workdir)
    # monoLookup(5): 1 + 4 + 7 + 10 + 13 = 35; nonMonoLookup(5): 1 + 2 + 3 + 4 + 5 = 15
    assert out.splitlines() == ["70", "20", "1", "2", "35", "15"], out

    # Runtime traps for poolWithCap(-1) and poolWithCap(14680065)
    trap_neg = """
fn main() {
    let p: Pool<int> = poolWithCap(-1)
    print(len(p))
}
"""
    tid_neg, _ = run_both(cli, trap_neg, workdir, expect_trap=True)
    assert tid_neg == "R_ARRAY_SIZE", tid_neg

    trap_alloc = """
fn main() {
    let p: Pool<int> = poolWithCap(14680065)
    print(len(p))
}
"""
    tid_alloc, _ = run_both(cli, trap_alloc, workdir, expect_trap=True)
    assert tid_alloc == "R_ALLOC", tid_alloc

    # emit-ir verification: monotonic vs non-monotonic helpers
    ir_file = workdir / "mono_ir.turi"
    ir_file.write_text(diff_src, encoding="utf-8", newline="\n")
    ir_p = run_cmd([str(cli), "emit-ir", str(ir_file)])
    assert ir_p.returncode == 0, ir_p.stderr
    mono_fn_ir = extract_llvm_fn(ir_p.stdout, "monoLookup")
    nonmono_fn_ir = extract_llvm_fn(ir_p.stdout, "nonMonoLookup")
    assert "@tz.poolGetMono" in mono_fn_ir and "@tz.poolHasMono" in mono_fn_ir, mono_fn_ir
    assert "@tz.poolGet." not in mono_fn_ir and "@tz.poolHas." not in mono_fn_ir, mono_fn_ir
    assert "@tz.poolGet." in nonmono_fn_ir and "@tz.poolHas." in nonmono_fn_ir, nonmono_fn_ir
    assert "@tz_col_pool_get_ptr" in ir_p.stdout, ir_p.stdout

    # Native malloc audit on full Pool<T> stress loop: live_allocs == 0
    clang = find_clang()
    pool_stress = """
struct Entry {
    xs: [int]
    name: str
}

fn main() {
    var total = 0
    for round in 0..80 {
        var p: Pool<Entry> = poolWithCap(4)
        var hs: [Handle<Entry>] = []
        for j in 0..6 {
            push(mut hs, poolAdd(mut p, Entry { xs: [j, j + 1], name: "item-{j}" }))
        }
        var pCopy = p
        let r0 = poolRemove(mut p, hs[0])
        let r1 = poolRemove(mut p, hs[2])
        assert !poolSet(mut p, hs[0], Entry { xs: [99], name: "stale-{round}" })
        assert poolSet(mut p, hs[1], Entry { xs: [77, 88], name: "alive-{round}" })
        let reused = poolAdd(mut p, Entry { xs: [55], name: "reused" })
        for h in poolHandles(pCopy) {
            match poolGet(pCopy, h) {
                Some(e) => { total += e.xs[0] }
                None => {}
            }
        }
        match poolGet(p, reused) {
            Some(e) => { total += e.xs[0] }
            None => {}
        }
        clear(mut p)
        assert isSome(r0) && isSome(r1)
    }
    assert total > 0
}
"""
    total_p, live_p = compile_and_audit(cli, clang, pool_stress, workdir, "pool_stress")
    assert total_p > 0 and live_p == 0, (total_p, live_p)


def test_shared_perceus_and_audit(cli: Path, workdir: Path):
    diff_src = """
enum PList {
    Nil
    Cons(head: int, tail: Shared<PList>)
}

struct Unit {}

fn sumPList(n: PList) -> int {
    match n {
        PList.Nil => return 0
        PList.Cons(h, tail) => return h + sumPList(sharedGet(tail))
    }
}

fn borrowAndFresh(s: Shared<int>, fresh: Shared<int>) -> int {
    return sharedGet(s) + sharedGet(fresh)
}

fn main() {
    let base = sharedNew(PList.Cons(2, sharedNew(PList.Cons(1, sharedNew(PList.Nil)))))
    let left = PList.Cons(10, base)
    let right = PList.Cons(20, base)
    print(sumPList(left))
    print(sumPList(right))

    // Diamond DAG & COW vs unique sharedSet
    let leaf = sharedNew([1, 2])
    var a = leaf
    var b = leaf
    sharedSet(mut a, [3, 4, 5])
    assert len(sharedGet(b)) == 2
    assert len(sharedGet(a)) == 3
    // Now b holds the single remaining reference to [1, 2]
    let tmp = leaf
    let c = tmp
    assert len(sharedGet(c)) == 2
    sharedSet(mut a, [6, 7, 8, 9])
    print(len(sharedGet(a)) + len(sharedGet(b)))

    // sharedUpdate with unannotated closure (unique & shared)
    var u = sharedNew(10)
    sharedUpdate(mut u, |x| x * 3 + 2)
    print(sharedGet(u))

    var su1 = sharedNew([10, 20])
    let su2 = su1
    sharedUpdate(mut su1, |arr| [arr[0] + 1, arr[1] + 2])
    print(sharedGet(su1)[0] + sharedGet(su2)[0])

    var ss = sharedNew("hi")
    sharedUpdate(mut ss, |t| t + "-ok")
    print(sharedGet(ss))

    var sUnit = sharedNew(Unit {})
    sharedSet(mut sUnit, Unit {})
    sharedUpdate(mut sUnit, |z| z)

    let last = sharedNew(5)
    print(borrowAndFresh(last, sharedNew(20)))
}
"""
    out, _ = run_both(cli, diff_src, workdir)
    assert out.splitlines() == ["13", "23", "6", "32", "21", "hi-ok", "25"], out

    clang = find_clang()
    # Audit 1: 500x cloning Shared<[int]> does ZERO extra mallocs (comparing N=1 vs N=500)
    clone_tpl = """
struct PairSh {
    x: Shared<[int]>
    y: Shared<[int]>
}

fn sinkSh(p: PairSh) -> int {
    return 1
}

fn main() {
    let base = sharedNew([1, 2, 3, 4])
    var acc = 0
    for i in 0..COUNT {
        let p = PairSh { x: base, y: base }
        acc += sinkSh(p)
    }
    assert acc == COUNT && len(sharedGet(base)) == 4
}
"""
    tot_c1, live_c1 = compile_and_audit(cli, clang, clone_tpl.replace("COUNT", "1"), workdir, "sh_clone1")
    tot_c500, live_c500 = compile_and_audit(cli, clang, clone_tpl.replace("COUNT", "500"), workdir, "sh_clone500")
    assert tot_c1 == tot_c500 and live_c1 == 0 and live_c500 == 0, (tot_c1, tot_c500, live_c1, live_c500)

    # Audit 2: 500x unique sharedSet + 500x unique sharedUpdate + 500x compiler-fused sharedReuse = 0 extra cell mallocs!
    reuse_src = """
fn main() {
    var s1 = sharedNew(0)
    var s2 = sharedNew(0)
    var s3 = sharedNew(0)
    for i in 0..500 {
        sharedSet(mut s1, i)
        sharedUpdate(mut s2, |x| x + 1)
        s3 = sharedNew(sharedGet(s3) + 1)
    }
    assert sharedGet(s1) == 499 && sharedGet(s2) == 500 && sharedGet(s3) == 500
}
"""
    tot_r, live_r = compile_and_audit(cli, clang, reuse_src, workdir, "sh_reuse")
    assert tot_r == 3 and live_r == 0, (tot_r, live_r)


def test_costes_max_level_and_diagnostics(cli: Path, workdir: Path):
    levels_src = """
fn useRegion(n: int) -> int {
    let a = repeat(1, n)
    var s = 0
    for x in a {
        s += x
    }
    return s
}

fn useOwnerAndClone(v: [int]) -> int {
    let owned = [1, 2, 3]
    let cloned = v
    return owned[0] + cloned[0]
}

fn useMonoPool() -> int {
    var p: Pool<int> = poolNew()
    let h = poolAdd(mut p, 42)
    match poolGet(p, h) {
        Some(v) => return v
        None => return 0
    }
}

fn useNonMonoPool() -> int {
    var p: Pool<int> = poolNew()
    let h = poolAdd(mut p, 7)
    let rem = poolRemove(mut p, h)
    match rem {
        Some(v) => return v
        None => return 0
    }
}

fn useRc() -> int {
    let s = sharedNew(11)
    return sharedGet(s)
}

fn useRcReuse() -> int {
    var s = sharedNew(1)
    s = sharedNew(sharedGet(s) + 1)
    return sharedGet(s)
}

fn main() {
    print(useRegion(4) + useOwnerAndClone([5]) + useMonoPool() + useNonMonoPool() + useRc() + useRcReuse())
}
"""
    lvl_file = workdir / "levels.turi"
    lvl_file.write_text(levels_src, encoding="utf-8", newline="\n")

    r_cost = run_cmd([str(cli), "costes", str(lvl_file)])
    assert r_cost.returncode == 0, r_cost.stderr
    sites = [json.loads(line) for line in r_cost.stdout.splitlines() if line.strip()]
    by_fn = {}
    for s in sites:
        by_fn.setdefault(s["function"], []).append(s)

    assert any(s["level"] == 2 and s["kind"] == "region" for s in by_fn["useRegion"]), by_fn["useRegion"]
    assert any(s["level"] == 3 and s["kind"] == "owner" for s in by_fn["useOwnerAndClone"]), by_fn["useOwnerAndClone"]
    assert any(s["level"] == 3 and s["kind"] == "clone" for s in by_fn["useOwnerAndClone"]), by_fn["useOwnerAndClone"]
    mono_site = next(s for s in by_fn["useMonoPool"] if s["kind"] == "pool")
    assert mono_site["level"] == 4 and "pool monótono" in mono_site["reason"], mono_site
    nonmono_site = next(s for s in by_fn["useNonMonoPool"] if s["kind"] == "pool")
    assert nonmono_site["level"] == 4 and "pool monótono" not in nonmono_site["reason"], nonmono_site
    assert any(s["level"] == 5 and s["kind"] == "rc" for s in by_fn["useRc"]), by_fn["useRc"]
    assert any(s["level"] == 5 and s["kind"] == "rc-reuse" for s in by_fn["useRcReuse"]), by_fn["useRcReuse"]

    # Default without --max-level passes check and build
    assert run_cmd([str(cli), "check", str(lvl_file)]).returncode == 0
    exe_ok = workdir / ("lvl_ok.exe" if os.name == "nt" else "lvl_ok")
    assert run_cmd([str(cli), "build", str(lvl_file), "-o", str(exe_ok)]).returncode == 0

    # --max-level N enforcement on costes, check, and build
    exe_fail = workdir / ("lvl_fail.exe" if os.name == "nt" else "lvl_fail")
    for cmd in [
        [str(cli), "costes", str(lvl_file), "--max-level", "3"],
        [str(cli), "check", str(lvl_file), "--max-level", "3"],
        [str(cli), "build", str(lvl_file), "-o", str(exe_fail), "--max-level", "3"],
    ]:
        fail = run_cmd(cmd)
        assert fail.returncode != 0, cmd
        d = json.loads(fail.stderr.strip().splitlines()[-1])
        assert d["id"] == "E_MEMORY_LEVEL", d
        assert "useMonoPool" in d["message"], d
        assert ("pool" in d["message"] or "poolNew" in d["message"]), d

    # --max-level 5 allows all levels 2..5
    assert run_cmd([str(cli), "costes", str(lvl_file), "--max-level", "5"]).returncode == 0
    assert run_cmd([str(cli), "check", str(lvl_file), "--max-level", "5"]).returncode == 0
    assert run_cmd([str(cli), "build", str(lvl_file), "-o", str(exe_ok), "--max-level", "5"]).returncode == 0

    # Filtering by function allows --max-level 2 for useRegion while failing for useOwnerAndClone
    assert run_cmd([str(cli), "costes", str(lvl_file), "useRegion", "--max-level", "2"]).returncode == 0
    f_reg = run_cmd([str(cli), "costes", str(lvl_file), "useOwnerAndClone", "--max-level", "2"])
    assert f_reg.returncode != 0 and json.loads(f_reg.stderr.strip().splitlines()[-1])["id"] == "E_MEMORY_LEVEL"

    # Invalid --max-level fails with E_USAGE
    for bad_lvl in ["-1", "6", "abc"]:
        u = run_cmd([str(cli), "costes", str(lvl_file), "--max-level", bad_lvl])
        assert u.returncode != 0 and json.loads(u.stderr.strip().splitlines()[-1])["id"] == "E_USAGE"

    # False-friend and ownership diagnostics
    expect_diag(
        cli,
        "fn main() {\n    var p: Pool<int> = poolNew()\n    let h = poolAdd(mut p, 1)\n    let x = p[h]\n}\n",
        workdir,
        "E_POOL_INDEX",
    )
    for alias_name in ["Arena", "SlotMap", "Slab"]:
        d_pool = expect_diag(
            cli,
            f"fn main() {{\n    var p: {alias_name}<int> = poolNew()\n}}\n",
            workdir,
            "E_POOL_NAME",
        )
        assert "Pool<T>" in d_pool["message"] or "Pool" in d_pool.get("example", ""), d_pool

    for rc_name in ["Rc", "Arc", "SharedPtr"]:
        d_rc = expect_diag(
            cli,
            f"fn main() {{\n    let s: {rc_name}<int> = sharedNew(1)\n}}\n",
            workdir,
            "E_RC_NAME",
        )
        assert "Shared<T>" in d_rc["message"] or "Shared" in d_rc.get("example", ""), d_rc

    d_cycle = expect_diag(cli, "struct Loop {\n    next: Loop\n}\nfn main() {}\n", workdir, "E_TYPE_CYCLE")
    assert "Handle<" in d_cycle["message"] or "Pool<" in d_cycle["message"], d_cycle

    d_alias = expect_diag(
        cli,
        "struct Pair {\n    a: [int]\n    b: [int]\n}\nfn f(p: mut Pair, a: mut [int]) {}\nfn main() {\n    var x = Pair { a: [1], b: [2] }\n    f(mut x, mut x.a)\n}\n",
        workdir,
        "E_ALIAS_MUT",
    )
    assert "x" in d_alias["message"] and d_alias.get("related"), d_alias

    for topic in [
        "E_POOL_INDEX",
        "E_POOL_NAME",
        "E_RC_NAME",
        "E_MEMORY_LEVEL",
        "E_TYPE_CYCLE",
        "E_ALIAS_MUT",
        "E_KEY_TYPE",
    ]:
        exp = run_cmd([str(cli), "explain", topic])
        assert exp.returncode == 0 and exp.stdout.strip(), (topic, exp)


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m26a-") as tmp:
        workdir = Path(tmp)
        test_move_elision_and_audit(cli, workdir)
        test_pool_and_handles(cli, workdir)
        test_shared_perceus_and_audit(cli, workdir)
        test_costes_max_level_and_diagnostics(cli, workdir)
    print("turi03_m26a: ok")


if __name__ == "__main__":
    main()
