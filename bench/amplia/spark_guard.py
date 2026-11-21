"""Supervise only this private CPU experiment; yield to production as a complete case transaction."""

import contextlib
import ctypes
import http.client
import json
import os
from pathlib import Path
import re
import resource
import select
import signal
import subprocess
import sys
import threading
import time

PORTS = {5000, 7070, 8000, 8001, 8002, 8013, 8014, 8080, 11000, 11434}
QUIET = 8
PERIOD = 0.30
GATEWAY = (
    "gateway_requests_admitted_total",
    "gateway_requests_completed_total",
    "gateway_requests_admitted_high_total",
    "vllm:num_requests_running",
    "vllm:num_requests_waiting",
    "gateway_requests_waiting_high",
)
LIMIT_RSS = 128 << 20
ROOT = Path(__file__).resolve().parent
Hz = os.sysconf("SC_CLK_TCK") if sys.platform == "linux" else 100


def metrics():
    conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=0.75)
    try:
        conn.request("GET", "/metrics", headers={"Connection": "close"})
        res = conn.getresponse()
        if res.status != 200:
            raise ValueError("gateway")
        text = res.read(2 << 20).decode()
    finally:
        conn.close()
    parsed = {}
    for line in text.splitlines():
        field = line.split("{", 1)[0].split(" ", 1)[0]
        if field in GATEWAY:
            parsed[field] = parsed.get(field, 0) + float(line.rsplit(" ", 1)[1])
    if not set(GATEWAY) <= parsed.keys():
        raise ValueError("contadores del gateway incompletos")
    return [parsed[name] for name in GATEWAY]


def network():
    active = False
    time_wait = set()
    for filename in ("/proc/net/tcp", "/proc/net/tcp6"):
        with open(filename) as f:
            next(f)
            for line in f:
                cols = line.split()
                a, b, st = cols[1:4]
                p = int(a.split(":")[1], 16)
                q = int(b.split(":")[1], 16)
                if p in PORTS or q in PORTS:
                    active |= st == "01"
                    if st == "06":
                        time_wait.add((a, b))
    return active, time_wait


def processes(worker, ignored):
    state = {}
    rss = 0
    for ent in os.scandir("/proc"):
        if not ent.name.isdecimal():
            continue
        pid = int(ent.name)
        try:
            raw = Path(ent.path, "stat").read_text()
            close = raw.rindex(")")
            parts = raw[close + 2 :].split()
            # Only count our own process group as benchmark consumption.
            if worker and int(parts[2]) == worker:
                rss += max(0, int(parts[21])) * 4096
                continue
            if pid in ignored or int(parts[6]) & 0x00200000:
                continue  # PF_KTHREAD
            state[(pid, parts[19])] = int(parts[11]) + int(parts[12])
        except (OSError, ValueError, IndexError):
            continue
    if len(state) < 10:
        raise RuntimeError("visibilidad de procesos insuficiente")
    return state, rss


def terminals():
    return {p.name: p.stat().st_atime_ns for p in Path("/dev/pts").iterdir() if p.name.isdecimal()}


class Sensors:
    def __init__(self):
        # One low-rate nvidia-smi watcher. It reads utilization and never creates a CUDA context.
        self.gpu = (None, 0.0)
        self.gpu_monitor = subprocess.Popen(
            [
                "nvidia-smi",
                "--query-gpu=utilization.gpu",
                "--format=csv,noheader,nounits",
                "-lms",
                "300",
            ],
            stdout=subprocess.PIPE,
            text=True,
        )

        def read():
            for line in self.gpu_monitor.stdout:
                try:
                    self.gpu = (int(line.strip()), time.monotonic())
                except ValueError:
                    self.gpu = (None, 0.0)

        threading.Thread(target=read, daemon=True).start()
        self.previous = None
        self.memory_min = float("inf")
        self.scans = 0
        self.start_counts = None
        self.end_counts = None
        self.max_gap = 0.0
        self.last_cpu = 0.0
        self.last_rss = 0
        self.min_sample = time.monotonic()
        self.ignored = {os.getpid(), self.gpu_monitor.pid}
        self.tty = terminals()
        self.tty_until = 0.0

    def checkpoint(self):
        counts = metrics()
        if self.start_counts is None:
            self.start_counts = counts[:3]
        self.end_counts = counts[:3]
        # The gateway verifies its model card through two internal ports.
        # Resolve only our own completed read outside any measured group.
        time.sleep(0.18)
        if self.previous:
            then, _, procs = self.previous
            self.previous = then, network()[1], procs
        return counts

    def sample(self, worker=None):
        now = time.monotonic()
        self.scans += 1
        self.max_gap = max(self.max_gap, now - self.min_sample)
        self.min_sample = now
        try:
            active, closed = network()
            proc, rss = processes(worker, self.ignored)
            av = (
                int(re.search(r"^MemAvailable:\s+(\d+)", Path("/proc/meminfo").read_text(), re.M)[1])
                * 1024
            )
            self.memory_min = min(self.memory_min, av)
            tty = terminals()
            if any(k not in self.tty or self.tty[k] != v for k, v in tty.items()):
                self.tty_until = now + 30
            self.tty = tty
            gpu, stamp = self.gpu
            causes = []
            if gpu is None or now - stamp > 1.8:
                causes.append("sensor_gpu")
            elif gpu > 0:
                causes.append("gpu")
            if active:
                causes.append("servicio_activo")
            if av < 8 << 30:
                causes.append("memoria_disponible")
            if rss > LIMIT_RSS:
                causes.append("limite_memoria_propia")
            if now < self.tty_until:
                causes.append("terminal")
            if self.previous:
                then, net, prior = self.previous
                if closed - net:
                    causes.append("solicitud")
                elapsed = max(0.001, now - then)
                cpu = max(
                    (max(0, v - prior.get(k, 0)) / Hz / elapsed for k, v in proc.items()), default=0
                )
                self.last_cpu = cpu
                if cpu > 0.25:
                    causes.append("otra_cpu")
            self.last_rss = rss
            self.previous = now, closed, proc
            return causes
        except (OSError, ValueError, RuntimeError, AttributeError, IndexError, TimeoutError) as exc:
            self.previous = None
            return ["sensor_" + type(exc).__name__]

    def close(self):
        self.gpu_monitor.terminate()
        self.gpu_monitor.wait(timeout=3)


def stop_group(proc):
    if proc is None:
        return
    with contextlib.suppress(ProcessLookupError):
        os.killpg(proc.pid, signal.SIGKILL)
    proc.wait()


def worker_limits():
    parent = os.getppid()
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(1, signal.SIGKILL, 0, 0, 0) or os.getppid() != parent:
        os._exit(117)
    os.sched_setaffinity(0, {19})
    os.nice(19)
    os.sched_setscheduler(0, os.SCHED_IDLE, os.sched_param(0))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_AS, (2 << 30, 2 << 30))


last_heartbeat = time.monotonic()


def connected():
    global last_heartbeat
    ready, _, _ = select.select([sys.stdin], [], [], 0)
    if ready:
        if not os.read(sys.stdin.fileno(), 32):
            return False
        last_heartbeat = time.monotonic()
    return time.monotonic() - last_heartbeat < 7


def main():
    if os.geteuid() == 0 or not re.fullmatch(r"/tmp/turi-cpu-[A-Za-z0-9]+", str(ROOT)):
        raise RuntimeError(
            "se necesita el directorio privado creado para este trabajo y un usuario ordinario"
        )
    if ROOT.stat().st_mode & 0o077:
        raise RuntimeError("directorio compartido")

    def interrupted(*_):
        raise KeyboardInterrupt()

    signal.signal(signal.SIGHUP, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    supervise(os.uname().machine, os.uname().release)


def supervise(machine, kernel):
    staging = ROOT / "staging"
    staging.mkdir(exist_ok=True)
    dst = ROOT / "results"
    dst.mkdir(exist_ok=True)
    sensor = Sensors()
    journal = open(ROOT / "guard.jsonl", "a", buffering=1)

    def record(what, **data):
        line = json.dumps(dict(event=what, time=time.time(), **data))
        journal.write(line + "\n")
        print(line, flush=True)

    proc = None
    quiet_since = None
    reason = None
    try:
        record(
            "start",
            cpu=19,
            scheduler="SCHED_IDLE",
            nice=19,
            rss_limit=LIMIT_RSS,
            quiet_s=QUIET,
            machine=machine,
            kernel=kernel,
        )
        i = 0
        batch = None
        provisional = []
        while i < 100:
            target = dst / f"{i:03d}.json"
            if target.exists():
                i += 1
                continue
            temp = staging / target.name
            while i not in provisional:
                if not connected():
                    raise EOFError("SSH cerrado")
                causes = sensor.sample(proc.pid if proc else None)
                now = time.monotonic()
                if causes:
                    if proc:
                        stop_group(proc)
                        proc = None
                        temp.unlink(missing_ok=True)
                        record("discard", case=i, reasons=causes)
                    if reason != causes:
                        record("pause", case=i, reasons=causes)
                        reason = causes
                    quiet_since = None
                elif proc is not None:
                    rc = proc.poll()
                    if rc is not None:
                        if rc or not temp.is_file():
                            raise RuntimeError(("worker", i, rc))
                        proc = None
                        provisional.append(i)
                else:
                    if quiet_since is None:
                        quiet_since = now
                    if now - quiet_since >= QUIET:
                        if batch is None:
                            try:
                                counts = sensor.checkpoint()
                            except (OSError, ValueError, http.client.HTTPException):
                                quiet_since = None
                                record("pause", case=i, reasons=["sensor_gateway"])
                                continue
                            if counts[0] != counts[1] or any(counts[3:]):
                                quiet_since = None
                                record("pause", case=i, reasons=["cola_gateway"])
                                continue
                            batch = counts[:3]
                            quiet_since = None
                            continue
                        if reason:
                            record("resume", case=i)
                        reason = None
                        temp.unlink(missing_ok=True)
                        proc = subprocess.Popen(
                            [
                                "ionice",
                                "-c3",
                                sys.executable,
                                str(ROOT / "run.py"),
                                "--one",
                                str(i),
                                "--root",
                                str(ROOT),
                                "--out",
                                str(staging),
                            ],
                            stdout=subprocess.DEVNULL,
                            start_new_session=True,
                            preexec_fn=worker_limits,
                        )
                if i not in provisional:
                    time.sleep(PERIOD)
            i += 1
            if len(provisional) >= 10 or i == 100:
                try:
                    counts = sensor.checkpoint()
                except (OSError, ValueError, http.client.HTTPException):
                    counts = None
                if counts is None or counts[:3] != batch or counts[0] != counts[1] or any(counts[3:]):
                    record("discard_batch", cases=provisional, reasons=["contador_gateway"])
                    i = provisional[0]
                    for number in provisional:
                        (staging / f"{number:03d}.json").unlink(missing_ok=True)
                else:
                    for number in provisional:
                        f = staging / f"{number:03d}.json"
                        f.replace(dst / f.name)
                        record("accepted", case=number)
                provisional = []
                batch = None
                quiet_since = None
        record(
            "complete",
            scans=sensor.scans,
            min_available_bytes=sensor.memory_min,
            max_sensor_gap_s=round(sensor.max_gap, 3),
            start_counts=sensor.start_counts,
            end_counts=sensor.end_counts,
        )
    finally:
        stop_group(proc)
        sensor.close()
        journal.close()


if __name__ == "__main__":
    main()
