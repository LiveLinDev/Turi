"""Prueba las órdenes del agente como las invocaría un editor."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]


def call(*args, ok=True):
    result = subprocess.run([cli, *args], capture_output=True, timeout=10)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result.stdout if ok else json.loads(result.stderr)


with tempfile.TemporaryDirectory() as directory:
    file = Path(directory) / "edición.turi"
    patch = file.with_suffix(".fn")
    file.write_text('// Una ficha.\nstruct Tag { x: int }\nfn emit(n:int){print(n)}\n'
                    '// Llama a emit.\nfn outer(){emit(2)}\nfn main(){outer()}\n'
                    'test "dato" { assert 2 > 1 }\n', encoding="utf8")
    entries = [json.loads(line) for line in call("sig", file).splitlines()]
    outer = next(entry for entry in entries if entry["name"] == "outer")
    assert outer["doc"] == "Llama a emit." and outer["io"] is True
    assert json.loads(call("sig", file, "Tag"))["signature"].startswith("struct Tag")
    assert json.loads(call("types", file, "outer").splitlines()[-1])["type"] == "int"
    assert call("sig", file, "--offset", "2").splitlines() == call("sig", file).splitlines()[2:]
    assert call("explain", "R_OVERFLOW")
    assert call("fmt", file, "--check", ok=False)["id"] == "E_FORMAT"
    call("fmt", file)
    source = file.read_bytes()
    call("fmt", file)
    assert source == file.read_bytes() and b"// Llama a emit." in source
    call("check", file, "--format")
    patch.write_text("fn outer() { print(99) }", encoding="utf8")
    assert call("patch", file, "outer", outer["hash"], patch, ok=False)["id"] == "E_STALE"
    assert file.read_bytes() == source
    digest = json.loads(call("sig", file, "outer"))["hash"]
    patch.write_text("fn outer() { print(unknown) }", encoding="utf8")
    call("patch", file, "outer", digest, patch, ok=False)
    assert file.read_bytes() == source
    patch.write_text("fn outer() { print(99) }", encoding="utf8")
    call("patch", file, "outer", digest, patch)
    assert call("run", file) == b"99\n"
    assert call("patch", file, "outer", digest, patch, ok=False)["id"] == "E_STALE"
    # Every complete program printed on the agent card is runnable with check.
    for snippet in (root / "AGENT_CARD.md").read_text(encoding="utf8").split("```turi\n")[1:]:
        file.write_text(snippet.split("```", 1)[0], encoding="utf8")
        call("check", file)
print("Firmas, tipos, formato, reparación por hash y tarjeta verificados.")
