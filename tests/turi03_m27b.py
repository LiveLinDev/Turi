"""Pruebas de concurrencia estructurada: Scope, Task<T>, Frozen<T>, robo de trabajo y movimientos entre hilos (Hito 2.7B)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_cmd(args, env=None, text=True, cwd=None):
    return subprocess.run(
        args,
        capture_output=True,
        text=text,
        encoding="utf-8" if text else None,
        env=env,
        cwd=str(cwd) if cwd else None,
    )


def find_clang() -> str:
    if os.environ.get("VELOZ_CLANG"):
        return os.environ["VELOZ_CLANG"]
    win_default = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang.exe"
    if win_default.is_file():
        return str(win_default)
    return "clang"


def run_both(cli: Path, source: str, workdir: Path, expect_trap=False, check_fmt=True):
    src = workdir / "main.turi"
    exe = workdir / ("prog.exe" if os.name == "nt" else "prog")
    src.write_text(source, encoding="utf-8", newline="\n")

    if check_fmt:
        fmt = run_cmd([str(cli), "fmt", str(src)], cwd=workdir)
        assert fmt.returncode == 0, f"fmt failed: {fmt.stderr}\n{source}"
        chk = run_cmd([str(cli), "check", str(src), "--format"], cwd=workdir)
        assert chk.returncode == 0, f"check --format failed: {chk.stderr}\n{src.read_text(encoding='utf-8')}"

    interp = run_cmd([str(cli), "run", str(src)], cwd=workdir)

    bld = run_cmd([str(cli), "build", str(src), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"build failed: {bld.stderr}\n{source}"

    nat = run_cmd([str(exe)], cwd=workdir)

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
    assert interp.returncode == 0, (
        f"unexpected error rc={interp.returncode}\ninterp: {interp.stderr}\nnat: {nat.stderr}\n{source}"
    )
    assert interp.stdout == nat.stdout, (
        f"stdout mismatch:\ninterp={interp.stdout!r}\nnative={nat.stdout!r}\n{source}"
    )
    assert interp.stderr == nat.stderr, (
        f"stderr mismatch:\ninterp={interp.stderr!r}\nnative={nat.stderr!r}\n{source}"
    )
    return interp.stdout, interp.stderr


def expect_diag(cli: Path, source: str, workdir: Path, expected_id: str, extra_args=()):
    src = workdir / "diag.turi"
    src.write_text(source, encoding="utf-8", newline="\n")
    r = run_cmd([str(cli), "check", str(src), *extra_args], cwd=workdir)
    assert r.returncode != 0, f"expected compile error {expected_id}, got OK\n{source}"
    line = r.stderr.strip().splitlines()[-1]
    d = json.loads(line)
    assert d["id"] == expected_id, f"expected {expected_id}, got {d}\n{source}"
    return d


def inject_c_audit(c_text: str, anchor: str) -> str:
    audit_macros = (
        "void* audit_malloc(size_t n);\n"
        "void* audit_calloc(size_t nmemb, size_t size);\n"
        "void* audit_realloc(void* p, size_t n);\n"
        "void audit_free(void* p);\n"
        "#define malloc audit_malloc\n"
        "#define calloc audit_calloc\n"
        "#define realloc audit_realloc\n"
        "#define free audit_free\n"
    )
    assert anchor in c_text, f"anchor not found: {anchor}"
    return c_text.replace(anchor, audit_macros + anchor, 1)


def compile_and_audit_conc(cli: Path, clang: str, src_text: str, workdir: Path, name: str, opt_flag: str):
    src_file = workdir / f"{name}.turi"
    src_file.write_text(src_text, encoding="utf-8", newline="\n")
    ir_p = run_cmd([str(cli), "emit-ir", str(src_file)], cwd=workdir)
    assert ir_p.returncode == 0, f"emit-ir failed: {ir_p.stderr}"
    ir = ir_p.stdout
    for orig, rep in (
        ("@malloc(", "@audit_malloc("),
        ("@calloc(", "@audit_calloc("),
        ("@realloc(", "@audit_realloc("),
        ("@free(", "@audit_free("),
        ("declare noalias ptr @malloc(i64)", "declare noalias ptr @audit_malloc(i64)"),
        ("declare noalias ptr @calloc(i64, i64)", "declare noalias ptr @audit_calloc(i64, i64)"),
        ("declare ptr @realloc(ptr, i64)", "declare ptr @audit_realloc(ptr, i64)"),
        ("declare void @free(ptr)", "declare void @audit_free(ptr)"),
    ):
        ir = ir.replace(orig, rep)
    ir_wrapped = ir.replace("define i32 @main()", "define i32 @turi_main()")
    ir_file = workdir / f"{name}_{opt_flag[1:]}.ll"
    ir_file.write_text(ir_wrapped, encoding="utf-8")

    src_dir = Path(__file__).resolve().parents[1] / "src"

    def extract_rt(header_name: str) -> str:
        txt = (src_dir / header_name).read_text(encoding="utf-8")
        c_start = txt.index('R"c(\n') + 5
        c_end = txt.index('\n)c";')
        return txt[c_start:c_end]

    sys_file = workdir / f"{name}_sys_rt.c"
    sys_file.write_text(
        inject_c_audit(extract_rt("native_sys_rt.hpp"), "typedef struct tz_text {"),
        encoding="utf-8",
    )

    conc_file = workdir / f"{name}_conc_rt.c"
    conc_file.write_text(
        inject_c_audit(extract_rt("native_conc_rt.hpp"), "#define TZ_MAX_WORKERS 16"),
        encoding="utf-8",
    )

    shim_file = workdir / f"{name}_shim.c"
    shim_file.write_text("""
#include <stdio.h>
#include <stdlib.h>

static long long g_total_allocs = 0;
static long long g_live_allocs = 0;

void* audit_malloc(size_t n) {
    void* p = malloc(n > 0 ? n : 1);
    if (p) {
        __atomic_add_fetch(&g_total_allocs, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
    }
    return p;
}
void* audit_calloc(size_t nmemb, size_t size) {
    void* p = calloc(nmemb > 0 ? nmemb : 1, size > 0 ? size : 1);
    if (p) {
        __atomic_add_fetch(&g_total_allocs, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
    }
    return p;
}
void* audit_realloc(void* p, size_t n) {
    if (!p) return audit_malloc(n);
    if (n == 0) {
        free(p);
        __atomic_sub_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
        return NULL;
    }
    return realloc(p, n);
}
void audit_free(void* p) {
    if (p) {
        __atomic_sub_fetch(&g_live_allocs, 1, __ATOMIC_ACQ_REL);
        free(p);
    }
}

int turi_main(void);

int main(void) {
    int rc = turi_main();
    long long tot = __atomic_load_n(&g_total_allocs, __ATOMIC_ACQUIRE);
    long long live = __atomic_load_n(&g_live_allocs, __ATOMIC_ACQUIRE);
    printf("%lld %lld\\n", tot, live);
    return rc;
}
""".lstrip(), encoding="utf-8")

    exe = workdir / f"{name}_audit_{opt_flag[1:]}{'.exe' if os.name == 'nt' else ''}"
    cmd = [
        clang,
        opt_flag,
        "-Wno-override-module",
        "-DTZ_USE_NET",
        str(shim_file),
        str(sys_file),
        str(conc_file),
        "-x",
        "ir",
        str(ir_file),
        "-o",
        str(exe),
    ]
    if os.name == "nt":
        cmd.extend(["-lws2_32", "-Xlinker", "/STACK:33554432"])
    else:
        cmd.extend(["-lm", "-lpthread"])
    cc = run_cmd(cmd, cwd=workdir)
    assert cc.returncode == 0, f"audit compile failed ({opt_flag}): {cc.stderr}"
    run_res = run_cmd([str(exe)], cwd=workdir)
    assert run_res.returncode == 0, f"audit run failed ({opt_flag}): {run_res.stdout!r} {run_res.stderr!r}"
    total_allocs, live_allocs = map(int, run_res.stdout.strip().splitlines()[-1].split())
    return total_allocs, live_allocs


def test_scope_spawn_join_types(cli: Path, workdir: Path):
    src = r"""
struct Msg {
    title: str
    items: [int]
}

fn checkDiv(a: int, b: int) -> Result<int, str> {
    if b == 0 {
        return Err("divzero")
    }
    return Ok(a / b)
}

fn main() {
    var s = taskScope()
    let tInt = taskSpawn(mut s, || 40 + 2)
    let prefix = "hola"
    let tStr = taskSpawn(mut s, || "{prefix}-mundo")
    let tArr = taskSpawn(mut s, || {
        var xs = [1, 2]
        push(mut xs, 3)
        return xs
    })
    let tMsg = taskSpawn(mut s, || Msg { title: "box-" + "ok", items: [7, 8, 9] })
    let tOk = taskSpawn(mut s, || checkDiv(84, 2))
    let tErr = taskSpawn(mut s, || checkDiv(10, 0))
    let tVoid: Task<void> = taskSpawn(mut s, || {
        let tmp = [1, 2, 3]
        assert len(tmp) == 3
    })

    taskJoin(tVoid)
    print(taskJoin(tInt))
    print(taskJoin(tStr))
    let arr = taskJoin(tArr)
    print(len(arr))
    print(arr[2])
    let msg = taskJoin(tMsg)
    print(msg.title)
    print(len(msg.items))
    print(msg.items[1])
    match taskJoin(tOk) {
        Ok(v) => print(v)
        Err(err) => print(err)
    }
    match taskJoin(tErr) {
        Ok(v) => print(v)
        Err(err) => print(err)
    }
}
""".lstrip()
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == ["42", "hola-mundo", "3", "3", "box-ok", "3", "8", "42", "divzero"]


def test_task_spawn_move_resources_and_values(cli: Path, workdir: Path):
    src = r"""
struct Payload {
    tag: str
    nums: [int]
}

fn buildPayload() -> Payload {
    return Payload { tag: "mv", nums: [5, 6, 7] }
}

fn main() {
    var sc = taskScope()
    let named = [10, 20]
    let tNamed = taskSpawnMove(mut sc, named, |v| {
        push(mut v, 30)
        return len(v) * 100 + v[2]
    })
    let tRval = taskSpawnMove(mut sc, buildPayload(), |p| {
        return p.tag + ":" + str(len(p.nums)) + ":" + str(p.nums[2])
    })
    let tDropOnly = taskSpawnMove(mut sc, ["uno", "dos"], |v| {
        assert len(v) == 2
        return len(v[0]) + len(v[1])
    })

    var fOut = fileOpen("moved_task.txt", "w")
    match fOut {
        Ok(file) => {
            let tFile = taskSpawnMove(mut sc, file, |fh| {
                let r = fileWrite(mut fh, "desde-hilo")
                match r {
                    Ok(n) => return n
                    Err(err) => return -1
                }
            })
            let written = taskJoin(tFile)
            print(written)
        }
        Err(err) => print(-1)
    }

    var mx = mutexNew()
    let tMutex = taskSpawnMove(mut sc, mx, |m| {
        var g = mutexLock(mut m)
        return lockIsHeld(g)
    })

    print(taskJoin(tNamed))
    print(len(named))
    print(taskJoin(tRval))
    print(taskJoin(tDropOnly))
    print(taskJoin(tMutex))
    print(readFile("moved_task.txt"))
}
""".lstrip()
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == ["10", "330", "2", "mv:3:7", "6", "true", "desde-hilo"]


def test_nested_recursive_work_stealing(cli: Path, workdir: Path):
    src = r"""
fn parSum(lo: int, hi: int, thresh: int) -> int {
    if hi - lo <= thresh {
        var acc = 0
        for i in lo..hi {
            acc = acc + i
        }
        return acc
    }
    let mid = lo + (hi - lo) / 2
    var sc = taskScope()
    let left = taskSpawn(mut sc, || parSum(lo, mid, thresh))
    let right = taskSpawn(mut sc, || parSum(mid, hi, thresh))
    return taskJoin(left) + taskJoin(right)
}

fn main() {
    let total = parSum(1, 1025, 16)
    print(total)
}
""".lstrip()
    out, _ = run_both(cli, src, workdir)
    assert out.strip() == str(1024 * 1025 // 2)


def test_unjoined_tasks_and_scope_lifetime(cli: Path, workdir: Path):
    src = r"""
fn earlyReturnWithUnjoined(flag: bool) -> int {
    var sc = taskScope()
    let t1 = taskSpawn(mut sc, || repeat(9, 12))
    let t2 = taskSpawn(mut sc, || "temp-" + "str")
    if flag {
        return 77
    }
    return len(taskJoin(t1)) + len(taskJoin(t2))
}

fn tryUnwindWithUnjoined(failIt: bool) -> Result<int, str> {
    var sc = taskScope()
    let t1 = taskSpawn(mut sc, || ["a", "b", "c"])
    if failIt {
        let err: Result<int, str> = Err("boom")
        let val = err?
        return Ok(val)
    }
    return Ok(len(taskJoin(t1)))
}

fn spawnOutlivingInt() -> Task<int> {
    var sc = taskScope()
    return taskSpawn(mut sc, || 1200 + 34)
}

fn spawnOutlivingArr() -> Task<[int]> {
    var sc = taskScope()
    return taskSpawn(mut sc, || [3, 1, 4, 1, 5])
}

fn main() {
    if true {
        var sc = taskScope()
        let u1 = taskSpawn(mut sc, || [1, 2, 3, 4])
        let u2 = taskSpawn(mut sc, || "sin-unir")
    }
    print(earlyReturnWithUnjoined(true))
    match tryUnwindWithUnjoined(true) {
        Ok(v) => print(v)
        Err(err) => print(err)
    }
    let ti = spawnOutlivingInt()
    let ta = spawnOutlivingArr()
    print(taskJoin(ti))
    let xs = taskJoin(ta)
    print(len(xs))
    print(xs[4])
}
""".lstrip()
    out, _ = run_both(cli, src, workdir)
    assert out.splitlines() == ["77", "boom", "1234", "5", "5"]


def test_parallel_map_and_freeze(cli: Path, workdir: Path):
    src = r"""
struct Empty {
}

fn sumSlice(xs: [int]) -> int {
    var acc = 0
    for x in xs {
        acc = acc + x
    }
    return acc
}

fn main() {
    let e0 = parallelMap(0, |i| i + 1)
    let e1 = parallelMap(1, |i| i + 10)
    print(len(e0))
    print(len(e1))
    print(e1[0])

    let base = [2, 4, 6, 8]
    let fz: Frozen<[int]> = freeze(base)
    let mapped = parallelMap(128, |i| frozenRead(fz, |xs| xs[i % len(xs)] + i))
    print(len(mapped))
    print(mapped[0])
    print(mapped[127])
    print(sumSlice(mapped))

    let strs = parallelMap(32, |i| "k-{i}")
    print(len(strs))
    print(strs[0])
    print(strs[31])

    var sc = taskScope()
    let fz2 = fz
    let t1 = taskSpawn(mut sc, || frozenRead(fz, |xs| sumSlice(xs)))
    let t2 = taskSpawn(mut sc, || {
        let copy = frozenGet(fz2)
        return len(copy) * 10 + copy[3]
    })
    print(taskJoin(t1))
    print(taskJoin(t2))

    let fe = freeze(Empty {})
    print(frozenRead(fe, |emptyVal| 77))
}
""".lstrip()
    out, _ = run_both(cli, src, workdir)
    expected_sum = sum((2 * ((i % 4) + 1)) + i for i in range(128))
    assert out.splitlines() == [
        "0",
        "1",
        "10",
        "128",
        "2",
        "135",
        str(expected_sum),
        "32",
        "k-0",
        "k-31",
        "20",
        "48",
        "77",
    ]

    trap_src = r"""
fn main() {
    let bad = parallelMap(-1, |i| i)
    print(len(bad))
}
""".lstrip()
    tid, _ = run_both(cli, trap_src, workdir, expect_trap=True)
    assert tid == "R_ARRAY_SIZE"


def test_native_tcp_echo_concurrency(cli: Path, workdir: Path):
    src = r"""
fn serveOnce(l: mut TcpListener) -> Result<str, str> {
    var conn = tcpAccept(mut l)?
    var acc: str = ""
    while len(acc) < 9 {
        let chunk = socketRead(mut conn, 64)?
        if len(chunk) == 0 {
            break
        }
        acc = acc + chunk
    }
    let written = socketWrite(mut conn, "ACK:" + acc)?
    assert written >= 0
    return Ok(acc)
}

fn clientOnce(port: int) -> Result<str, str> {
    var sock = tcpConnect("127.0.0.1", port)?
    let sent = socketWrite(mut sock, "PING-1234")?
    assert sent > 0
    var resp: str = ""
    while true {
        let chunk = socketRead(mut sock, 64)?
        if len(chunk) == 0 {
            break
        }
        resp = resp + chunk
    }
    return Ok(resp)
}

fn main() {
    let lr = tcpListen("127.0.0.1", 0)
    match lr {
        Err(err) => print("listen-err:" + err)
        Ok(listener) => {
            let port = tcpLocalPort(listener)
            var sc = taskScope()
            let srv = taskSpawnMove(mut sc, listener, |l| serveOnce(mut l))
            let cli = taskSpawn(mut sc, || clientOnce(port))
            match taskJoin(srv) {
                Ok(s) => print(s)
                Err(err) => print("srv-err:" + err)
            }
            match taskJoin(cli) {
                Ok(c) => print(c)
                Err(err) => print("cli-err:" + err)
            }
        }
    }
}
""".lstrip()
    src_file = workdir / "tcp_conc.turi"
    exe = workdir / ("tcp_conc.exe" if os.name == "nt" else "tcp_conc")
    src_file.write_text(src, encoding="utf-8", newline="\n")
    bld = run_cmd([str(cli), "build", str(src_file), "-o", str(exe)], cwd=workdir)
    assert bld.returncode == 0, f"tcp conc build failed: {bld.stderr}"
    run_res = run_cmd([str(exe)], cwd=workdir)
    assert run_res.returncode == 0, f"tcp conc run failed: {run_res.stdout!r} {run_res.stderr!r}"
    assert run_res.stdout.splitlines() == ["PING-1234", "ACK:PING-1234"]


def test_native_malloc_audit_conc(cli: Path, clang: str, workdir: Path):
    audit_src = r"""
struct Boxed {
    tag: str
    nums: [int]
}

fn parTree(d: int) -> int {
    if d <= 0 {
        return 1
    }
    var sc = taskScope()
    let a = taskSpawn(mut sc, || parTree(d - 1))
    let b = taskSpawn(mut sc, || parTree(d - 1))
    return taskJoin(a) + taskJoin(b)
}

fn spawnOutlive() -> Task<[int]> {
    var sc = taskScope()
    return taskSpawn(mut sc, || [11, 22, 33])
}

fn main() {
    var sc = taskScope()
    let t1 = taskSpawn(mut sc, || 40 + 2)
    let t2 = taskSpawn(mut sc, || [1, 2, 3, 4])
    let t3: Task<void> = taskSpawn(mut sc, || {
        let tmp = "work-" + "done"
        assert len(tmp) == 9
    })
    let tUnjoined = taskSpawn(mut sc, || repeat(5, 8))
    let tUnjoinedStr = taskSpawn(mut sc, || "drop-" + "me")
    let tMove = taskSpawnMove(mut sc, Boxed { tag: "mv", nums: [7, 8] }, |b| {
        return len(b.tag) + len(b.nums)
    })
    var mx = mutexNew()
    let tMx = taskSpawnMove(mut sc, mx, |m| {
        var g = mutexLock(mut m)
        return lockIsHeld(g)
    })

    let fz = freeze([3, 6, 9, 12])
    let pm = parallelMap(64, |i| frozenRead(fz, |xs| "v=" + str(xs[i % len(xs)] + i)))
    let copied = frozenGet(fz)
    let tree = parTree(3)

    taskJoin(t3)
    assert taskJoin(t1) == 42
    assert len(taskJoin(t2)) == 4
    assert taskJoin(tMove) == 4
    assert taskJoin(tMx)
    assert len(pm) == 64
    assert len(copied) == 4
    assert tree == 8
    let outTask = spawnOutlive()
    assert len(taskJoin(outTask)) == 3
    let _hold = (tUnjoined, tUnjoinedStr)
}
""".lstrip()
    # Simplify the hold line (Turi doesn't have tuple syntax; keep named unjoined tasks)
    audit_src = audit_src.replace(
        "    let _hold = (tUnjoined, tUnjoinedStr)\n",
        "    assert taskJoin(taskSpawn(mut sc, || frozenRead(fz, |xs| xs[0]))) == 3\n",
    )
    for opt in ("-O0", "-O2"):
        total_allocs, live_allocs = compile_and_audit_conc(cli, clang, audit_src, workdir, "conc_audit", opt)
        assert total_allocs > 0, f"expected heap allocations at {opt}, got {total_allocs}"
        assert live_allocs == 0, f"leaked {live_allocs} allocations at {opt} (total={total_allocs})"


def test_tooling_and_diagnostics(cli: Path, workdir: Path):
    sig_src = r"""
pure fn pureParallel(xs: [int]) -> int {
    let fz = freeze(xs)
    var sc = taskScope()
    let t1 = taskSpawn(mut sc, || frozenRead(fz, |v| len(v)))
    let ys = parallelMap(4, |i| i * 2)
    return taskJoin(t1) + len(ys)
}

fn concWithIo(xs: [int]) -> int {
    let fz = freeze(xs)
    var sc = taskScope()
    let t1 = taskSpawn(mut sc, || {
        print(99)
        return frozenRead(fz, |v| len(v))
    })
    return taskJoin(t1)
}

fn pmapWithIo() -> [int] {
    return parallelMap(2, |i| {
        print(i)
        return i
    })
}

fn main() {
    print(pureParallel([1, 2]))
    print(concWithIo([1, 2]))
    print(len(pmapWithIo()))
}
""".lstrip()
    sig_file = workdir / "sig_conc.turi"
    sig_file.write_text(sig_src, encoding="utf-8", newline="\n")
    sig_res = run_cmd([str(cli), "sig", str(sig_file)], cwd=workdir)
    assert sig_res.returncode == 0, f"veloz sig failed: {sig_res.stderr}"
    sigs = {entry["name"]: entry for entry in (json.loads(line) for line in sig_res.stdout.splitlines() if line.strip())}
    assert sigs["pureParallel"]["io"] is False
    assert "pure" in sigs["pureParallel"]["effects"]
    assert sigs["concWithIo"]["io"] is True
    assert "io" in sigs["concWithIo"]["effects"]
    assert sigs["pmapWithIo"]["io"] is True

    pure_bad = r"""
pure fn badPure() -> int {
    var sc = taskScope()
    let t = taskSpawn(mut sc, || {
        print(1)
        return 2
    })
    return taskJoin(t)
}

fn main() {
    print(badPure())
}
""".lstrip()
    expect_diag(cli, pure_bad, workdir, "E_EFFECT")

    costes_src = r"""
fn useTask() -> int {
    var sc = taskScope()
    let t = taskSpawn(mut sc, || 1)
    return taskJoin(t)
}

fn useFreeze() -> int {
    let fz = freeze([1, 2, 3])
    return frozenRead(fz, |xs| len(xs))
}

fn main() {
    print(useTask() + useFreeze())
}
""".lstrip()
    cost_file = workdir / "costes_conc.turi"
    cost_file.write_text(costes_src, encoding="utf-8", newline="\n")
    cost_res = run_cmd([str(cli), "costes", str(cost_file)], cwd=workdir)
    assert cost_res.returncode == 0, f"veloz costes failed: {cost_res.stderr}"
    sites = [json.loads(line) for line in cost_res.stdout.splitlines() if line.strip()]
    assert any(s["function"] == "useTask" and s["kind"] == "task" and s["level"] == 3 for s in sites)
    assert any(s["function"] == "useFreeze" and s["kind"] == "frozen" and s["level"] == 5 for s in sites)

    expect_diag(
        cli,
        costes_src.replace("fn useFreeze()", "fn unused()").replace("let fz = freeze([1, 2, 3])\n    return frozenRead(fz, |xs| len(xs))", "return 0").replace(" + useFreeze()", ""),
        workdir,
        "E_MEMORY_LEVEL",
        ("--max-level", "2"),
    )
    expect_diag(cli, costes_src, workdir, "E_MEMORY_LEVEL", ("--max-level", "4"))

    # E_SEND diagnostics
    expect_diag(
        cli,
        """
fn main() {
    let sh = sharedNew(5)
    var sc = taskScope()
    let t = taskSpawn(mut sc, || sharedGet(sh))
    print(taskJoin(t))
}
""".lstrip(),
        workdir,
        "E_SEND",
    )
    expect_diag(
        cli,
        """
fn main() {
    var sc = taskScope()
    let t = taskSpawn(mut sc, || sharedNew(5))
    print(sharedGet(taskJoin(t)))
}
""".lstrip(),
        workdir,
        "E_SEND",
    )
    expect_diag(
        cli,
        """
fn main() {
    let f = |x: int| x + 1
    var sc = taskScope()
    let t = taskSpawn(mut sc, || f(2))
    print(taskJoin(t))
}
""".lstrip(),
        workdir,
        "E_SEND",
    )

    # E_USE_AFTER_MOVE diagnostics
    expect_diag(
        cli,
        """
fn main() {
    var sc = taskScope()
    let t = taskSpawn(mut sc, || 1)
    print(taskJoin(t))
    print(taskJoin(t))
}
""".lstrip(),
        workdir,
        "E_USE_AFTER_MOVE",
    )
    expect_diag(
        cli,
        """
fn main() {
    let rf = fileOpen("a.txt", "w")
    match rf {
        Ok(f) => {
            var sc = taskScope()
            let t = taskSpawnMove(mut sc, f, |fh| 1)
            print(fileIsOpen(f))
            print(taskJoin(t))
        }
        Err(err) => print(err)
    }
}
""".lstrip(),
        workdir,
        "E_USE_AFTER_MOVE",
    )

    # E_RESOURCE_COPY diagnostics
    expect_diag(
        cli,
        """
fn main() {
    let rf = fileOpen("a.txt", "r")
    match rf {
        Ok(f) => {
            let fzVal = freeze(f)
        }
        Err(err) => print(err)
    }
}
""".lstrip(),
        workdir,
        "E_RESOURCE_COPY",
    )
    expect_diag(
        cli,
        """
fn main() {
    var sc = taskScope()
    let t = taskSpawn(mut sc, || 1)
    let arrVal = repeat(t, 2)
}
""".lstrip(),
        workdir,
        "E_RESOURCE_COPY",
    )
    expect_diag(
        cli,
        """
fn main() {
    var sc = taskScope()
    let t = taskSpawn(mut sc, || 1)
    let t2 = taskSpawn(mut sc, || taskJoin(t))
    print(taskJoin(t2))
}
""".lstrip(),
        workdir,
        "E_RESOURCE_COPY",
    )

    # E_CONC_NAME false-friend diagnostics + veloz explain
    for bad_src in (
        "async fn f() -> int { return 1 }\nfn main() { print(1) }\n",
        "fn main() {\n    let x = await foo\n    print(x)\n}\n",
        "fn main() {\n    let x: Thread = 1\n}\n",
        "fn main() {\n    let x: Channel = 1\n}\n",
        "fn main() {\n    let x: Future = 1\n}\n",
    ):
        expect_diag(cli, bad_src, workdir, "E_CONC_NAME")

    for topic in ("E_SEND", "E_CONC_NAME"):
        ex = run_cmd([str(cli), "explain", topic], cwd=workdir)
        assert ex.returncode == 0, f"explain {topic} failed: {ex.stderr}"
        assert ex.stdout.strip()


def main():
    if len(sys.argv) < 2:
        print("usage: turi03_m27b.py <veloz>", file=sys.stderr)
        sys.exit(2)
    cli = Path(sys.argv[1]).resolve()
    clang = find_clang()
    with tempfile.TemporaryDirectory() as td:
        workdir = Path(td)
        test_scope_spawn_join_types(cli, workdir)
        test_task_spawn_move_resources_and_values(cli, workdir)
        test_nested_recursive_work_stealing(cli, workdir)
        test_unjoined_tasks_and_scope_lifetime(cli, workdir)
        test_parallel_map_and_freeze(cli, workdir)
        test_native_tcp_echo_concurrency(cli, workdir)
        test_native_malloc_audit_conc(cli, clang, workdir)
        test_tooling_and_diagnostics(cli, workdir)
    print("ok")


if __name__ == "__main__":
    main()
