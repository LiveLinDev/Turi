"""Prueba oficial del paquete visual lienzo."""
import subprocess
import sys
from pathlib import Path

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]

for rel in ["lib/lienzo/src/lienzo.turi", "lib/lienzo/src/tipografia.turi", "lib/lienzo/src/trazo.turi"]:
    target = root / rel
    res = subprocess.run([str(cli), "check", str(target)], capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert res.returncode == 0, (rel, res.returncode, res.stdout, res.stderr)

fuentes_dir = root / "lib/lienzo/fuentes"
for ttf_name in [
    "Inter-Regular.ttf",
    "Inter-SemiBold.ttf",
    "Inter-Bold.ttf",
    "JetBrainsMono-Regular.ttf",
    "JetBrainsMono-Bold.ttf",
    "JetBrainsMono-Italic.ttf",
]:
    p = fuentes_dir / ttf_name
    assert p.is_file(), f"falta fuente {p}"
    data = p.read_bytes()
    assert len(data) > 100_000, (ttf_name, len(data))
    assert data[:4] == b"\x00\x01\x00\x00", (ttf_name, data[:4])

for lic_name in ["LICENSE-Inter.txt", "OFL-JetBrainsMono.txt"]:
    lp = fuentes_dir / lic_name
    assert lp.is_file(), f"falta licencia {lp}"
    assert "SIL Open Font License" in lp.read_text(encoding="utf-8"), lic_name

print("lienzo ok")
