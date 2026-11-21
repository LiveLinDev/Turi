"""Mide PeakWorkingSetSize de procesos Turi x64 mediante la API de Windows."""
import argparse
import ctypes
from ctypes.wintypes import BOOL, DWORD, HANDLE
import json
from pathlib import Path
import random
import statistics
import subprocess
import tempfile

SOURCE = """fn sum(n: int) -> int {
    var answer = 0
    for i in 0..n {
        var scratch = repeat(i, 131072 + i % 31)
        scratch[i % len(scratch)] = 5
        answer += scratch[len(scratch) - 1] + scratch[i % len(scratch)]
    }
    return answer
}
fn main() { print(sum(COUNT)) }
"""


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", DWORD), ("pageFaults", DWORD)] + [
        (name, ctypes.c_size_t)
        for name in "peak current pagedPeak paged nonpagedPeak nonpaged pagefile pagefilePeak".split()
    ]


def peak_kib(exe, expected):
    get_memory = ctypes.WinDLL("kernel32", use_last_error=True).K32GetProcessMemoryInfo
    get_memory.argtypes = [HANDLE, ctypes.POINTER(ProcessMemoryCounters), DWORD]
    get_memory.restype = BOOL
    with subprocess.Popen([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE) as child:
        stdout, stderr = child.communicate(timeout=60)
        assert (child.returncode, stdout, stderr) == (0, f"{expected}\n".encode(), b"")
        data = ProcessMemoryCounters()
        data.cb = ctypes.sizeof(data)
        # CPython's Popen keeps the process handle open after communicate().
        if not get_memory(child._handle, ctypes.byref(data), data.cb):
            raise ctypes.WinError(ctypes.get_last_error())
        return data.peak // 1024


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("veloz", type=Path)
    cli = parser.parse_args().veloz.resolve()
    counts = [4, 32, 256, 2048]
    samples = {n: [] for n in counts}
    with tempfile.TemporaryDirectory(prefix="turi-rss-") as folder:
        root = Path(folder)
        for n in counts:
            path = root / f"sum{n}.turi"
            path.write_text(SOURCE.replace("COUNT", str(n)), encoding="utf8")
            site = json.loads(subprocess.check_output([cli, "costes", path]))
            assert site["level"] == 2, site
            subprocess.run([cli, "build", path, "-o", root / f"sum{n}.exe"], check=True)
        order = counts * 7
        random.Random(7921).shuffle(order)
        for n in order:
            expected = n * (n - 1) // 2 + 5 * n
            samples[n].append(peak_kib(root / f"sum{n}.exe", expected))
    print("iteraciones mediana_KiB minimo_KiB maximo_KiB muestras_KiB")
    for n, row in samples.items():
        print(n, int(statistics.median(row)), min(row), max(row), row)


if __name__ == "__main__":
    main()
