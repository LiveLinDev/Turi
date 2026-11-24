"""Pruebas funcionales, de memoria, paquetes, ordenación y diferenciales del Hito 2.4 de Turi 0.3."""
import json
import os
from pathlib import Path
import random
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


def test_map_and_set_parity(cli: Path, workdir: Path):
    src = """
struct Empty {
}

struct Point {
    x: int
    y: int
}

struct Trie {
    end: bool
    kids: Map<str, Trie>
}

enum Color {
    Red = 10
    Green = 20
    Blue = 30
}

fn fillMapMut(m: mut Map<str, int>, prefix: str, count: int) {
    for i in 0..count {
        mapSet(mut m, "{prefix}-{i}", i * 3 + 1)
    }
}

fn addTrieWord(t: mut Trie, words: [str], idx: int) {
    if idx >= len(words) {
        return
    }
    let ch = words[idx]
    var sub = unwrapOr(mapGet(t.kids, ch), Trie { end: false, kids: mapNew() })
    if idx + 1 == len(words) {
        sub = Trie { end: true, kids: sub.kids }
    } else {
        addTrieWord(mut sub, words, idx + 1)
    }
    var ks = t.kids
    mapSet(mut ks, ch, sub)
    t.kids = ks
}

fn hasTrieWord(t: Trie, words: [str], idx: int) -> bool {
    if idx >= len(words) {
        return t.end
    }
    let next = mapGet(t.kids, words[idx])
    match next {
        Some(child) => { return hasTrieWord(child, words, idx + 1) }
        None => { return false }
    }
}

fn insertChecked(m: Map<str, [str]>, k: str) -> str? {
    let arr = mapGet(m, k)?
    return Some(arr[0])
}

fn main() {
    var mi: Map<int, int> = mapNew()
    print(len(mi))
    print(cap(mi))
    reserve(mut mi, 20)
    print(cap(mi))
    for i in 0..25 {
        mapSet(mut mi, i * 17 - 10, i + 100)
    }
    print(len(mi))
    print(cap(mi))
    print(unwrapOr(mapGet(mi, 17 * 5 - 10), -1))
    print(mapHas(mi, 999))
    print(mapDel(mut mi, 17 * 5 - 10))
    print(mapDel(mut mi, 17 * 5 - 10))
    print(mapHas(mi, 17 * 5 - 10))
    print(len(mi))
    let ks = mapKeys(mi)
    let vs = mapValues(mi)
    print(len(ks))
    print(len(vs))
    for k in ks {
        write("{k},")
    }
    write("\\n")
    for v in vs {
        write("{v},")
    }
    write("\\n")

    var ms: Map<str, [str]> = mapWithCap(8)
    print(cap(ms))
    mapSet(mut ms, "short", ["a", "long-string-element-here"])
    mapSet(mut ms, "long-heap-key-exceeding-12-bytes", ["x1", "x2"])
    mapSet(mut ms, "café-perú-key", ["lima"])
    mapSet(mut ms, "short", ["replaced-short-elem", "second-long-payload-string"])
    let mCopy = ms
    mapSet(mut ms, "added-later", ["temp"])
    assert len(mCopy) == 3
    assert len(ms) == 4
    print(unwrapOr(insertChecked(ms, "short"), "none"))
    print(unwrapOr(insertChecked(ms, "missing"), "none"))
    print(mapDel(mut ms, "long-heap-key-exceeding-12-bytes"))
    print(len(ms))

    var mb: Map<bool, str> = mapNew()
    mapSet(mut mb, true, "verdad-larga-12345")
    mapSet(mut mb, false, "falso")
    print(len(mb))
    print(unwrapOr(mapGet(mb, true), "?"))
    print(unwrapOr(mapGet(mb, false), "?"))
    print(mapDel(mut mb, false))
    print(isNone(mapGet(mb, false)))

    var me: Map<Color, [int]> = mapNew()
    mapSet(mut me, Color.Red, [1, 2])
    mapSet(mut me, Color.Blue, [3, 4, 5])
    print(len(me))
    for ec in mapKeys(me) {
        print(int(ec))
    }

    var mp: Map<str, Point> = mapNew()
    mapSet(mut mp, "origin", Point { x: 7, y: 9 })
    let pt = unwrapOr(mapGet(mp, "origin"), Point { x: 0, y: 0 })
    print(pt.x + pt.y)

    var mo: Map<str, int?> = mapNew()
    mapSet(mut mo, "someKey", Some(42))
    mapSet(mut mo, "noneKey", None)
    print(isSome(unwrapOr(mapGet(mo, "someKey"), None)))
    print(isNone(unwrapOr(mapGet(mo, "noneKey"), Some(1))))

    var mr: Map<str, Result<int, str>> = mapNew()
    mapSet(mut mr, "ok", Ok(88))
    mapSet(mut mr, "err", Err("fallo-largo-owned"))
    print(isOk(unwrapOr(mapGet(mr, "ok"), Err("x"))))
    print(isErr(unwrapOr(mapGet(mr, "err"), Ok(0))))

    var mm: Map<str, Map<int, str>> = mapNew()
    var inner: Map<int, str> = mapNew()
    mapSet(mut inner, 10, "diez-cadena-larga")
    mapSet(mut mm, "outer", inner)
    let gotInner = unwrapOr(mapGet(mm, "outer"), mapNew())
    print(unwrapOr(mapGet(gotInner, 10), "miss"))

    var mz: Map<str, Empty> = mapNew()
    mapSet(mut mz, "unit", Empty {})
    print(isSome(mapGet(mz, "unit")))
    print(isNone(mapGet(mz, "absent")))
    print(len(mapValues(mz)))

    var root = Trie { end: false, kids: mapNew() }
    addTrieWord(mut root, ["h", "o", "l", "a"], 0)
    addTrieWord(mut root, ["h", "o", "j", "a"], 0)
    assert hasTrieWord(root, ["h", "o", "l", "a"], 0)
    assert hasTrieWord(root, ["h", "o", "j", "a"], 0)
    assert !hasTrieWord(root, ["h", "o", "l"], 0)
    print(hasTrieWord(root, ["h", "o", "l", "a"], 0))
    print(hasTrieWord(root, ["h", "o", "j", "a"], 0))
    print(hasTrieWord(root, ["h", "o", "l"], 0))

    // Churn deletes and inserts: same-size rehash keeps capacity bounded.
    var churn: Map<str, int> = mapNew()
    for step in 0..500 {
        mapSet(mut churn, "long-churn-key-{step}", step)
        if step >= 50 {
            let removed = mapDel(mut churn, "long-churn-key-{step - 50}")
            assert removed
        }
    }
    assert len(churn) == 50
    assert cap(churn) <= 112
    print(len(churn))
    print(cap(churn))
    let capBeforeClear = cap(churn)
    clear(mut churn)
    assert len(churn) == 0 && cap(churn) == capBeforeClear
    print(len(churn))
    print(cap(churn) == capBeforeClear)
    fillMapMut(mut churn, "post", 5)
    print(len(churn))

    // Set<T> parity
    var si: Set<int> = setWithCap(12)
    print(cap(si))
    print(setAdd(mut si, 10))
    print(setAdd(mut si, 20))
    print(setAdd(mut si, 10))
    print(setHas(si, 20))
    print(setDel(mut si, 20))
    print(setDel(mut si, 20))
    for x in setItems(si) {
        print(x)
    }

    var ss: Set<str> = setNew()
    assert setAdd(mut ss, "uno")
    assert setAdd(mut ss, "cadena-larga-para-set-utf8")
    assert !setAdd(mut ss, "cadena-larga-para-set-utf8")
    let ssCopy = ss
    assert setDel(mut ss, "uno")
    assert len(ss) == 1
    assert len(ssCopy) == 2

    var sb: Set<bool> = setNew()
    assert setAdd(mut sb, false)
    assert setAdd(mut sb, true)
    assert !setAdd(mut sb, false)
    for b in setItems(sb) {
        print(b)
    }

    var se: Set<Color> = setNew()
    assert setAdd(mut se, Color.Green)
    assert setAdd(mut se, Color.Red)
    assert !setAdd(mut se, Color.Green)
    print(len(se))
}

test "map y ? en test" {
    var m: Map<str, int> = mapNew()
    mapSet(mut m, "k", 7)
    assert mapGet(m, "k")? == 7
}
"""
    out, _ = run_both(cli, src, workdir)
    lines = out.strip().splitlines()
    assert lines[0] == "0" and lines[1] == "0" and lines[2] == "28", lines[:5]
    chk = run_cmd([str(cli), "check", str(workdir / "main.turi")])
    assert chk.returncode == 0, chk.stderr


def test_secure_and_generics_and_shadow(cli: Path, workdir: Path):
    src_sec = """
fn main() {
    var m: Map<str, int> = mapSecure()
    reserve(mut m, 32)
    for i in 0..40 {
        mapSet(mut m, "sec-key-{i}", i * 2)
    }
    var m2 = m
    assert mapDel(mut m, "sec-key-0")
    assert !mapHas(m, "sec-key-0")
    assert mapHas(m2, "sec-key-0")
    var sum = 0
    for k in mapKeys(m2) {
        sum += unwrapOr(mapGet(m2, k), 0)
    }
    print(len(m))
    print(len(m2))
    print(sum)

    var s: Set<int> = setSecure()
    for i in 0..30 {
        assert setAdd(mut s, i)
    }
    assert setHas(s, 15)
    assert setDel(mut s, 15)
    assert len(s) == 29
    clear(mut s)
    assert len(s) == 0
    print(len(s))
}
"""
    out, _ = run_both(cli, src_sec, workdir)
    assert out.strip().splitlines() == ["39", "40", "1560", "0"], out

    src_gen = """
struct Bag<T> {
    m: Map<str, T>
    seen: Set<int>
}

fn countUnique<T>(items: [T]) -> int {
    var s: Set<T> = setNew()
    for it in items {
        let ignored = setAdd(mut s, it)
    }
    return len(s)
}

fn sort(x: int) -> int {
    return x + 100
}

fn mapGet(k: str) -> str {
    return "shadow:" + k
}

fn main() {
    print(countUnique([1, 2, 2, 3, 1]))
    print(countUnique(["a", "larga-cadena-unicos", "a", "b"]))
    let b = Bag<int> { m: mapWithCap(4), seen: setNew() }
    print(len(b.m))
    print(cap(b.m))
    print(sort(5))
    print(mapGet("ok"))
}
"""
    out, _ = run_both(cli, src_gen, workdir)
    assert out.strip().splitlines() == ["3", "3", "0", "14", "105", "shadow:ok"], out


def test_sorting_parity(cli: Path, workdir: Path):
    rng = random.Random(20261121)
    rand_ints = [rng.randint(-50000, 50000) for _ in range(250)]
    rand_strs = []
    for k in range(120):
        choice = k % 5
        if choice == 0:
            rand_strs.append(f"s{k % 11}")
        elif choice == 1:
            rand_strs.append(f"long-owned-string-sample-{k % 17}-{rng.randint(10, 99)}")
        elif choice == 2:
            rand_strs.append("")
        elif choice == 3:
            rand_strs.append("perú-" + ("á" if k % 2 == 0 else "z"))
        else:
            rand_strs.append(f"prefix-common-{k:04d}")
    rng.shuffle(rand_strs)

    ints_lit = ", ".join(str(x) for x in rand_ints)
    strs_lit = ", ".join(json.dumps(s, ensure_ascii=False) for s in rand_strs)

    src = f"""
fn sortInPlace(a: mut [int]) {{
    sort(mut a)
}}

fn main() {{
    var empty: [int] = []
    sort(mut empty)
    sortStable(mut empty)
    print(len(empty))

    var one = [42]
    sort(mut one)
    sortStable(mut one)
    print(one[0])

    var rev = [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]
    sort(mut rev)
    for x in rev {{
        write("{{x}},")
    }}
    write("\\n")

    var eqArr = repeat(5, 80)
    sort(mut eqArr)
    sortStable(mut eqArr)
    assert eqArr[0] == 5 && eqArr[79] == 5

    var extremes = [9223372036854775807, -9223372036854775808, 0, -1, 1]
    sortStable(mut extremes)
    for x in extremes {{
        write("{{x}};")
    }}
    write("\\n")

    var rints = [{ints_lit}]
    let orig = rints
    var rintsStable = rints
    sortInPlace(mut rints)
    sortStable(mut rintsStable)
    assert len(rints) == len(orig)
    assert orig[0] == {rand_ints[0]}
    for i in 0..len(rints) {{
        assert rints[i] == rintsStable[i]
        if i > 0 {{
            assert rints[i - 1] <= rints[i]
        }}
    }}
    print(rints[0])
    print(rints[len(rints) - 1])

    // Total order on float: -Inf < -1.5 < -0.0 < +0.0 < 1.5 < +Inf < NaN
    let nanV = -(0.0 / 0.0)
    let negInf = -1.0 / 0.0
    let posInf = 1.0 / 0.0
    var flts = [1.5, posInf, 0.0, -0.0, -1.5, negInf]
    var flts2 = flts
    sort(mut flts)
    sortStable(mut flts2)
    for i in 0..len(flts) {{
        assert flts[i] == flts2[i]
    }}
    assert isInf(flts[0]) && flts[0] < 0.0
    assert (1.0 / flts[2]) < 0.0
    assert (1.0 / flts[3]) > 0.0
    assert isInf(flts[5]) && flts[5] > 0.0
    print(flts[1])
    print(flts[4])

    var withNan = [1.0, nanV, -2.0]
    sort(mut withNan)
    var foundNan = false
    for v in withNan {{
        if isNaN(v) {{
            foundNan = true
        }}
    }}
    assert foundNan

    var rstrs = [{strs_lit}]
    var rstrs2 = rstrs
    sort(mut rstrs)
    sortStable(mut rstrs2)
    for i in 0..len(rstrs) {{
        assert rstrs[i] == rstrs2[i]
        if i > 0 {{
            assert rstrs[i - 1] <= rstrs[i]
        }}
    }}
    print(rstrs[0])
    print(rstrs[len(rstrs) - 1])

    var bools = [true, false, true, false, false, true, true, false]
    var bools2 = bools
    sort(mut bools)
    sortStable(mut bools2)
    for i in 0..len(bools) {{
        assert bools[i] == bools2[i]
        write("{{bools[i]}},")
    }}
    write("\\n")

    // Region-allocated repeat sorted in place.
    var reg = repeat(0, 60)
    for i in 0..60 {{
        reg[i] = 60 - i
    }}
    sort(mut reg)
    assert reg[0] == 1 && reg[59] == 60
    print(reg[0] + reg[59])
}}
"""
    run_both(cli, src, workdir)


def test_traps_and_package(cli: Path, workdir: Path):
    for bad in [
        "fn main() { let m: Map<str, int> = mapWithCap(-1) }",
        "fn main() { let s: Set<int> = setWithCap(-5) }",
        "fn main() { var m: Map<int, int> = mapNew()\nreserve(mut m, -2) }",
    ]:
        trap_id, _ = run_both(cli, bad, workdir, expect_trap=True)
        assert trap_id == "R_ARRAY_SIZE", trap_id

    for big in [
        "fn main() { let m: Map<str, int> = mapWithCap(20000000) }",
        "fn main() { let s: Set<int> = setWithCap(20000000) }",
        "fn main() { var m: Map<int, int> = mapNew()\nreserve(mut m, 20000000) }",
    ]:
        trap_id, _ = run_both(cli, big, workdir, expect_trap=True)
        assert trap_id == "R_ALLOC", trap_id

    pkg_dir = workdir / "pkg_m24"
    (pkg_dir / "src").mkdir(parents=True)
    (pkg_dir / "veloz.pkg").write_text("package m24app\nversion 1.0.0\n", encoding="utf-8")
    (pkg_dir / "src/repo.turi").write_text("""
enum Kind {
    Alpha
    Beta
}

struct Item {
    label: str
    tags: Set<Kind>
}

fn makeItem(name: str, k: Kind) -> Item {
    var s: Set<Kind> = setNew()
    let added = setAdd(mut s, k)
    return Item { label: name, tags: s }
}
""".lstrip(), encoding="utf-8")
    (pkg_dir / "src/main.turi").write_text("""
import repo

fn main() {
    var reg: Map<str, repo.Item> = mapNew()
    mapSet(mut reg, "one", repo.makeItem("alpha-long-label", repo.Kind.Alpha))
    let got = unwrapOr(mapGet(reg, "one"), repo.makeItem("none", repo.Kind.Beta))
    print(got.label)
    print(setHas(got.tags, repo.Kind.Alpha))
}
""".lstrip(), encoding="utf-8")
    entry = pkg_dir / "src/main.turi"
    exe = workdir / ("pkg_app.exe" if os.name == "nt" else "pkg_app")
    ir = run_cmd([str(cli), "run", str(entry)])
    assert ir.returncode == 0, ir.stderr
    bd = run_cmd([str(cli), "build", str(entry), "-o", str(exe)])
    assert bd.returncode == 0, bd.stderr
    nr = run_cmd([str(exe)])
    assert nr.returncode == 0 and nr.stdout == ir.stdout, (nr.returncode, nr.stdout, ir.stdout)


def test_diagnostics_and_agent_cli(cli: Path, workdir: Path):
    expect_diag(cli, "struct Point { x: int }\nfn main() { let m: Map<Point, int> = mapNew() }", workdir, "E_KEY_TYPE")
    expect_diag(cli, "fn main() { let s: Set<[int]> = setNew() }", workdir, "E_KEY_TYPE")
    expect_diag(cli, "fn main() { let m: Map<float, int> = mapNew() }", workdir, "E_KEY_TYPE")
    expect_diag(cli, "enum Boxed { Val(n: int) }\nfn main() { let s: Set<Boxed> = setNew() }", workdir, "E_KEY_TYPE")
    expect_diag(
        cli,
        "struct Point { x: int }\nfn countU<T>(xs: [T]) -> int {\n var s: Set<T> = setNew()\n return len(s)\n}\nfn main() { print(countU([Point { x: 1 }])) }",
        workdir,
        "E_KEY_TYPE",
    )
    expect_diag(cli, "fn main() { let m = mapNew() }", workdir, "E_INFER_TYPE")
    expect_diag(cli, "fn main() { let s = setNew() }", workdir, "E_INFER_TYPE")
    expect_diag(cli, "fn main() { let m = mapWithCap(8) }", workdir, "E_INFER_TYPE")
    expect_diag(cli, "fn main() { let s = setSecure() }", workdir, "E_INFER_TYPE")

    bad_idx_read = 'fn main() {\n    var m: Map<str, int> = mapNew()\n    mapSet(mut m, "a", 1)\n    let x = m["a"]\n    print(unwrapOr(x, 0))\n}'
    d_r = expect_diag(cli, bad_idx_read, workdir, "E_MAP_INDEX")
    assert "fix" in d_r
    fixed_read = apply_fix(bad_idx_read, d_r)
    fsrc = workdir / "fixed.turi"
    fsrc.write_text(fixed_read, encoding="utf-8")
    assert run_cmd([str(cli), "check", str(fsrc)]).returncode == 0, fixed_read

    bad_idx_write = 'fn main() {\n    var m: Map<str, int> = mapNew()\n    m["a"] = 5\n    print(unwrapOr(mapGet(m, "a"), 0))\n}'
    d_w = expect_diag(cli, bad_idx_write, workdir, "E_MAP_INDEX")
    assert "fix" in d_w
    fixed_write = apply_fix(bad_idx_write, d_w)
    fsrc.write_text(fixed_write, encoding="utf-8")
    assert run_cmd([str(cli), "check", str(fsrc)]).returncode == 0, fixed_write

    for wrong, src_txt in [
        ("E_MAP_NAME", "fn main() {\n    var m: HashMap<str, int> = mapNew()\n    mapSet(mut m, \"a\", 1)\n}"),
        ("E_MAP_NAME", "fn main() {\n    var m: Dict<str, int> = mapNew()\n    mapSet(mut m, \"a\", 1)\n}"),
        ("E_SET_NAME", "fn main() {\n    var s: HashSet<int> = setNew()\n    let added = setAdd(mut s, 1)\n}"),
    ]:
        d_c = expect_diag(cli, src_txt, workdir, wrong)
        assert "fix" in d_c, d_c
        fixed = apply_fix(src_txt, d_c)
        fsrc.write_text(fixed, encoding="utf-8")
        assert run_cmd([str(cli), "check", str(fsrc)]).returncode == 0, fixed

    expect_diag(cli, 'fn main() { var m: Map<str, int> = mapNew()\nmapSet(m, "a", 1) }', workdir, "E_MUT_REQUIRED")
    expect_diag(cli, "fn main() { var a = [3, 1, 2]\nsort(a) }", workdir, "E_MUT_REQUIRED")
    expect_diag(cli, "fn main() { var a = [3, 1, 2]\nsortStable(a) }", workdir, "E_MUT_REQUIRED")
    expect_diag(cli, "fn main() { var m: Map<str, int> = mapNew()\nprint(m.length) }", workdir, "E_LENGTH")
    expect_diag(cli, "fn main() { var s: Set<int> = setNew()\nprint(s.length) }", workdir, "E_LENGTH")
    expect_diag(cli, "fn main() { var sb = sbNew()\nprint(sb.length) }", workdir, "E_LENGTH")
    expect_diag(cli, "fn main() { let a: Map<int, int> = mapNew()\nlet b: Map<int, int> = mapNew()\nprint(a == b) }", workdir, "E_EQUALITY")
    expect_diag(cli, "fn main() { let a: Set<int> = setNew()\nlet b: Set<int> = setNew()\nprint(a == b) }", workdir, "E_EQUALITY")
    expect_diag(cli, "struct Map { x: int }", workdir, "E_DUP_TYPE")
    expect_diag(cli, "struct Set { x: int }", workdir, "E_DUP_TYPE")
    expect_diag(cli, "fn f(m: Map<int>) {}", workdir, "E_GENERIC")
    expect_diag(cli, "fn f(s: Set<int, str>) {}", workdir, "E_GENERIC")
    expect_diag(cli, "fn f(m: Map) {}", workdir, "E_GENERIC")
    expect_diag(cli, "fn main() { let a: [int] = mapNew() }", workdir, "E_TYPE")
    expect_diag(cli, "struct Point { x: int }\nfn main() { var a = [Point { x: 1 }]\nsort(mut a) }", workdir, "E_TYPE")

    sample = workdir / "meta.turi"
    sample.write_text('fn useCol(m: mut Map<str, int>, a: mut [str]) -> int? {\n    mapSet(mut m, "k", 1)\n    sortStable(mut a)\n    return mapGet(m, "k")\n}\nfn main() {}\n', encoding="utf-8")
    sig_r = run_cmd([str(cli), "sig", str(sample), "useCol"])
    assert sig_r.returncode == 0
    sig_obj = json.loads(sig_r.stdout.strip())
    assert sig_obj["mayTrap"] is True
    assert "Map<str, int>" in sig_obj["signature"]
    types_r = run_cmd([str(cli), "types", str(sample), "useCol"])
    assert types_r.returncode == 0 and "int?" in types_r.stdout
    for topic in ["E_KEY_TYPE", "E_MAP_INDEX", "E_MAP_NAME", "E_SET_NAME"]:
        ex = run_cmd([str(cli), "explain", topic])
        assert ex.returncode == 0 and ex.stdout.strip(), (topic, ex.stderr)


def test_native_leak_and_stability(cli: Path, clang: str, workdir: Path):
    stress_src = """
struct Trie {
    end: bool
    kids: Map<str, Trie>
}

fn maybeGet(m: Map<str, str>, k: str) -> str? {
    let v = mapGet(m, k)?
    return Some(v + "-suffix-long-owned")
}

fn main() {
    var nested: Map<str, [Map<int, str>]> = mapNew()
    for i in 0..40 {
        var arr: [Map<int, str>] = []
        for j in 0..4 {
            var inner: Map<int, str> = mapNew()
            mapSet(mut inner, j, "val-owned-long-{i}-{j}")
            push(mut arr, inner)
        }
        mapSet(mut nested, "outer-long-key-{i}", arr)
    }
    var copyNested = nested
    mapSet(mut copyNested, "outer-long-key-0", [])
    assert mapDel(mut nested, "outer-long-key-1")
    clear(mut copyNested)
    mapSet(mut copyNested, "reused-long-key", [])

    var root = Trie { end: false, kids: mapNew() }
    var leaf = Trie { end: true, kids: mapNew() }
    var rk = root.kids
    mapSet(mut rk, "branch-long-key", leaf)
    root.kids = rk

    var s: Set<str> = setNew()
    for i in 0..50 {
        let ins = setAdd(mut s, "set-long-owned-{i}")
    }
    let rem = setDel(mut s, "set-long-owned-10")

    var mm: Map<str, str> = mapNew()
    mapSet(mut mm, "k1", "value-owned-exceeding-12-bytes")
    let g1 = unwrapOr(maybeGet(mm, "k1"), "fb")
    let g2 = unwrapOr(maybeGet(mm, "miss"), "fb-long-owned-fallback")
    assert len(g1) > 12 && len(g2) > 12

    var strs: [str] = []
    for i in 0..60 {
        push(mut strs, "sort-long-owned-{60 - i}")
    }
    sort(mut strs)
    sortStable(mut strs)
    assert len(strs) == 60
}
"""
    src_file = workdir / "stress.turi"
    src_file.write_text(stress_src, encoding="utf-8")
    ir_p = run_cmd([str(cli), "emit-ir", str(src_file)])
    assert ir_p.returncode == 0, ir_p.stderr
    ir = ir_p.stdout.replace("@malloc(", "@audit_malloc(").replace("@free(", "@audit_free(")
    ir = ir.replace("declare noalias ptr @malloc(i64)", "declare noalias ptr @audit_malloc(i64)")
    ir = ir.replace("declare void @free(ptr)", "declare void @audit_free(ptr)")
    ir_file = workdir / "stress.ll"
    ir_file.write_text(ir, encoding="utf-8")

    # Extract turiCollectionsRuntimeC cleanly by reusing native_collections_rt.hpp.
    rt_hdr = Path(__file__).resolve().parents[1] / "src/native_collections_rt.hpp"
    hdr_txt = rt_hdr.read_text(encoding="utf-8")
    c_start = hdr_txt.index('R"c(\n') + 5
    c_end = hdr_txt.index('\n)c";')
    col_c = hdr_txt[c_start:c_end]
    # Redirect malloc/free AFTER standard headers to track every allocation inside C runtime.
    inject_anchor = "typedef struct tz_gstr"
    audit_macros = (
        "void* audit_malloc(size_t n);\n"
        "void audit_free(void* p);\n"
        "#define malloc audit_malloc\n"
        "#define free audit_free\n"
    )
    col_c_inst = col_c.replace(inject_anchor, audit_macros + inject_anchor, 1)
    col_file = workdir / "col_rt_audit.c"
    col_file.write_text(col_c_inst, encoding="utf-8")

    # Inject a tiny live-count verification wrapper via C destructor / main rename:
    # Wrap IR main into @turi_main and call it from C main so we assert live_allocs == 0 right before process exit.
    ir_wrapped = ir.replace("define i32 @main()", "define i32 @turi_main()")
    ir_file.write_text(ir_wrapped, encoding="utf-8")
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

    stress_exe = workdir / ("stress_audit.exe" if os.name == "nt" else "stress_audit")
    cc = run_cmd([clang, "-O2", "-Wno-override-module", str(shim_file), str(col_file), "-x", "ir", str(ir_file), "-o", str(stress_exe)])
    assert cc.returncode == 0, f"audit compile failed: {cc.stderr}"
    run_res = run_cmd([str(stress_exe)])
    assert run_res.returncode == 0, f"leak detected! stdout={run_res.stdout!r} stderr={run_res.stderr!r}"

    # Verify strict stability of tz_col_sort_stable_str on 200 distinct heap pointers with equal content.
    stab_c = workdir / "stability_test.c"
    stab_c.write_text(col_c + """
#include <assert.h>
#include <stdio.h>

int main(void) {
    enum { N = 200 };
    tz_gstr arr[N];
    uint8_t* ptrs[N];
    uint8_t* groups[5][N];
    int gcnt[5] = {0, 0, 0, 0, 0};

    for (int i = 0; i < N; ++i) {
        int b = (i * 7) % 5;
        char txt[32];
        snprintf(txt, sizeof(txt), "bucket-key-%02d-equal", b);
        uint32_t len = (uint32_t)strlen(txt);
        uint8_t* p = (uint8_t*)malloc(len);
        assert(p != NULL);
        memcpy(p, txt, len);
        ptrs[i] = p;
        uint32_t pref = 0;
        memcpy(&pref, txt, 4);
        arr[i].hdr = len | UINT32_C(0x80000000);
        arr[i].pref = pref;
        arr[i].tail = (uintptr_t)p;
        groups[b][gcnt[b]++] = p;
    }

    uint8_t ok = tz_col_sort_stable_str(arr, N);
    assert(ok == 1);

    int pos = 0;
    for (int b = 0; b < 5; ++b) {
        for (int j = 0; j < gcnt[b]; ++j) {
            assert((uint8_t*)arr[pos].tail == groups[b][j]);
            ++pos;
        }
    }
    for (int i = 0; i < N; ++i) free(ptrs[i]);
    return 0;
}
""", encoding="utf-8")
    stab_exe = workdir / ("stab_test.exe" if os.name == "nt" else "stab_test")
    sc = run_cmd([clang, "-O2", "-Wall", "-Wextra", str(stab_c), "-o", str(stab_exe)])
    assert sc.returncode == 0, f"stability compile/warnings failed:\n{sc.stderr}"
    sr = run_cmd([str(stab_exe)])
    assert sr.returncode == 0, f"stability test failed: {sr.stderr}"


def main():
    cli = Path(sys.argv[1]).resolve()
    clang = find_clang()
    with tempfile.TemporaryDirectory(prefix="turi03_m24_") as tmp:
        wd = Path(tmp)
        test_map_and_set_parity(cli, wd)
        test_secure_and_generics_and_shadow(cli, wd)
        test_sorting_parity(cli, wd)
        test_traps_and_package(cli, wd)
        test_diagnostics_and_agent_cli(cli, wd)
        test_native_leak_and_stability(cli, clang, wd)
    print("Hito 2.4 verificado: Map<K, V> (SwissTable/foldhash/SipHash), Set<T> y sort/sortStable (ipnsort/driftsort).")


if __name__ == "__main__":
    main()
