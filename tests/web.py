"""Pruebas de integracion y cero-sobrecarga del paquete web y --web."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

cli = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
web_pkg_dir = (root / "lib/web").resolve()


def run_cli(*args, timeout=60):
    return subprocess.run(
        [str(cli), *[str(a) for a in args]],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=timeout,
    )


# 1. Pure-Turi unit tests inside lib/web/src/web.turi
res_chk = run_cli("check", root / "lib/web/src/web.turi")
assert res_chk.returncode == 0, (res_chk.returncode, res_chk.stdout, res_chk.stderr)

# 2. Zero-overhead check on non-web programs & duplicate --web E_USAGE
ir_fib = run_cli("emit-ir", root / "examples/fibonacci.turi")
assert ir_fib.returncode == 0, ir_fib.stderr
assert "@tz_web_" not in ir_fib.stdout, "fibonacci.turi no debe referenciar simbolos web"

build_tmp_parent = root / "build"
build_tmp_parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="veloz_web_test_", dir=build_tmp_parent) as tmp:
    tmp_path = Path(tmp)
    # 2b. Duplicate --web fails with E_USAGE
    tiny = tmp_path / "tiny.turi"
    tiny.write_text("fn main() { print(1) }\n", encoding="utf-8")
    dup = run_cli("build", tiny, "-o", tmp_path / "tiny.exe", "--web", "--web")
    assert dup.returncode != 0, "duplicar --web debe fallar"
    assert json.loads(dup.stderr)["id"] == "E_USAGE", dup.stderr

    # 2c. Importing pure modules (json/http) from web builds & runs cleanly without --web
    pure_proj = tmp_path / "pure_proj"
    (pure_proj / "src").mkdir(parents=True)
    pure_rel = Path(os.path.relpath(web_pkg_dir, pure_proj)).as_posix()
    (pure_proj / "veloz.pkg").write_text(
        f'package pureweb\nversion 0.1.0\nrequire web 0.1.0 "{pure_rel}"\n',
        encoding="utf-8",
    )
    (pure_proj / "src/main.turi").write_text(
        'import json\nimport http\nfn main() {\n'
        '    let r = json.parsearJson("\\{\\"x\\":7\\}")\n'
        '    assert r.isOk()\n'
        '    print(http.decodificarUrl("ok+1"))\n'
        '}\n',
        encoding="utf-8",
    )
    pure_exe = tmp_path / ("pure_web.exe" if sys.platform == "win32" else "pure_web")
    pure_bld = run_cli("build", pure_proj / "src/main.turi", "-o", pure_exe, "--release")
    assert pure_bld.returncode == 0, pure_bld.stderr
    pure_run = subprocess.run([str(pure_exe)], capture_output=True, text=True, encoding="utf-8", timeout=10)
    assert pure_run.returncode == 0 and pure_run.stdout.strip() == "ok 1", (pure_run.returncode, pure_run.stdout, pure_run.stderr)

    # 3. Full native end-to-end project (HTTP/1.1 + pipelining + concurrency + static + WS + client + SQLite + TLS probe)
    proj = tmp_path / "srv_proj"
    (proj / "src").mkdir(parents=True)
    pub_dir = proj / "public"
    pub_dir.mkdir()
    (pub_dir / "index.html").write_text("<h1>Hola Turi Web</h1>", encoding="utf-8")
    (pub_dir / "app.js").write_text("console.log('turi');", encoding="utf-8")
    (proj / "secret.txt").write_text("NO_EXPONER", encoding="utf-8")
    db_path = proj / "datos.db"

    srv_rel = Path(os.path.relpath(web_pkg_dir, proj)).as_posix()
    (proj / "veloz.pkg").write_text(
        f'package srvtest\nversion 0.1.0\nrequire web 0.1.0 "{srv_rel}"\n',
        encoding="utf-8",
    )

    pub_posix = pub_dir.as_posix()
    db_posix = db_path.as_posix()

    (proj / "src/app.turi").write_text(
        f"""
import http
import json
import servidor
import ws
import cliente
import sqlite
import tls

extern "C" fn mwAntes(ctx: MutPtr<void>) -> i32 {{
    let p = servidor.rutaReq(ctx)
    if p == "/bloqueado" {{
        servidor.texto(ctx, 401, "bloqueado")
        return i32(0)
    }}
    if p == "/api/mw" {{
        servidor.cabeceraResp(ctx, "X-Turi-Mw", "activo")
    }}
    return i32(1)
}}

extern "C" fn mwDespues(ctx: MutPtr<void>) {{
    let _ = servidor.metodo(ctx)
}}

extern "C" fn apiInfo(ctx: MutPtr<void>) {{
    servidor.jsonInicio(ctx)
    servidor.jsonCampoTexto(ctx, "motor", "turi-web")
    servidor.jsonCampoInt(ctx, "ver", 5)
    servidor.jsonCampoBool(ctx, "ok", true)
    servidor.jsonFin(ctx, 200)
}}

extern "C" fn apiMw(ctx: MutPtr<void>) {{
    servidor.texto(ctx, 200, "mw-ok")
}}

extern "C" fn apiItem(ctx: MutPtr<void>) {{
    let idOpt = servidor.paramInt(ctx, "id")
    let id = idOpt.unwrapOr(-1)
    let eco = servidor.cabecera(ctx, "X-Test")
    servidor.jsonInicio(ctx)
    servidor.jsonCampoInt(ctx, "id", id)
    servidor.jsonCampoTexto(ctx, "eco", eco)
    servidor.jsonFin(ctx, 200)
}}

extern "C" fn apiSumar(ctx: MutPtr<void>) {{
    let a = servidor.jsonInt(ctx, "a").unwrapOr(0)
    let b = servidor.jsonInt(ctx, "b").unwrapOr(0)
    let q = http.parsearQuery(servidor.consulta(ctx))
    let exTxt = q.get("extra").unwrapOr("0")
    let ex = tryParseInt(exTxt, 0)
    servidor.jsonInicio(ctx)
    servidor.jsonCampoInt(ctx, "suma", a + b + ex)
    servidor.jsonFin(ctx, 200)
}}

extern "C" fn apiDb(ctx: MutPtr<void>) {{
    let bd = sqlite.desdePtr(servidor.datosCtx(ctx))
    let id = servidor.paramInt(ctx, "id").unwrapOr(0)
    let qRes = sqlite.preparar(bd, "SELECT id, nombre, cantidad FROM items WHERE id = ?1")
    match qRes {{
        Err(e) => {{ servidor.texto(ctx, 500, e) }}
        Ok(q) => {{
            let _ = sqlite.ligarInt(q, 1, id)
            let stepRes = sqlite.siguienteFila(q)
            match stepRes {{
                Ok(hay) => {{
                    if hay {{
                        let rid = sqlite.colInt(q, 0)
                        let rnom = sqlite.colTexto(q, 1)
                        let rcant = sqlite.colInt(q, 2)
                        servidor.jsonInicio(ctx)
                        servidor.jsonCampoInt(ctx, "id", rid)
                        servidor.jsonCampoTexto(ctx, "nombre", rnom)
                        servidor.jsonCampoInt(ctx, "cantidad", rcant)
                        servidor.jsonFin(ctx, 200)
                    }} else {{
                        servidor.texto(ctx, 404, "no encontrado")
                    }}
                }}
                Err(e) => {{ servidor.texto(ctx, 500, e) }}
            }}
            sqlite.finalizar(q)
        }}
    }}
}}

extern "C" fn apiClienteSelf(ctx: MutPtr<void>) {{
    let q = http.parsearQuery(servidor.consulta(ctx))
    let puertoTxt = q.get("puerto").unwrapOr("80")
    let r1 = cliente.get("http://127.0.0.1:" + puertoTxt + "/ping")
    let r2 = cliente.postJson("http://127.0.0.1:" + puertoTxt + "/api/sumar?extra=5", "\\{{\\"a\\":10,\\"b\\":20\\}}")
    let s1 = match r1 {{
        Ok(r) => r.cuerpo
        Err(e) => "ERR:" + e
    }}
    let s2 = match r2 {{
        Ok(r) => r.cuerpo
        Err(e) => "ERR:" + e
    }}
    servidor.texto(ctx, 200, s1 + "|" + s2)
}}

extern "C" fn wsOpen(w: MutPtr<void>) {{
    let _ = ws.enviarTexto(w, "bienvenido")
}}

extern "C" fn wsMsg(w: MutPtr<void>, opcode: i32, data: Ptr<u8>, n: i64) {{
    if opcode == i32(ws.kOpTexto) {{
        let txt = ws.leerDatosMensaje(data, int(n))
        let _ = ws.enviarTexto(w, "eco:" + txt)
    }}
}}

extern "C" fn wsClose(w: MutPtr<void>, code: i32) {{
    let _ = ws.id(w) + int(code)
}}

fn main() {{
    let bd = match sqlite.abrir("{db_posix}") {{
        Ok(d) => d
        Err(e) => {{
            print("SQL_ERR=" + e)
            return
        }}
    }}
    let _ = sqlite.ejecutar(bd, "CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY, nombre TEXT NOT NULL, cantidad INTEGER NOT NULL);")
    let _ = sqlite.ejecutar(bd, "DELETE FROM items;")
    let ins = sqlite.preparar(bd, "INSERT INTO items(id, nombre, cantidad) VALUES(?1, ?2, ?3)")
    match ins {{
        Ok(q) => {{
            let _ = sqlite.ligarInt(q, 1, 1)
            let _ = sqlite.ligarTexto(q, 2, "Transponder-Ka")
            let _ = sqlite.ligarInt(q, 3, 7)
            let _ = sqlite.siguienteFila(q)
            sqlite.finalizar(q)
        }}
        Err(e) => {{ print("INS_ERR=" + e) }}
    }}

    let srv = servidor.nuevo("127.0.0.1", 0, 4)
    servidor.fijarDatos(srv, bd.ptr)
    servidor.middleware(srv, fnPtr(mwAntes), fnPtr(mwDespues))
    servidor.estatica(srv, "/ping", 200, "text/plain; charset=utf-8", "pong")
    servidor.get(srv, "/api/info", fnPtr(apiInfo))
    servidor.get(srv, "/api/mw", fnPtr(apiMw))
    servidor.get(srv, "/api/item/:id", fnPtr(apiItem))
    servidor.post(srv, "/api/sumar", fnPtr(apiSumar))
    servidor.get(srv, "/api/db/:id", fnPtr(apiDb))
    servidor.get(srv, "/api/client-self", fnPtr(apiClienteSelf))
    servidor.montarEstaticos(srv, "/static", "{pub_posix}")
    ws.rutaWs(srv, "/ws", fnPtr(wsOpen), fnPtr(wsMsg), fnPtr(wsClose))

    let _ = tls.disponible()

    match servidor.iniciarFondo(srv) {{
        Err(e) => {{
            print("SRV_ERR=" + e)
            sqlite.cerrar(bd)
            servidor.liberar(srv)
            return
        }}
        Ok(p) => {{
            print("PORT=" + str(p))
            flush()
        }}
    }}

    let _ = readByte()
    servidor.detener(srv)
    sqlite.cerrar(bd)
    servidor.liberar(srv)
    print("SALIDA_LIMPIA")
    flush()
}}
""",
        encoding="utf-8",
    )

    exe_path = tmp_path / ("srv_test.exe" if sys.platform == "win32" else "srv_test")
    bld = run_cli("build", proj / "src/app.turi", "-o", exe_path, "--release", "--web", timeout=120)
    assert bld.returncode == 0, (bld.returncode, bld.stdout, bld.stderr)
    assert exe_path.is_file(), "falta ejecutable"

    proc = subprocess.Popen(
        [str(exe_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    try:
        port_line = proc.stdout.readline().strip()
        assert port_line.startswith("PORT="), (port_line, proc.stderr.read())
        port = int(port_line.split("=", 1)[1])
        assert port > 0

        def http_req(raw_bytes):
            with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
                s.sendall(raw_bytes)
                s.shutdown(socket.SHUT_WR)
                chunks = []
                while True:
                    d = s.recv(4096)
                    if not d:
                        break
                    chunks.append(d)
                return b"".join(chunks)

        def read_one_http_resp(sock, buf):
            while b"\r\n\r\n" not in buf:
                d = sock.recv(8192)
                if not d:
                    raise RuntimeError("EOF before headers")
                buf.extend(d)
            sep = buf.index(b"\r\n\r\n") + 4
            hdr_raw = bytes(buf[:sep]).decode("latin1")
            del buf[:sep]
            status = int(hdr_raw.splitlines()[0].split()[1])
            headers = {}
            clen = 0
            for ln in hdr_raw.splitlines()[1:]:
                if ":" in ln:
                    k, v = ln.split(":", 1)
                    headers[k.strip().lower()] = v.strip()
            if "content-length" in headers:
                clen = int(headers["content-length"])
            while len(buf) < clen:
                d = sock.recv(8192)
                if not d:
                    raise RuntimeError("EOF before body")
                buf.extend(d)
            body = bytes(buf[:clen])
            del buf[:clen]
            return status, headers, body

        # A. Basic routes, JSON, params, query, middleware
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            buf = bytearray()
            s.sendall(b"GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, body = read_one_http_resp(s, buf)
            assert st == 200 and body == b"pong", (st, body)

            s.sendall(b"GET /api/info HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, body = read_one_http_resp(s, buf)
            assert st == 200 and json.loads(body) == {"motor": "turi-web", "ver": 5, "ok": True}, (st, body)

            s.sendall(b"GET /api/mw HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, hdrs, body = read_one_http_resp(s, buf)
            assert st == 200 and hdrs.get("x-turi-mw") == "activo" and body == b"mw-ok", (st, hdrs, body)

            s.sendall(b"GET /bloqueado HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, body = read_one_http_resp(s, buf)
            assert st == 401 and body == b"bloqueado", (st, body)

            s.sendall(b"GET /api/item/42 HTTP/1.1\r\nHost: localhost\r\nX-Test: val99\r\n\r\n")
            st, _, body = read_one_http_resp(s, buf)
            assert st == 200 and json.loads(body) == {"id": 42, "eco": "val99"}, (st, body)

            payload = b'{"a":12,"b":30}'
            s.sendall(
                f"POST /api/sumar?extra=5 HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(payload)}\r\n\r\n".encode()
                + payload
            )
            st, _, body = read_one_http_resp(s, buf)
            assert st == 200 and json.loads(body) == {"suma": 47}, (st, body)

            # SQLite WAL query route
            s.sendall(b"GET /api/db/1 HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, body = read_one_http_resp(s, buf)
            assert st == 200 and json.loads(body) == {"id": 1, "nombre": "Transponder-Ka", "cantidad": 7}, (st, body)

            # Outbound HTTP client self-test
            s.sendall(f"GET /api/client-self?puerto={port} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())
            st, _, body = read_one_http_resp(s, buf)
            assert st == 200 and body == b'pong|{"suma":35}', (st, body)

            # 404 & 405
            s.sendall(b"GET /no-existe HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, _ = read_one_http_resp(s, buf)
            assert st == 404, st
            s.sendall(b"DELETE /api/info HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, _ = read_one_http_resp(s, buf)
            assert st == 405, st

            # B. 20-request pipelining burst on one socket
            burst = b"GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n" * 20
            s.sendall(burst)
            for _ in range(20):
                st, _, body = read_one_http_resp(s, buf)
                assert st == 200 and body == b"pong"

            # C. Static files, MIME, ETag 304, traversal protection
            s.sendall(b"GET /static/index.html HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, hdrs, body = read_one_http_resp(s, buf)
            assert st == 200 and "text/html" in hdrs["content-type"] and body == b"<h1>Hola Turi Web</h1>"
            etag = hdrs["etag"]
            s.sendall(f"GET /static/index.html HTTP/1.1\r\nHost: localhost\r\nIf-None-Match: {etag}\r\n\r\n".encode())
            st, _, body = read_one_http_resp(s, buf)
            assert st == 304 and body == b"", (st, body)

            s.sendall(b"GET /static/../../secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, _ = read_one_http_resp(s, buf)
            assert st in (403, 404), st
            s.sendall(b"GET /static/%2e%2e/secret.txt HTTP/1.1\r\nHost: localhost\r\n\r\n")
            st, _, _ = read_one_http_resp(s, buf)
            assert st in (403, 404), st

        # D. Malformed HTTP request -> 400
        bad_resp = http_req(b"BADREQUEST\r\n\r\n")
        assert bad_resp.startswith(b"HTTP/1.1 400 "), bad_resp[:64]

        # E. 32 concurrent threads x 50 requests = 1600 requests
        errors = []
        def worker_fn(tid):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=10) as s:
                    buf = bytearray()
                    for k in range(50):
                        req = f"GET /api/item/{tid * 100 + k} HTTP/1.1\r\nHost: localhost\r\nX-Test: t\r\n\r\n".encode()
                        s.sendall(req)
                        st, _, body = read_one_http_resp(s, buf)
                        if st != 200 or json.loads(body)["id"] != tid * 100 + k:
                            errors.append((tid, k, st, body))
                            break
            except Exception as ex:
                errors.append((tid, repr(ex)))

        threads = [threading.Thread(target=worker_fn, args=(t,)) for t in range(32)]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        assert not errors, errors

        # F. WebSocket RFC 6455 handshake, welcome, masked text echo, Ping/Pong, Close
        ws_key = base64.b64encode(b"0123456789abcdef").decode()
        want_accept = base64.b64encode(
            hashlib.sha1((ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        ).decode()
        with socket.create_connection(("127.0.0.1", port), timeout=5) as ws_sock:
            ws_sock.sendall(
                (
                    f"GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                    f"Connection: Upgrade\r\nSec-WebSocket-Key: {ws_key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
                ).encode()
            )
            wbuf = bytearray()
            while b"\r\n\r\n" not in wbuf:
                wbuf.extend(ws_sock.recv(4096))
            sep = wbuf.index(b"\r\n\r\n") + 4
            hs = bytes(wbuf[:sep]).decode("latin1")
            del wbuf[:sep]
            assert "101 Switching Protocols" in hs and want_accept in hs, hs

            def recv_ws_frame():
                while len(wbuf) < 2:
                    wbuf.extend(ws_sock.recv(4096))
                b0, b1 = wbuf[0], wbuf[1]
                plen = b1 & 0x7F
                pos = 2
                if plen == 126:
                    while len(wbuf) < 4:
                        wbuf.extend(ws_sock.recv(4096))
                    plen = struct.unpack("!H", wbuf[2:4])[0]
                    pos = 4
                while len(wbuf) < pos + plen:
                    wbuf.extend(ws_sock.recv(4096))
                data = bytes(wbuf[pos:pos + plen])
                del wbuf[:pos + plen]
                return b0 & 0x0F, data

            def send_ws_masked(opcode, data_bytes):
                mask = b"\x12\x34\x56\x78"
                n = len(data_bytes)
                hdr = bytes([0x80 | opcode, 0x80 | n]) + mask
                masked = bytes(b ^ mask[i & 3] for i, b in enumerate(data_bytes))
                ws_sock.sendall(hdr + masked)

            op, msg = recv_ws_frame()
            assert op == 1 and msg == b"bienvenido", (op, msg)

            send_ws_masked(1, b"hola_ws")
            op, msg = recv_ws_frame()
            assert op == 1 and msg == b"eco:hola_ws", (op, msg)

            send_ws_masked(9, b"ping1")
            op, msg = recv_ws_frame()
            assert op == 10 and msg == b"ping1", (op, msg)

            send_ws_masked(8, struct.pack("!H", 1000))
            op, _ = recv_ws_frame()
            assert op == 8, op

        # Graceful shutdown
        proc.stdin.write("q\n")
        proc.stdin.flush()
        proc.stdin.close()
        out, err = proc.communicate(timeout=10)
        assert proc.returncode == 0, (proc.returncode, out, err)
        assert "SALIDA_LIMPIA" in out, (out, err)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    # 4. TLS 1.3 End-to-End Server (HTTPS + WSS + TLS 1.2 rejection) & Turi HTTPS client
    openssl_bin = shutil.which("openssl")
    if not openssl_bin and sys.platform == "win32":
        for cand in (r"C:\Program Files\Git\mingw64\bin\openssl.exe", r"C:\Program Files\PostgreSQL\18\bin\openssl.exe"):
            if Path(cand).is_file():
                openssl_bin = cand
                break
    if openssl_bin:
        cert_pem = (tmp_path / "cert.pem").as_posix()
        key_pem = (tmp_path / "key.pem").as_posix()
        cnf_path = tmp_path / "openssl.cnf"
        cnf_path.write_text(
            "[req]\n"
            "distinguished_name = dn\n"
            "x509_extensions = v3_ext\n"
            "prompt = no\n"
            "[dn]\n"
            "CN = localhost\n"
            "[v3_ext]\n"
            "subjectAltName = DNS:localhost,IP:127.0.0.1\n"
            "basicConstraints = CA:TRUE\n",
            encoding="utf-8",
        )
        subprocess.run(
            [
                openssl_bin, "req", "-x509", "-newkey", "rsa:2048",
                "-config", cnf_path.as_posix(),
                "-keyout", key_pem, "-out", cert_pem, "-days", "1", "-nodes",
            ],
            check=True, capture_output=True, text=True, timeout=30,
        )
        tls_proj = tmp_path / "tls_proj"
        (tls_proj / "src").mkdir(parents=True)
        tls_rel = Path(os.path.relpath(web_pkg_dir, tls_proj)).as_posix()
        (tls_proj / "veloz.pkg").write_text(
            f'package tlstest\nversion 0.1.0\nrequire web 0.1.0 "{tls_rel}"\n',
            encoding="utf-8",
        )
        url_file = (tls_proj / "url.txt").as_posix()
        (tls_proj / "src/srv.turi").write_text(
            f"""
import servidor
import tls
import ws

extern "C" fn apiJson(ctx: MutPtr<void>) {{
    servidor.jsonStart(ctx)
    servidor.jsonStr(ctx, "lang", "turi")
    servidor.jsonFieldInt(ctx, "v", 7)
    servidor.jsonBool(ctx, "ok", true)
    servidor.jsonEnd(ctx, 200)
}}

extern "C" fn wssOpen(w: MutPtr<void>) {{
    let _ = ws.enviarTexto(w, "hola-wss")
}}

extern "C" fn wssMsg(w: MutPtr<void>, opcode: i32, data: Ptr<u8>, n: i64) {{
    if opcode == i32(ws.kOpTexto) {{
        let txt = ws.leerDatosMensaje(data, int(n))
        let _ = ws.enviarTexto(w, "wss:" + txt)
    }}
}}

extern "C" fn wssClose(w: MutPtr<void>, code: i32) {{
    let _ = ws.id(w) + int(code)
}}

fn main() {{
    assert tls.available()
    let srv = servidor.newServer("127.0.0.1", 0, 2)
    servidor.staticRoute(srv, "/ping", 200, "text/plain; charset=utf-8", "pong-tls")
    servidor.getRoute(srv, "/json", fnPtr(apiJson))
    ws.rutaWs(srv, "/wss", fnPtr(wssOpen), fnPtr(wssMsg), fnPtr(wssClose))
    match tls.startBgTls(srv, "{cert_pem}", "{key_pem}") {{
        Err(e) => {{
            print("TLS_ERR=" + e)
            servidor.free(srv)
            return
        }}
        Ok(p) => {{
            print("PORT=" + str(p))
            flush()
        }}
    }}
    let _ = readByte()
    servidor.stop(srv)
    servidor.free(srv)
    print("TLS_SALIDA_LIMPIA")
    flush()
}}
""",
            encoding="utf-8",
        )
        (tls_proj / "src/cli.turi").write_text(
            f"""
import cliente

fn main() {{
    let url = readFile("{url_file}")
    match cliente.request("GET", url, "", "", 5000) {{
        Ok(r) => {{ print("OK:" + str(r.estado) + ":" + r.cuerpo) }}
        Err(e) => {{ print("ERR:" + e) }}
    }}
}}
""",
            encoding="utf-8",
        )
        tls_srv_exe = tmp_path / ("tls_srv.exe" if sys.platform == "win32" else "tls_srv")
        tls_cli_exe = tmp_path / ("tls_cli.exe" if sys.platform == "win32" else "tls_cli")
        b1 = run_cli("build", tls_proj / "src/srv.turi", "-o", tls_srv_exe, "--release", "--web", timeout=120)
        assert b1.returncode == 0, b1.stderr
        b2 = run_cli("build", tls_proj / "src/cli.turi", "-o", tls_cli_exe, "--release", timeout=120)
        assert b2.returncode == 0, b2.stderr

        tproc = subprocess.Popen(
            [str(tls_srv_exe)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8",
        )
        try:
            pline = tproc.stdout.readline().strip()
            assert pline.startswith("PORT="), (pline, tproc.stderr.read())
            tport = int(pline.split("=", 1)[1])

            # 4a. TLS 1.3 client with CA verification + keep-alive multi-request
            ctx13 = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx13.minimum_version = ssl.TLSVersion.TLSv1_3
            ctx13.maximum_version = ssl.TLSVersion.TLSv1_3
            ctx13.load_verify_locations(cert_pem)
            with socket.create_connection(("127.0.0.1", tport), timeout=5) as raw_s:
                with ctx13.wrap_socket(raw_s, server_hostname="localhost") as ss:
                    assert ss.version() == "TLSv1.3", ss.version()
                    buf = bytearray()
                    ss.sendall(b"GET /ping HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    st, _, body = read_one_http_resp(ss, buf)
                    assert st == 200 and body == b"pong-tls", (st, body)
                    ss.sendall(b"GET /json HTTP/1.1\r\nHost: localhost\r\n\r\n")
                    st, _, body = read_one_http_resp(ss, buf)
                    assert st == 200 and json.loads(body) == {"lang": "turi", "v": 7, "ok": True}, (st, body)

            # 4b. TLS 1.2-only client MUST be rejected
            ctx12 = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx12.maximum_version = ssl.TLSVersion.TLSv1_2
            ctx12.load_verify_locations(cert_pem)
            rejected = False
            try:
                with socket.create_connection(("127.0.0.1", tport), timeout=5) as raw12:
                    with ctx12.wrap_socket(raw12, server_hostname="localhost"):
                        pass
            except (ssl.SSLError, OSError):
                rejected = True
            assert rejected, "El servidor TLS 1.3 debe rechazar TLS <= 1.2"

            # 4c. WSS (WebSocket over TLS 1.3)
            ws_key = base64.b64encode(b"fedcba9876543210").decode()
            with socket.create_connection(("127.0.0.1", tport), timeout=5) as raw_w:
                with ctx13.wrap_socket(raw_w, server_hostname="localhost") as wss:
                    wss.sendall(
                        (
                            f"GET /wss HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
                            f"Connection: Upgrade\r\nSec-WebSocket-Key: {ws_key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
                        ).encode()
                    )
                    wbuf = bytearray()
                    while b"\r\n\r\n" not in wbuf:
                        wbuf.extend(wss.recv(4096))
                    sep = wbuf.index(b"\r\n\r\n") + 4
                    assert b"101 Switching Protocols" in wbuf[:sep], wbuf[:sep]
                    del wbuf[:sep]
                    while len(wbuf) < 10:
                        wbuf.extend(wss.recv(4096))
                    plen = wbuf[1] & 0x7F
                    assert bytes(wbuf[2:2 + plen]) == b"hola-wss", wbuf
                    del wbuf[:2 + plen]
                    mask = b"\x01\x02\x03\x04"
                    msg = b"tls"
                    wss.sendall(bytes([0x81, 0x80 | len(msg)]) + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(msg)))
                    while len(wbuf) < 2:
                        wbuf.extend(wss.recv(4096))
                    plen2 = wbuf[1] & 0x7F
                    while len(wbuf) < 2 + plen2:
                        wbuf.extend(wss.recv(4096))
                    assert bytes(wbuf[2:2 + plen2]) == b"wss:tls", wbuf

            # 4d. Turi HTTPS client: fail without CA, succeed with TURI_TLS_CA_FILE
            (tls_proj / "url.txt").write_text(f"https://127.0.0.1:{tport}/ping", encoding="utf-8")
            env_no_ca = dict(os.environ)
            env_no_ca.pop("TURI_TLS_CA_FILE", None)
            r_fail = subprocess.run([str(tls_cli_exe)], env=env_no_ca, capture_output=True, text=True, timeout=15)
            assert r_fail.returncode == 0 and r_fail.stdout.strip().startswith("ERR:"), (r_fail.stdout, r_fail.stderr)

            env_ca = dict(os.environ)
            env_ca["TURI_TLS_CA_FILE"] = cert_pem
            r_ok = subprocess.run([str(tls_cli_exe)], env=env_ca, capture_output=True, text=True, timeout=15)
            assert r_ok.returncode == 0 and r_ok.stdout.strip() == "OK:200:pong-tls", (r_ok.stdout, r_ok.stderr)

            tproc.stdin.write("q\n")
            tproc.stdin.flush()
            tproc.stdin.close()
            tout, terr = tproc.communicate(timeout=10)
            assert tproc.returncode == 0 and "TLS_SALIDA_LIMPIA" in tout, (tproc.returncode, tout, terr)
        finally:
            if tproc.poll() is None:
                tproc.kill()
                tproc.wait()

print("web ok")
