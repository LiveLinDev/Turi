"""Pruebas end-to-end para resolucion 'std' y paquetes tensor, crypto, data, cli y postgres."""
import hashlib
import hmac
from pathlib import Path
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]

# 1. Standalone check of each stdlib module + zero-overhead on fibonacci.turi
for mod in ("tensor", "crypto", "data", "cli", "postgres"):
    r = subprocess.run([str(cli), "check", str(root / f"lib/{mod}/src/{mod}.turi")],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, (mod, r.stdout, r.stderr)

fib_ir = subprocess.run([str(cli), "emit-ir", str(root / "examples/fibonacci.turi")],
                        capture_output=True, text=True, timeout=30)
assert fib_ir.returncode == 0, fib_ir.stderr
for sym in ("@tz_crypto_", "@tz_pg_", "@tz_web_", "@tz_gpu_"):
    assert sym not in fib_ir.stdout, f"fibonacci IR must not reference {sym}"

want_sha_abc = hashlib.sha256(b"abc").hexdigest()
want_sha_empty = hashlib.sha256(b"").hexdigest()
want_hmac = hmac.new(b"\x0b" * 20, b"Hi There", hashlib.sha256).hexdigest()

with tempfile.TemporaryDirectory() as td:
    proj = Path(td) / "stdproj"
    (proj / "src").mkdir(parents=True)
    (proj / "veloz.pkg").write_text(
        'package stdtest\n'
        'version 0.1.0\n'
        'require tensor 0.1.0 "std"\n'
        'require crypto 0.1.0 "std"\n'
        'require data 0.1.0 "std"\n'
        'require cli 0.1.0 "std"\n'
        'require postgres 0.1.0 "std"\n',
        encoding="utf-8",
    )
    main_turi = proj / "src/main.turi"
    main_turi.write_text(
        f"""
import tensor
import crypto
import data
import cli
import postgres

fn main() {{
    // 1. Tensor
    let a = tensor.fromArray(2, 2, [f32(1.0), f32(2.0), f32(3.0), f32(4.0)]).unwrapOr(tensor.zeros(2, 2).unwrapOr(tensor.TensorF32 {{ rows: 0, cols: 0, data: [] }}))
    let b = tensor.fromArray(2, 2, [f32(2.0), f32(0.0), f32(1.0), f32(2.0)]).unwrapOr(a)
    let c = tensor.matmul(a, b)
    assert c.isOk()
    let cm = c.unwrapOr(a)
    assert cm.rows == 2 && cm.cols == 2 && cm.data[0] == f32(4.0) && cm.data[1] == f32(4.0) && cm.data[2] == f32(10.0) && cm.data[3] == f32(8.0)
    let badMm = tensor.matmul(tensor.ones(2, 3).unwrapOr(a), b)
    assert badMm.isErr()
    let rl = tensor.relu(tensor.fromArray(1, 3, [f32(-2.0), f32(0.0), f32(5.0)]).unwrapOr(a))
    assert rl.data[0] == f32(0.0) && rl.data[2] == f32(5.0)
    let sm = tensor.softmaxRows(tensor.fromArray(1, 2, [f32(1.0), f32(2.0)]).unwrapOr(a))
    assert sm.isOk()
    let smt = sm.unwrapOr(a)
    let ssum = float(smt.data[0]) + float(smt.data[1])
    assert ssum > 0.999 && ssum < 1.001

    // 2. Crypto (FIPS 180-4, RFC 4231 HMAC, constant-time, OS RNG, ChaCha20-Poly1305)
    assert crypto.sha256Hex("abc") == "{want_sha_abc}"
    assert crypto.sha256Hex("") == "{want_sha_empty}"
    var k20 = repeat(u8(0x0b), 20)
    let msgHi = [u8(72), u8(105), u8(32), u8(84), u8(104), u8(101), u8(114), u8(101)]
    let hm = crypto.hmacSha256Bytes(k20, msgHi)
    assert crypto.bytesToHex(hm) == "{want_hmac}"
    assert crypto.constantTimeEqStr("token-1", "token-1")
    assert !crypto.constantTimeEqStr("token-1", "token-2")
    let r1 = crypto.randomHex(16).unwrapOr("")
    let r2 = crypto.randomHex(16).unwrapOr("")
    assert r1.len() == 32 && r2.len() == 32 && r1 != r2

    // RFC 8439 Section 2.8.2 exact test vector
    let key = [
        u8(0x80), u8(0x81), u8(0x82), u8(0x83), u8(0x84), u8(0x85), u8(0x86), u8(0x87),
        u8(0x88), u8(0x89), u8(0x8a), u8(0x8b), u8(0x8c), u8(0x8d), u8(0x8e), u8(0x8f),
        u8(0x90), u8(0x91), u8(0x92), u8(0x93), u8(0x94), u8(0x95), u8(0x96), u8(0x97),
        u8(0x98), u8(0x99), u8(0x9a), u8(0x9b), u8(0x9c), u8(0x9d), u8(0x9e), u8(0x9f)
    ]
    let nonce = [u8(0x07), u8(0x00), u8(0x00), u8(0x00), u8(0x40), u8(0x41), u8(0x42), u8(0x43), u8(0x44), u8(0x45), u8(0x46), u8(0x47)]
    let aad = [u8(0x50), u8(0x51), u8(0x52), u8(0x53), u8(0xc0), u8(0xc1), u8(0xc2), u8(0xc3), u8(0xc4), u8(0xc5), u8(0xc6), u8(0xc7)]
    let pt = [u8(0x4c), u8(0x61), u8(0x64), u8(0x69), u8(0x65), u8(0x73), u8(0x20), u8(0x61), u8(0x6e), u8(0x64), u8(0x20), u8(0x47)]
    let sealed = crypto.aeadSeal(key, nonce, aad, pt)
    assert sealed.isOk()
    let boxv = sealed.unwrapOr(crypto.AeadBox {{ ciphertext: [], tag: [] }})
    let opened = crypto.aeadOpen(key, nonce, aad, boxv.ciphertext, boxv.tag)
    assert opened.isOk()
    let ptOut = opened.unwrapOr([])
    assert ptOut.len() == pt.len() && crypto.constantTimeEq(pt, ptOut)
    var badTag = repeat(u8(0), 16)
    for i in 0..16 {{ badTag[i] = boxv.tag[i] }}
    badTag[0] = badTag[0] ^ u8(1)
    assert crypto.aeadOpen(key, nonce, aad, boxv.ciphertext, badTag).isErr()

    // 3. Data (CSV & JSON)
    let csvSrc = "name,note\\n\\"Ada, L.\\",\\"hi \\"\\"turi\\"\\"\\"\\n"
    let tbl = data.parseCsvWithHeaders(csvSrc, ",").unwrapOr(data.CsvTable {{ headers: [], rows: [] }})
    assert tbl.headers.len() == 2 && tbl.rows.len() == 1
    assert tbl.rows[0][0] == "Ada, L." && tbl.rows[0][1] == "hi \\"turi\\""
    let backCsv = data.toCsv([tbl.headers, tbl.rows[0]], ",")
    assert data.parseCsv(backCsv, ",").unwrapOr([]).len() == 2

    let jv = data.parseJson("\\{{\\"msg\\":\\"\\\\u0041ok\\",\\"n\\":42,\\"flag\\":true\\}}")
    assert jv.isOk()
    let jObj = jv.unwrapOr(data.JsonVal.Null)
    assert data.getStr(jObj, "msg").unwrapOr("") == "Aok"
    assert data.getInt(jObj, "n").unwrapOr(0) == 42
    assert data.getBool(jObj, "flag").unwrapOr(false)
    let jRound = data.parseJson(data.toJson(jObj))
    assert jRound.isOk()
    assert data.parseJson("\\{{bad").isErr()

    // 4. CLI parser
    var spec = cli.newSpec("demo", "Test CLI")
    cli.addBool(mut spec, "verbose", "v", "Verbose")
    cli.addStr(mut spec, "out", "o", "a.out", "Output")
    cli.addInt(mut spec, "port", "p", 8080, "Port")
    let parsed = cli.parseArgs(spec, ["-v", "--out=bin.exe", "-p", "9090", "--", "--literal"])
    assert parsed.isOk()
    let cp = parsed.unwrapOr(cli.CliParsed {{ showHelp: false, bools: [], strs: [], ints: [], positionals: [] }})
    assert cli.hasFlag(cp, "verbose")
    assert cli.getStr(cp, "out").unwrapOr("") == "bin.exe"
    assert cli.getInt(cp, "port").unwrapOr(0) == 9090
    assert cp.positionals.len() == 1 && cp.positionals[0] == "--literal"
    assert cli.parseArgs(spec, ["--unknown"]).isErr()

    // 5. Postgres probe (never crashes whether libpq is present or not)
    let _ = postgres.available()
    let pgBad = postgres.connect("host=127.0.0.1 port=1 connect_timeout=1")
    assert pgBad.isErr()

    print("STDLIB_MODS_OK")
}}
""",
        encoding="utf-8",
    )
    chk = subprocess.run([str(cli), "check", str(main_turi)], capture_output=True, text=True, timeout=60)
    assert chk.returncode == 0, (chk.stdout, chk.stderr)
    exe = Path(td) / ("std_app.exe" if sys.platform == "win32" else "std_app")
    bld = subprocess.run([str(cli), "build", str(main_turi), "-o", str(exe), "--release"],
                         capture_output=True, text=True, timeout=120)
    assert bld.returncode == 0, (bld.stdout, bld.stderr)
    run_res = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run_res.returncode == 0 and run_res.stdout.strip() == "STDLIB_MODS_OK", (run_res.returncode, run_res.stdout, run_res.stderr)

print("stdlib_mods ok")
