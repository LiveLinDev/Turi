"""Pruebas de recursos RAII afines y deterministas (File, TcpListener, Socket, Mutex, Lock) (Hito 2.6B)."""
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
    assert interp.returncode == 0, f"unexpected error rc={interp.returncode}\ninterp: {interp.stderr}\nnat: {nat.stderr}\n{source}"
    assert interp.stdout == nat.stdout, f"stdout mismatch:\ninterp={interp.stdout!r}\nnative={nat.stdout!r}\n{source}"
    assert interp.stderr == nat.stderr, f"stderr mismatch:\ninterp={interp.stderr!r}\nnative={nat.stderr!r}\n{source}"
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


def compile_and_audit_resources(cli: Path, clang: str, src_text: str, workdir: Path, name: str, opt_flag: str):
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

    rt_hdr = Path(__file__).resolve().parents[1] / "src/native_sys_rt.hpp"
    hdr_txt = rt_hdr.read_text(encoding="utf-8")
    c_start = hdr_txt.index('R"c(\n') + 5
    c_end = hdr_txt.index('\n)c";')
    sys_c = hdr_txt[c_start:c_end]
    inject_anchor = "typedef struct tz_text {"
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
    assert inject_anchor in sys_c
    sys_file = workdir / f"{name}_sys_rt.c"
    sys_file.write_text(sys_c.replace(inject_anchor, audit_macros + inject_anchor, 1), encoding="utf-8")

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
void* audit_calloc(size_t nmemb, size_t size) {
    void* p = calloc(nmemb > 0 ? nmemb : 1, size > 0 ? size : 1);
    if (p) { ++g_total_allocs; ++g_live_allocs; }
    return p;
}
void* audit_realloc(void* p, size_t n) {
    if (!p) return audit_malloc(n);
    if (n == 0) {
        free(p);
        --g_live_allocs;
        return NULL;
    }
    return realloc(p, n);
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

    exe = workdir / f"{name}_audit_{opt_flag[1:]}{'.exe' if os.name == 'nt' else ''}"
    cmd = [clang, opt_flag, "-Wno-override-module", "-DTZ_USE_NET", str(shim_file), str(sys_file), "-x", "ir", str(ir_file), "-o", str(exe)]
    if os.name == "nt":
        cmd.append("-lws2_32")
    else:
        cmd.extend(["-lm", "-lpthread"])
    cc = run_cmd(cmd, cwd=workdir)
    assert cc.returncode == 0, f"audit compile failed ({opt_flag}): {cc.stderr}"
    run_res = run_cmd([str(exe)], cwd=workdir)
    assert run_res.returncode == 0, f"audit run failed ({opt_flag}): {run_res.stdout!r} {run_res.stderr!r}"
    total_allocs, live_allocs = map(int, run_res.stdout.strip().splitlines()[-1].split())
    return total_allocs, live_allocs


def test_file_raii_and_reverse_close(cli: Path, workdir: Path):
    src = r"""
struct FilePair {
    f1: File
    f2: File
}

fn borrowFile(f: File) -> bool {
    return fileIsOpen(f)
}

fn earlyReturnWrite(p: str) -> Result<int, str> {
    var f = fileOpen(p, "w")?
    let n = fileWrite(mut f, "ret-line\n")?
    return Ok(n)
}

fn maybeFailAfterOpen(p: str, flag: bool) -> Result<int, str> {
    var f = fileOpen(p, "w")?
    let w0 = fileWrite(mut f, "temp")?
    if flag && w0 >= 0 {
        let bad = fileOpen("no_such_dir_xyz/missing_m26b.txt", "r")?
        assert fileIsOpen(bad)
    }
    return Ok(1)
}

fn testReverseLocals(p: str) -> Result<str, str> {
    let rm0 = removeFile(p)
    if true {
        var f1 = fileOpen(p, "a")?
        var f2 = fileOpen(p, "a")?
        let w1 = fileWrite(mut f1, "FIRST,")?
        let w2 = fileWrite(mut f2, "SECOND,")?
        assert w1 + w2 >= 0
    }
    var r = fileOpen(p, "r")?
    let txt = fileReadAll(mut r)?
    fileClose(mut r)
    assert !fileIsOpen(r)
    assert removeFile(p)
    return Ok(txt)
}

fn testReverseStruct(p: str) -> Result<str, str> {
    let rm0 = removeFile(p)
    if true {
        var a = fileOpen(p, "a")?
        var b = fileOpen(p, "a")?
        let w1 = fileWrite(mut a, "FIRST,")?
        let w2 = fileWrite(mut b, "SECOND,")?
        assert w1 + w2 >= 0
        let pair = FilePair { f1: a, f2: b }
        assert borrowFile(pair.f1)
    }
    let out = readFile(p)
    assert removeFile(p)
    return Ok(out)
}

fn testReverseArrayMutations(p: str) -> Result<str, str> {
    let rm0 = removeFile(p)
    if true {
        var a = fileOpen(p, "a")?
        var b = fileOpen(p, "a")?
        let w1 = fileWrite(mut a, "FIRST,")?
        let w2 = fileWrite(mut b, "SECOND,")?
        assert w1 + w2 >= 0
        var arr: [File] = []
        push(mut arr, a)
        push(mut arr, b)
        clear(mut arr)
    }
    let out1 = readFile(p)
    assert removeFile(p)
    if true {
        var x = fileOpen(p, "a")?
        var y = fileOpen(p, "a")?
        let w1 = fileWrite(mut x, "ONE,")?
        let w2 = fileWrite(mut y, "TWO,")?
        assert w1 + w2 >= 0
        var arr: [File] = [x, y]
        let popped = pop(mut arr)
        assert borrowFile(popped)
    }
    let out2 = readFile(p)
    assert removeFile(p)
    var first = fileOpen(p, "a")?
    var second = fileOpen(p, "a")?
    var third = fileOpen(p, "a")?
    assert (fileWrite(mut first, "FIRST,")?) == 6
    assert (fileWrite(mut second, "SECOND,")?) == 7
    assert (fileWrite(mut third, "THIRD,")?) == 6
    var files = [first, second, third]
    truncate(mut files, 4)
    assert len(files) == 3
    files.truncate(1)
    assert len(files) == 1
    assert readFile(p) == "THIRD,SECOND,"
    truncate(mut files, -1)
    assert len(files) == 0
    let out3 = readFile(p)
    assert removeFile(p)
    return Ok(out1 + "|" + out2 + "|" + out3)
}

fn main() {
    assert isErr(fileOpen("bad_mode.txt", "z"))
    assert isErr(fileOpen("missing_file_m26b_nope.txt", "r"))

    let p1 = "m26b_basic.txt"
    let rm0 = removeFile(p1)
    match earlyReturnWrite(p1) {
        Ok(n) => assert n == 9
        Err(err) => assert false
    }
    assert removeFile(p1)

    if true {
        var fFallback = fileOpen(p1, "w")
        match fFallback {
            Ok(fv) => {
                var fOk = fv
                assert fileIsOpen(fOk)
            }
            Err(err) => assert false
        }
    }
    assert removeFile(p1)

    // Lines + CRLF + empty line + EOF + read/write after close Err
    match fileOpen(p1, "w") {
        Ok(fInit) => {
            var wf = fInit
            assert borrowFile(wf)
            match fileWrite(mut wf, "alpha\r\n\nbeta") {
                Ok(n) => assert n == 12
                Err(err) => assert false
            }
            match fileFlush(mut wf) {
                Ok(ok) => assert ok
                Err(err) => assert false
            }
            fileClose(mut wf)
            assert !fileIsOpen(wf)
            assert isErr(fileRead(mut wf, 4))
            assert isErr(fileWrite(mut wf, "x"))
            assert isErr(fileFlush(mut wf))
        }
        Err(err) => assert false
    }

    match fileOpen(p1, "r") {
        Ok(rInit) => {
            var rf = rInit
            match fileReadLine(mut rf) {
                Ok(o1) => match o1 {
                    Some(l1) => assert l1 == "alpha"
                    None => assert false
                }
                Err(err) => assert false
            }
            match fileReadLine(mut rf) {
                Ok(o2) => match o2 {
                    Some(l2) => assert l2 == ""
                    None => assert false
                }
                Err(err) => assert false
            }
            match fileReadLine(mut rf) {
                Ok(o3) => match o3 {
                    Some(l3) => assert l3 == "beta"
                    None => assert false
                }
                Err(err) => assert false
            }
            match fileReadLine(mut rf) {
                Ok(o4) => match o4 {
                    None => {}
                    Some(s) => assert false
                }
                Err(err) => assert false
            }
        }
        Err(err) => assert false
    }
    assert removeFile(p1)

    // fileRead chunks + EOF + r+
    match fileOpen(p1, "w") {
        Ok(fInit) => {
            var wf = fInit
            let w0 = fileWrite(mut wf, "abcdef")
            assert isOk(w0)
        }
        Err(err) => assert false
    }
    match fileOpen(p1, "r+") {
        Ok(rInit) => {
            var rf = rInit
            match fileRead(mut rf, 2) {
                Ok(s) => assert s == "ab"
                Err(err) => assert false
            }
            match fileRead(mut rf, 0) {
                Ok(s) => assert s == ""
                Err(err) => assert false
            }
            match fileRead(mut rf, 10) {
                Ok(s) => assert s == "cdef"
                Err(err) => assert false
            }
            match fileRead(mut rf, 10) {
                Ok(s) => assert s == ""
                Err(err) => assert false
            }
        }
        Err(err) => assert false
    }
    assert removeFile(p1)

    // Unwind via '?' + break + labeled break/continue
    assert isErr(maybeFailAfterOpen(p1, true))
    assert removeFile(p1)

    var loopDone = 0
    for idx in 0..3 {
        match fileOpen(p1, "w") {
            Ok(fi) => {
                var f = fi
                let w0 = fileWrite(mut f, "loop")
                assert isOk(w0)
                if idx == 1 {
                    break
                }
                if idx == 0 {
                    continue
                }
            }
            Err(err) => assert false
        }
        loopDone += 1
    }
    assert loopDone == 0
    assert removeFile(p1)

    'outer: for i in 0..2 {
        match fileOpen(p1, "w") {
            Ok(fi) => {
                var f = fi
                let w0 = fileWrite(mut f, "outer")
                assert isOk(w0)
                if i == 0 {
                    for j in 0..2 {
                        match fileOpen("m26b_inner.txt", "w") {
                            Ok(fi2) => {
                                var f2 = fi2
                                let w2 = fileWrite(mut f2, "in")
                                assert isOk(w2) && j == 0
                                break 'outer
                            }
                            Err(err) => assert false
                        }
                    }
                }
            }
            Err(err) => assert false
        }
    }
    assert removeFile("m26b_inner.txt")
    assert removeFile(p1)

    match testReverseLocals("m26b_rev_loc.txt") {
        Ok(s) => print(s)
        Err(err) => assert false
    }
    match testReverseStruct("m26b_rev_st.txt") {
        Ok(s) => print(s)
        Err(err) => assert false
    }
    match testReverseArrayMutations("m26b_rev_arr.txt") {
        Ok(s) => print(s)
        Err(err) => assert false
    }
}
"""
    out, _ = run_both(cli, src, workdir)
    lines = out.strip().splitlines()
    assert lines == ["SECOND,FIRST,", "SECOND,FIRST,", "SECOND,FIRST,|TWO,ONE,|THIRD,SECOND,FIRST,"], lines

    # Negative count trap R_ARRAY_SIZE parity
    trap_src = """
fn runTrap() -> Result<str, str> {
    var f = fileOpen("m26b_trap.txt", "w")?
    let s = fileRead(mut f, -1)?
    return Ok(s)
}

fn main() {
    let r = runTrap()
    assert isErr(r)
}
"""
    run_both(cli, trap_src, workdir, expect_trap=True)


def test_net_and_mutex_raii(cli: Path, workdir: Path):
    src = r"""
struct LockWrap {
    g: Lock
}

struct Owned<T> {
    item: T
}

fn lockAndTryReturn(m: mut Mutex) -> int {
    let g = mutexLock(mut m)
    assert lockIsHeld(g)
    if isSome(mutexTryLock(mut m)) {
        return -1
    }
    return 42
}

fn lockAndErrReturn(m: mut Mutex) -> Result<int, str> {
    let g0 = mutexLock(mut m)
    assert lockIsHeld(g0)
    let bad = fileOpen("nope_dir_x/missing.txt", "r")?
    assert fileIsOpen(bad)
    return Ok(0)
}

fn tryTcpRoundtrip() -> Result<str, str> {
    var listener = tcpListen("127.0.0.1", 0)?
    assert listenerIsOpen(listener)
    let port = tcpLocalPort(listener)
    assert port > 0
    var server = tcpConnect("127.0.0.1", port)?
    var accepted = tcpAccept(mut listener)?
    assert socketIsOpen(server) && socketIsOpen(accepted)
    if true {
        var peer = server
        let wn = socketWrite(mut peer, "ping-m26b")?
        assert wn == 9
        let got = socketRead(mut accepted, 9)?
        assert got == "ping-m26b"
    }
    let eofStr = socketRead(mut accepted, 16)?
    assert eofStr == ""
    socketClose(mut accepted)
    assert !socketIsOpen(accepted)
    listenerClose(mut listener)
    assert !listenerIsOpen(listener)
    return Ok("net-ok")
}

fn peerCloseViaTry(port: int) -> Result<int, str> {
    var cli = tcpConnect("127.0.0.1", port)?
    let w0 = socketWrite(mut cli, "hi")?
    let bad = fileOpen("nope_dir_x/missing2.txt", "r")?
    assert w0 >= 0 && fileIsOpen(bad)
    return Ok(1)
}

fn testNetPeerCloseOnTry() -> Result<str, str> {
    var listener = tcpListen("127.0.0.1", 0)?
    let port = tcpLocalPort(listener)
    assert isErr(peerCloseViaTry(port))
    var acc = tcpAccept(mut listener)?
    let s1 = socketRead(mut acc, 2)?
    let s2 = socketRead(mut acc, 8)?
    return Ok(s1 + ":" + s2)
}

fn main() {
    var m = mutexNew()
    var m2 = mutexNew()
    if true {
        let g = mutexLock(mut m)
        assert lockIsHeld(g)
        assert isNone(mutexTryLock(mut m))
    }
    assert lockAndTryReturn(mut m) == 42
    assert isErr(lockAndErrReturn(mut m))

    if true {
        var g = mutexLock(mut m)
        assert isNone(mutexTryLock(mut m))
        unlock(mut g)
        assert !lockIsHeld(g)
        match mutexTryLock(mut m) {
            Some(g2) => assert lockIsHeld(g2)
            None => assert false
        }
    }

    if true {
        var g = mutexLock(mut m)
        assert isNone(mutexTryLock(mut m))
        g = mutexLock(mut m2)
        assert isSome(mutexTryLock(mut m))
        assert isNone(mutexTryLock(mut m2))
    }
    assert isSome(mutexTryLock(mut m2))

    for i in 0..3 {
        let g0 = mutexLock(mut m)
        assert lockIsHeld(g0)
        if i == 0 {
            continue
        }
        break
    }
    assert isSome(mutexTryLock(mut m))

    if true {
        let g1 = mutexLock(mut m)
        let w0 = LockWrap { g: g1 }
        assert lockIsHeld(w0.g)
        assert isNone(mutexTryLock(mut m))
    }
    assert isSome(mutexTryLock(mut m))

    if true {
        let g1 = mutexLock(mut m)
        let o = Owned<Lock> { item: g1 }
        assert lockIsHeld(o.item)
        assert isNone(mutexTryLock(mut m))
    }
    assert isSome(mutexTryLock(mut m))

    if true {
        var locks: [Lock] = []
        push(mut locks, mutexLock(mut m))
        push(mut locks, mutexLock(mut m2))
        clear(mut locks)
        assert isSome(mutexTryLock(mut m))
        assert isSome(mutexTryLock(mut m2))
    }

    // Lock outlives Mutex
    var movedLock = mutexLock(mut m2)
    if true {
        var tempM = mutexNew()
        let tl = mutexLock(mut tempM)
        movedLock = tl
    }
    assert lockIsHeld(movedLock)
    unlock(mut movedLock)

    // var re-init after move
    let fPath = "m26b_reinit.txt"
    let rm0 = removeFile(fPath)
    let fVar = fileOpen(fPath, "w")
    match fVar {
        Ok(fh) => {
            var x = fh
            let y = x
            x = y
            assert fileIsOpen(x)
        }
        Err(err) => assert false
    }
    assert removeFile(fPath)

    match tryTcpRoundtrip() {
        Ok(s) => print(s)
        Err(err) => assert false
    }
    match testNetPeerCloseOnTry() {
        Ok(s) => print(s)
        Err(err) => assert false
    }
}
"""
    out, _ = run_both(cli, src, workdir)
    lines = out.strip().splitlines()
    assert lines == ["net-ok", "hi:"], lines


def test_native_audit_and_tooling(cli: Path, workdir: Path):
    clang = find_clang()
    audit_src = r"""
struct BoxLock {
    g: Lock
    f: File
}

fn tryUnwind(m: mut Mutex, p: str) -> Result<int, str> {
    let g0 = mutexLock(mut m)
    var f = fileOpen(p, "w")?
    let w0 = fileWrite(mut f, "abc")?
    let bad = fileOpen("no_dir_x/missing.txt", "r")?
    assert lockIsHeld(g0) && w0 >= 0 && fileIsOpen(bad)
    return Ok(1)
}

fn main() {
    let p = "m26b_audit.txt"
    let rm0 = removeFile(p)
    var m = mutexNew()
    let u = tryUnwind(mut m, p)
    assert isErr(u)
    if true {
        let g = mutexLock(mut m)
        match fileOpen(p, "r") {
            Ok(f) => {
                let bl = BoxLock { g: g, f: f }
                var arr: [Lock] = []
                var m2 = mutexNew()
                push(mut arr, mutexLock(mut m2))
                assert lockIsHeld(bl.g)
            }
            Err(err) => assert false
        }
    }
    match fileOpen(p, "r") {
        Ok(rfInit) => {
            var rf = rfInit
            match fileReadAll(mut rf) {
                Ok(s) => assert s == "abc"
                Err(err) => assert false
            }
            match fileOpen(p, "r") {
                Ok(rf2Init) => {
                    var rf2 = rf2Init
                    match fileReadLine(mut rf2) {
                        Ok(o) => match o {
                            Some(line) => assert line == "abc"
                            None => assert false
                        }
                        Err(err) => assert false
                    }
                }
                Err(err) => assert false
            }
        }
        Err(err) => assert false
    }
    assert removeFile(p)

    match tcpListen("127.0.0.1", 0) {
        Ok(lInit) => {
            var l = lInit
            let port = tcpLocalPort(l)
            match tcpConnect("127.0.0.1", port) {
                Ok(cInit) => {
                    var c = cInit
                    match tcpAccept(mut l) {
                        Ok(sInit) => {
                            var s = sInit
                            let wn = socketWrite(mut c, "ok")
                            let rd = socketRead(mut s, 8)
                            assert isOk(wn) && isOk(rd)
                        }
                        Err(err) => assert false
                    }
                }
                Err(err) => assert false
            }
        }
        Err(err) => assert false
    }
}
"""
    for opt in ["-O0", "-O2"]:
        tot, live = compile_and_audit_resources(cli, clang, audit_src, workdir, "res_audit", opt)
        assert tot > 0 and live == 0, f"leak at {opt}: total={tot} live={live}"

    cost_src = workdir / "costes_res.turi"
    cost_src.write_text(
        'fn touch(p: str) -> Result<str, str> {\n'
        '    var f = fileOpen(p, "r")?\n'
        '    let s = fileReadAll(mut f)?\n'
        '    return Ok(s)\n'
        '}\n'
        'fn lockIt() {\n'
        '    var m = mutexNew()\n'
        '    let g = mutexLock(mut m)\n'
        '    assert lockIsHeld(g)\n'
        '}\n'
        'fn listenIt() -> Result<int, str> {\n'
        '    let l = tcpListen("127.0.0.1", 0)?\n'
        '    return Ok(tcpLocalPort(l))\n'
        '}\n'
        'fn main() {}\n',
        encoding="utf-8",
    )
    c_res = run_cmd([str(cli), "costes", str(cost_src)], cwd=workdir)
    assert c_res.returncode == 0, c_res.stderr
    sites = [json.loads(line) for line in c_res.stdout.strip().splitlines()]
    res_sites = [s for s in sites if s["kind"] == "resource" and s["level"] == 3]
    assert len(res_sites) >= 4, sites

    expect_diag(cli, cost_src.read_text(encoding="utf-8"), workdir, "E_MEMORY_LEVEL", ["--max-level", "2"])

    sig_res = run_cmd([str(cli), "sig", str(cost_src)], cwd=workdir)
    assert sig_res.returncode == 0, sig_res.stderr
    sigs = {row["name"]: row for row in (json.loads(ln) for ln in sig_res.stdout.strip().splitlines()) if "io" in row}
    assert sigs["touch"]["io"] is True and sigs["touch"]["mayTrap"] is True, sigs["touch"]
    assert sigs["listenIt"]["io"] is True, sigs["listenIt"]
    assert sigs["lockIt"]["io"] is False, sigs["lockIt"]


def test_affine_and_resource_diagnostics(cli: Path, workdir: Path):
    use_after_move_cases = [
        # 1. after let
        'fn main() {\n    let m = mutexNew()\n    let m2 = m\n    let m3 = m\n}\n',
        # 2. after push
        'fn main() {\n    var m = mutexNew()\n    var a: [Mutex] = []\n    push(mut a, m)\n    let g = mutexLock(mut m)\n}\n',
        # 3. after match on Result<File, str>
        'fn main() {\n    let r = fileOpen("x.txt", "r")\n    match r {\n        Ok(f) => {}\n        Err(err) => {}\n    }\n    let ok = isOk(r)\n}\n',
        # 4. after '?' on owned local Result<Lock, str>
        'fn mkRes() -> Result<Lock, str> { return Err("no") }\n'
        'fn helper() -> Result<int, str> {\n    let r = mkRes()\n    let x = r?\n    let ok = isOk(r)\n    return Ok(1)\n}\nfn main() {}\n',
        # 5. loop back-edge move
        'fn main() {\n    let m = mutexNew()\n    var i = 0\n    while i < 2 {\n        let m2 = m\n        i += 1\n    }\n}\n',
        # 6. across one branch of if
        'fn f(b: bool) {\n    let m = mutexNew()\n    if b {\n        let m2 = m\n    }\n    let m3 = m\n}\nfn main() {}\n',
    ]
    for case in use_after_move_cases:
        d = expect_diag(cli, case, workdir, "E_USE_AFTER_MOVE")
        assert d.get("related"), d

    resource_copy_cases = [
        # 1. borrowed param move
        'fn bad(m: Mutex) {\n    let m2 = m\n}\nfn main() {}\n',
        # 2. partial struct field move
        'struct S { m: Mutex }\nfn main() {\n    let s = S { m: mutexNew() }\n    let m2 = s.m\n}\n',
        # 3. a[i] on [Mutex]
        'fn main() {\n    var a: [Mutex] = []\n    push(mut a, mutexNew())\n    let x = a[0]\n}\n',
        # 4. a[lo..hi] slice on [Mutex]
        'fn main() {\n    var a: [Mutex] = []\n    push(mut a, mutexNew())\n    let s = a[0..1]\n}\n',
        # 5. repeat(r, n)
        'fn main() {\n    let m = mutexNew()\n    let a = repeat(m, 2)\n}\n',
        # 6. for x in arr
        'fn main() {\n    var a: [Mutex] = []\n    push(mut a, mutexNew())\n    for x in a { print(len(a)) }\n}\n',
        # 7. Map<int, File>
        'fn main() {\n    let m: Map<int, File> = mapNew()\n}\n',
        # 8. Pool<File>
        'fn main() {\n    let p: Pool<File> = poolNew()\n}\n',
        # 9. Shared<File>
        'fn f(f: File) {\n    let s = sharedNew(f)\n}\nfn main() {}\n',
        # 10. dyn boxing of resource struct
        'interface Ping { fn ping(self) -> int }\nstruct Boxed { m: Mutex }\nimpl Ping for Boxed { fn ping(self) -> int { return 1 } }\nfn take(x: dyn Ping) -> int { return x.ping() }\nfn main() {\n    let r = take(Boxed { m: mutexNew() })\n}\n',
        # 11. closure capture of File
        'fn f(x: File) {\n    let c = || fileIsOpen(x)\n    let b = c()\n}\nfn main() {}\n',
    ]
    for case in resource_copy_cases:
        expect_diag(cli, case, workdir, "E_RESOURCE_COPY")

    for bad_name, good_name in [
        ("FileStream", "File"),
        ("TcpStream", "Socket"),
        ("MutexGuard", "Lock"),
        ("BufReader", "File"),
        ("LockGuard", "Lock"),
    ]:
        bad_src = f"fn f(x: {bad_name}) {{}}\nfn main() {{}}\n"
        d = expect_diag(cli, bad_src, workdir, "E_RESOURCE_NAME")
        assert d.get("fix") and d["fix"]["replacement"] == good_name, d

    fix_file = workdir / "fix_res.turi"
    orig_fix = "fn f(x: FileStream) {}\nfn main() {}\n"
    fix_file.write_text(orig_fix, encoding="utf-8", newline="\n")
    d_fix = expect_diag(cli, orig_fix, workdir, "E_RESOURCE_NAME")
    sp = d_fix["fix"]["span"]
    patched = orig_fix[:sp["start"]] + d_fix["fix"]["replacement"] + orig_fix[sp["end"]:]
    assert patched == "fn f(x: File) {}\nfn main() {}\n", patched
    fix_file.write_text(patched, encoding="utf-8", newline="\n")
    chk = run_cmd([str(cli), "check", str(fix_file)], cwd=workdir)
    assert chk.returncode == 0, chk.stderr

    for topic in ["E_USE_AFTER_MOVE", "E_RESOURCE_COPY", "E_RESOURCE_NAME"]:
        exp = run_cmd([str(cli), "explain", topic], cwd=workdir)
        assert exp.returncode == 0 and exp.stdout.strip(), (topic, exp)


def main():
    cli = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="turi-m26b-") as tmp:
        workdir = Path(tmp)
        test_file_raii_and_reverse_close(cli, workdir)
        test_net_and_mutex_raii(cli, workdir)
        test_native_audit_and_tooling(cli, workdir)
        test_affine_and_resource_diagnostics(cli, workdir)
    print("turi03_m26b: ok")


if __name__ == "__main__":
    main()
