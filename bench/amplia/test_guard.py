"""Local simulation of mid-case preemption, a brief request and SSH heartbeat expiry."""

import io
import json
import os
from pathlib import Path
import sys
import tempfile
from contextlib import redirect_stdout
from unittest.mock import Mock, patch

if os.name == "nt":
    with patch.dict(sys.modules, {"resource": Mock()}):
        import spark_guard as g
else:
    import spark_guard as g


class Time:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def time(self):
        return self.now

    def sleep(self, d):
        self.now += d


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        dest = root / "results"
        dest.mkdir()
        for i in range(99):
            (dest / f"{i:03d}.json").write_text("{}")
        started = []
        killed = []

        class Sensors:
            def __init__(self):
                self.scans = 0
                self.memory_min = 10 << 30
                self.max_gap = 0
                self.start_counts = [0, 0, 0]
                self.end_counts = [1, 1, 0]

            def sample(self, pid):
                self.scans += 1
                return ["servicio_activo"] if pid == 101 else []

            def checkpoint(self):
                # A short request finishes between two samples of the second attempt.
                count = int(len(started) >= 2)
                return [count, count, 0, 0, 0, 0]

            def close(self):
                pass

        def launch(*args, **kwargs):
            pid = 101 + len(started)
            started.append(pid)
            (root / "staging/099.json").write_text(str(pid))
            return Mock(pid=pid, poll=lambda: 0)

        def stop(p):
            if p:
                killed.append(p.pid)

        clock = Time()
        with (
            patch.multiple(
                g, ROOT=root, Sensors=Sensors, time=clock, connected=lambda: True, stop_group=stop
            ),
            patch.object(g.subprocess, "Popen", launch),
            redirect_stdout(io.StringIO()),
        ):
            g.supervise("simulado", "simulado")
        events = [json.loads(k)["event"] for k in (root / "guard.jsonl").read_text().splitlines()]
        assert started == [101, 102, 103] and killed == [101]
        assert events.count("accepted") == 1 and "discard" in events and "discard_batch" in events
        assert (dest / "099.json").read_text() == "103"
        with (
            patch.multiple(g, time=clock, last_heartbeat=clock.now),
            patch.object(g.select, "select", return_value=([], [], [])),
        ):
            assert g.connected()
            clock.sleep(7.1)
            assert not g.connected()
    print("Pausa en un caso, descarte de una tanda y límite del latido SSH verificados localmente.")


if __name__ == "__main__":
    main()
