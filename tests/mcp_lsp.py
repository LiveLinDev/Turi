"""Pruebas de integracion para servidores stdio veloz mcp y veloz lsp."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

cli = Path(sys.argv[1]).resolve()


def read_lsp_msg(stream):
    clen = None
    while True:
        line = stream.readline()
        if not line:
            return None
        s = line.decode("ascii", errors="ignore").strip()
        if not s:
            if clen is not None:
                break
            continue
        if ":" in s:
            k, v = s.split(":", 1)
            if k.strip().lower() == "content-length":
                clen = int(v.strip())
    body = stream.read(clen)
    return json.loads(body.decode("utf-8"))


def write_lsp_msg(stream, obj):
    raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    stream.write(f"Content-Length: {len(raw)}\r\n\r\n".encode("ascii") + raw)
    stream.flush()


with tempfile.TemporaryDirectory() as td:
    tmp = Path(td)
    turi_file = tmp / "sample.turi"
    turi_file.write_text(
        'fn doubleIt(x: int) -> int {\n'
        '    return x * 2\n'
        '}\n'
        'fn main() {\n'
        '    print(doubleIt(21))\n'
        '}\n',
        encoding="utf-8",
    )

    # 1. MCP stdio server (line-delimited JSON-RPC 2.0)
    mcp_reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2024-11-05"}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "ping"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "turi_check", "arguments": {"file": str(turi_file)}}},
        {"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "turi_check", "arguments": {"source": "fn main() { let x: int = \"bad\" }"}}},
        {"jsonrpc": "2.0", "id": 6, "method": "tools/call", "params": {"name": "turi_sig", "arguments": {"file": str(turi_file), "symbol": "doubleIt"}}},
        {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "turi_costs", "arguments": {"file": str(turi_file), "explain_checks": True}}},
        {"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "turi_explain", "arguments": {"id": "E_IMMUTABLE"}}},
        {"jsonrpc": "2.0", "id": 9, "method": "tools/call", "params": {"name": "turi_run", "arguments": {"file": str(turi_file)}}},
    ]
    mcp_input = "\n".join(json.dumps(r) for r in mcp_reqs) + "\n"
    mcp_proc = subprocess.run([str(cli), "mcp"], input=mcp_input, capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert mcp_proc.returncode == 0, mcp_proc.stderr
    resps = {r["id"]: r for r in (json.loads(ln) for ln in mcp_proc.stdout.splitlines() if ln.strip())}
    assert resps[1]["result"]["protocolVersion"] == "2024-11-05"
    assert resps[2]["result"] == {}
    tool_names = {t["name"] for t in resps[3]["result"]["tools"]}
    for expected in ("turi_check", "turi_sig", "turi_slice", "turi_types", "turi_costs", "turi_fmt", "turi_fix", "turi_patch", "turi_explain", "turi_run"):
        assert expected in tool_names, tool_names
    chk_ok = json.loads(resps[4]["result"]["content"][0]["text"])
    assert chk_ok["ok"] is True, chk_ok
    chk_bad = json.loads(resps[5]["result"]["content"][0]["text"])
    assert chk_bad["ok"] is False and chk_bad["diagnostic"]["id"] == "E_TYPE", chk_bad
    sig_info = json.loads(resps[6]["result"]["content"][0]["text"])
    assert sig_info["name"] == "doubleIt"
    digest = sig_info["hash"]
    assert "check" in resps[7]["result"]["content"][0]["text"]
    assert "var" in resps[8]["result"]["content"][0]["text"]
    run_info = json.loads(resps[9]["result"]["content"][0]["text"])
    assert run_info["ok"] is True and run_info["stdout"].strip() == "42", run_info

    # MCP Content-Length mode + turi_patch optimistic concurrency
    patch_req = json.dumps({
        "jsonrpc": "2.0", "id": 10, "method": "tools/call",
        "params": {
            "name": "turi_patch",
            "arguments": {"file": str(turi_file), "symbol": "doubleIt", "hash": digest, "replacement": "fn doubleIt(x: int) -> int { return x * 3 }"}
        }
    }).encode("utf-8")
    cl_proc = subprocess.run(
        [str(cli), "mcp"],
        input=f"Content-Length: {len(patch_req)}\r\n\r\n".encode("ascii") + patch_req,
        capture_output=True, timeout=30,
    )
    assert cl_proc.returncode == 0 and b"Content-Length:" in cl_proc.stdout
    assert "x * 3" in turi_file.read_text(encoding="utf-8")

    # 2. LSP server (Content-Length framed)
    lsp = subprocess.Popen([str(cli), "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        uri = turi_file.as_uri()
        write_lsp_msg(lsp.stdin, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"capabilities": {}}})
        init_r = read_lsp_msg(lsp.stdout)
        assert init_r["id"] == 1 and init_r["result"]["capabilities"]["hoverProvider"] is True

        write_lsp_msg(lsp.stdin, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        bad_src = "fn main() {\n    let a: int = \"oops\"\n}\n"
        write_lsp_msg(lsp.stdin, {
            "jsonrpc": "2.0", "method": "textDocument/didOpen",
            "params": {"textDocument": {"uri": uri, "languageId": "turi", "version": 1, "text": bad_src}}
        })
        diag1 = read_lsp_msg(lsp.stdout)
        assert diag1["method"] == "textDocument/publishDiagnostics"
        assert len(diag1["params"]["diagnostics"]) == 1 and diag1["params"]["diagnostics"][0]["code"] == "E_TYPE", diag1

        good_src = "fn addOne(n:int)->int{return n+1}\nfn main(){print(addOne(41))}\n"
        write_lsp_msg(lsp.stdin, {
            "jsonrpc": "2.0", "method": "textDocument/didChange",
            "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": good_src}]}
        })
        diag2 = read_lsp_msg(lsp.stdout)
        assert diag2["params"]["diagnostics"] == [], diag2

        write_lsp_msg(lsp.stdin, {
            "jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
            "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 11}}
        })
        hov = read_lsp_msg(lsp.stdout)
        assert hov["id"] == 2 and hov["result"] is not None and "int" in hov["result"]["contents"]["value"], hov

        write_lsp_msg(lsp.stdin, {
            "jsonrpc": "2.0", "id": 3, "method": "textDocument/formatting",
            "params": {"textDocument": {"uri": uri}, "options": {"tabSize": 4, "insertSpaces": True}}
        })
        fmt_r = read_lsp_msg(lsp.stdout)
        assert fmt_r["id"] == 3 and len(fmt_r["result"]) == 1 and "fn addOne(n: int) -> int" in fmt_r["result"][0]["newText"], fmt_r

        write_lsp_msg(lsp.stdin, {"jsonrpc": "2.0", "id": 4, "method": "shutdown"})
        sd = read_lsp_msg(lsp.stdout)
        assert sd["id"] == 4 and sd["result"] is None
        write_lsp_msg(lsp.stdin, {"jsonrpc": "2.0", "method": "exit"})
        lsp.stdin.close()
        rc = lsp.wait(timeout=10)
        assert rc == 0, rc
    finally:
        if lsp.poll() is None:
            lsp.kill()
            lsp.wait()

print("mcp_lsp ok")
