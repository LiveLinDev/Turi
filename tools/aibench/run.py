"""Multi-language AI coding benchmark: does a model make fewer errors writing Turi than other languages?

For every (task, language) the model gets the same English prompt, a typed signature and two public
examples. The reply is compiled and run against public and hidden tests. On failure the model gets
up to --repairs repair rounds with the compiler message or the failing public example.

Reported per language:
  compile@1   first reply builds
  pass@1      first reply passes every test (public and hidden)
  pass@N      passes within N attempts
  tokens/ok   total completion tokens spent (all attempts) per solved task
  code tokens tokens of the final correct solution, counted with the model's own tokenizer

The endpoint is OpenAI-compatible. With --ssh HOST the script opens a local-only SSH tunnel and, if
the server exposes the gateway counters in /metrics, waits for it to be idle before every request so
that shared production traffic is never delayed. Requests are strictly sequential.
"""
import argparse
import hashlib
import http.client
import json
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from langs import all_langs, run_candidate  # noqa: E402
from tasks import PUBLIC, TASKS  # noqa: E402

GATEWAY_METRICS = (
    "vllm:num_requests_running",
    "vllm:num_requests_waiting",
    "gateway_requests_waiting_high",
    "gateway_requests_admitted_total",
    "gateway_requests_completed_total",
    "gateway_requests_admitted_high_total",
)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Endpoint:
    def __init__(self, base_url=None, ssh=None, remote_port=8000):
        self.proc = None
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        if ssh:
            port = free_port()
            self.base_url = f"http://127.0.0.1:{port}"
            self.proc = subprocess.Popen(
                ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15",
                 "-o", "ServerAliveCountMax=3", "-o", "ExitOnForwardFailure=yes", "-T", "-N",
                 "-L", f"127.0.0.1:{port}:127.0.0.1:{remote_port}", ssh],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            deadline = time.monotonic() + 20
            while True:
                if self.proc.poll() is not None:
                    raise RuntimeError("SSH tunnel failed: " + (self.proc.stderr.read() or "").strip())
                try:
                    self.get("/v1/models", timeout=2)
                    break
                except (OSError, urllib.error.URLError, http.client.HTTPException):
                    if time.monotonic() > deadline:
                        self.close()
                        raise TimeoutError("SSH tunnel did not come up")
                    time.sleep(0.3)
        else:
            self.base_url = base_url.rstrip("/")

    def close(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def get(self, path, timeout=5):
        req = urllib.request.Request(self.base_url + path, headers={"Connection": "close"})
        with self.opener.open(req, timeout=timeout) as resp:
            return resp.read(4 << 20).decode("utf-8")

    def post(self, path, payload, timeout=300):
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(self.base_url + path, data=data,
                                     headers={"Content-Type": "application/json", "Connection": "close"})
        try:
            with self.opener.open(req, timeout=timeout) as resp:
                return json.load(resp)
        except urllib.error.HTTPError as ex:
            raise RuntimeError(f"HTTP {ex.code}: {ex.read().decode('utf-8', 'replace')[:600]}") from ex

    def metrics(self):
        parsed = {}
        for line in self.get("/metrics", timeout=4).splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            field = line.split("{", 1)[0].split(" ", 1)[0]
            if field in GATEWAY_METRICS:
                parsed[field] = parsed.get(field, 0.0) + float(line.rsplit(" ", 1)[1])
        if not set(GATEWAY_METRICS) <= parsed.keys():
            raise ValueError("gateway counters missing")
        return parsed


def idle(m):
    return (m["vllm:num_requests_running"] == 0 and m["vllm:num_requests_waiting"] == 0
            and m["gateway_requests_waiting_high"] == 0
            and m["gateway_requests_admitted_total"] == m["gateway_requests_completed_total"])


class PoliteGate:
    """Yield to any other traffic on a shared server: require a quiet window before each request."""

    def __init__(self, endpoint, quiet_s=3.0, yield_s=15.0, pause_s=1.5, max_wait_s=900.0):
        self.endpoint, self.quiet_s, self.yield_s = endpoint, quiet_s, yield_s
        self.pause_s, self.max_wait_s = pause_s, max_wait_s
        self.enabled = True
        self.expected = None
        self.window = quiet_s
        self.yields = 0
        try:
            endpoint.metrics()
        except Exception:
            self.enabled = False

    def wait(self):
        if not self.enabled:
            time.sleep(self.pause_s)
            return
        started = time.monotonic()
        quiet_since = None
        while True:
            try:
                m = self.endpoint.metrics()
            except Exception:
                quiet_since = None
                if time.monotonic() - started > self.max_wait_s:
                    raise TimeoutError("server unreachable")
                time.sleep(1.0)
                continue
            admitted = m["gateway_requests_admitted_total"]
            foreign = self.expected is not None and admitted > self.expected
            if not idle(m) or foreign:
                if quiet_since is not None or self.window != self.yield_s:
                    print("[gate] other traffic on the server; yielding", flush=True)
                self.yields += 1
                self.window = self.yield_s
                self.expected = admitted
                quiet_since = None
                if time.monotonic() - started > self.max_wait_s:
                    raise TimeoutError("server busy for too long; stopping politely")
                time.sleep(1.0)
                continue
            if quiet_since is None:
                quiet_since = time.monotonic()
                self.expected = admitted
            if time.monotonic() - quiet_since >= self.window:
                self.expected = admitted
                return
            time.sleep(0.5)

    def done(self):
        if not self.enabled:
            return
        time.sleep(0.2)
        try:
            m = self.endpoint.metrics()
        except Exception:
            self.window = self.yield_s
            return
        foreign = m["gateway_requests_admitted_total"] > (self.expected or 0) + 1 or not idle(m)
        self.expected = m["gateway_requests_admitted_total"]
        self.window = self.yield_s if foreign else self.quiet_s


def system_prompt(lang):
    text = f"You are an expert {lang.title} programmer. {lang.reply_rule}"
    if lang.card:
        text += "\n\nLanguage reference:\n\n" + lang.card
    return text


def user_prompt(lang, task):
    lines = [f"Write this {lang.title} function:", "", lang.signature(task), "", task.doc]
    note = lang.notes(task)
    if note:
        lines += ["", note]
    lines += ["", "Examples:"]
    from langs import canon
    for args, expected in task.tests[:PUBLIC]:
        lines.append(f"  {lang.call_text(task, args)}  ->  {canon(expected)}")
    return "\n".join(lines)


FENCE = re.compile(r"```[A-Za-z0-9_+#-]*\n(.*?)```", re.S)


def extract_code(text):
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
    blocks = FENCE.findall(text)
    if blocks:
        return max(blocks, key=len).strip("\n")
    return text.strip()


def chat(endpoint, gate, model, messages, max_tokens, effort):
    payload = {"model": model, "temperature": 0, "seed": 20261124, "max_tokens": max_tokens, "messages": messages}
    if effort:
        payload["reasoning_effort"] = effort
    gate.wait()
    started = time.monotonic()
    try:
        body = endpoint.post("/v1/chat/completions", payload)
    finally:
        gate.done()
    msg = body["choices"][0]["message"]
    content = msg.get("content") or ""
    usage = body.get("usage") or {}
    return content, usage, time.monotonic() - started


def count_tokens(endpoint, text, cache={}):
    key = hashlib.sha1(text.encode("utf-8")).hexdigest()
    if key in cache:
        return cache[key]
    def chat_count(content):
        body = endpoint.post("/tokenize", {"model": count_tokens.model, "messages": [{"role": "user", "content": content}]}, timeout=30)
        return body.get("count")

    try:
        # Gateways that tokenize chat messages: subtract the template overhead of an empty message.
        if "base" not in cache:
            cache["base"] = chat_count("")
        total = chat_count(text)
        if total is not None and cache["base"] is not None:
            cache[key] = total - cache["base"]
            return cache[key]
    except Exception:
        pass
    for path, payload in (("/v1/token/encode", {"text": text}), ("/tokenize", {"content": text})):
        try:
            body = endpoint.post(path, payload, timeout=30)
        except Exception:
            continue
        n = body.get("length") or len(body.get("tokens") or [])
        if n:
            cache[key] = n
            return n
    cache[key] = None
    return None


count_tokens.model = ""


def summarize(rows, attempts):
    by = {}
    for r in rows:
        by.setdefault(r["lang"], []).append(r)
    out = []
    for lang, rs in by.items():
        n = len(rs)
        solved = [r for r in rs if r["solved_at"] is not None]
        code_tokens = [r["code_tokens"] for r in solved if r.get("code_tokens")]
        out.append({
            "lang": lang, "tasks": n,
            "compile@1": sum(1 for r in rs if r["attempts"][0]["compiled"]) / n,
            "pass@1": sum(1 for r in rs if r["solved_at"] == 1) / n,
            f"pass@{attempts}": len(solved) / n,
            "completion_tokens_per_solved": (sum(r["completion_tokens"] for r in rs) / len(solved)) if solved else None,
            "prompt_tokens_first": statistics.mean(r["attempts"][0]["prompt_tokens"] or 0 for r in rs),
            "code_tokens_median": statistics.median(code_tokens) if code_tokens else None,
            "code_tokens_mean": statistics.mean(code_tokens) if code_tokens else None,
        })
    return out


def print_table(summary, attempts):
    cols = ["lang", "tasks", "compile@1", "pass@1", f"pass@{attempts}", "completion_tokens_per_solved",
            "prompt_tokens_first", "code_tokens_median", "code_tokens_mean"]
    print(" | ".join(cols))
    for s in summary:
        cells = []
        for c in cols:
            v = s[c]
            if isinstance(v, float):
                cells.append(f"{v:.3f}" if v <= 1.0 and "@" in c else f"{v:.1f}")
            else:
                cells.append(str(v))
        print(" | ".join(cells))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ssh", help="SSH host alias to tunnel through (the server listens on 127.0.0.1)")
    ap.add_argument("--remote-port", type=int, default=8000)
    ap.add_argument("--base-url", help="OpenAI-compatible base URL when no tunnel is needed")
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True, help="JSONL log; rerunning resumes it")
    ap.add_argument("--langs", default="turi,python,c,cpp,rust,go")
    ap.add_argument("--tasks", default="", help="comma-separated task names (default: all)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--repairs", type=int, default=2)
    ap.add_argument("--max-tokens", type=int, default=4096)
    ap.add_argument("--effort", default="low", help="reasoning_effort sent to the server ('' to omit)")
    ap.add_argument("--compiler", help="path to veloz (default: build/veloz or $VELOZ)")
    ap.add_argument("--selftest", action="store_true", help="only verify the Python reference against its own driver")
    ap.add_argument("--summary", action="store_true", help="only print the summary of --out")
    args = ap.parse_args()

    attempts = 1 + args.repairs
    out_path = Path(args.out)
    rows = [json.loads(l) for l in out_path.read_text(encoding="utf-8").splitlines() if l.strip()] if out_path.exists() else []
    if args.summary:
        print_table(summarize(rows, attempts), attempts)
        return
    langs = [l for l in all_langs(args.compiler) if l.name in args.langs.split(",")]
    missing = [l.name for l in langs if not l.available()]
    if missing:
        raise SystemExit("Toolchain missing for: " + ", ".join(missing))
    tasks = [t for t in TASKS if not args.tasks or t.name in args.tasks.split(",")]
    if args.limit:
        tasks = tasks[:args.limit]
    done = {(r["task"], r["lang"]) for r in rows}
    if not args.ssh and not args.base_url:
        raise SystemExit("Give --ssh HOST or --base-url URL")
    endpoint = Endpoint(args.base_url, args.ssh, args.remote_port)
    count_tokens.model = args.model
    gate = PoliteGate(endpoint)
    print(f"[bench] {len(tasks)} tasks x {len(langs)} languages, gate={'on' if gate.enabled else 'off'}", flush=True)
    tmp_root = Path(tempfile.mkdtemp(prefix="aibench-"))
    try:
        with out_path.open("a", encoding="utf-8", newline="\n") as log:
            for task in tasks:
                for lang in langs:
                    if (task.name, lang.name) in done:
                        continue
                    messages = [{"role": "system", "content": system_prompt(lang)},
                                {"role": "user", "content": user_prompt(lang, task)}]
                    row = {"task": task.name, "lang": lang.name, "model": args.model, "attempts": [],
                           "solved_at": None, "completion_tokens": 0, "code_tokens": None}
                    for attempt in range(1, attempts + 1):
                        content, usage, seconds = chat(endpoint, gate, args.model, messages, args.max_tokens, args.effort)
                        code = extract_code(content)
                        result = run_candidate(lang, task, code, tmp_root / f"{task.name}-{lang.name}-{attempt}")
                        row["completion_tokens"] += usage.get("completion_tokens") or 0
                        row["attempts"].append({
                            "compiled": result["compiled"], "stage": result["stage"], "all_ok": result["all_ok"],
                            "detail": result["detail"][:800], "code": code, "seconds": round(seconds, 2),
                            "prompt_tokens": usage.get("prompt_tokens"), "completion_tokens": usage.get("completion_tokens"),
                        })
                        if result["all_ok"]:
                            row["solved_at"] = attempt
                            row["code_tokens"] = count_tokens(endpoint, code)
                            break
                        if attempt < attempts:
                            if not result["compiled"]:
                                feedback = "The code does not build:\n" + result["detail"]
                            else:
                                feedback = "The code builds but is wrong. " + result["detail"]
                            messages += [{"role": "assistant", "content": content},
                                         {"role": "user", "content": feedback + "\n\nReply with the full corrected code."}]
                    log.write(json.dumps(row, ensure_ascii=False) + "\n")
                    log.flush()
                    rows.append(row)
                    first = row["attempts"][0]
                    print(f"[bench] {task.name:28s} {lang.name:7s} first={first['stage']:5s} solved_at={row['solved_at']} "
                          f"tok={row['completion_tokens']}", flush=True)
    finally:
        endpoint.close()
        shutil.rmtree(tmp_root, ignore_errors=True)
    print_table(summarize(rows, attempts), attempts)
    print(f"[bench] gate yields: {gate.yields}")


if __name__ == "__main__":
    main()
