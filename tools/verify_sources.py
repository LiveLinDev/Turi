"""Registra disponibilidad y versión observable de los enlaces en B8."""
import concurrent.futures
import datetime
import gzip
import hashlib
import html
import json
from pathlib import Path
import re
import socket
import sys
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def verify(url):
    row = {"url": url}
    req = urllib.request.Request(url, headers={"User-Agent": "Turi-reference-check/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=14) as res:
            sample = (gzip.GzipFile(fileobj=res) if res.headers.get("Content-Encoding") == "gzip"
                      else res).read(160000)
            row.update(status=res.status, final_url=res.geturl(),
                       content_type=res.headers.get_content_type(),
                       last_modified=res.headers.get("Last-Modified"), etag=res.headers.get("ETag"),
                       sample_sha256=hashlib.sha256(sample).hexdigest(), sample_bytes=len(sample))
            body = sample.decode("utf-8", errors="replace")
            title = re.search(r"<title[^>]*>(.*?)</title\s*>", body, re.I | re.S)
            if title:
                row["title"] = html.unescape(" ".join(title.group(1).split()))
            versions = re.findall(r"\[v(\d+)\]", body)
            if versions and "arxiv.org/abs/" in url:
                row["arxiv_version"] = max(map(int, versions))
            row["verified"] = bool(sample and (row.get("title") or sample.startswith(b"%PDF")))
            if re.search("just a moment|access denied|page not found|404 not found|verify you are human",
                         row.get("title", ""), re.I):
                row["verified"] = False
    except (urllib.error.URLError, OSError, ValueError, socket.timeout) as err:
        row.update(verified=False, status=getattr(err, "code", None), error=str(err))
    return row


def main():
    section = (ROOT / "EXPEDIENTE.md").read_text(encoding="utf8").split("## B8. Fuentes", 1)[1]
    urls = list(dict.fromkeys(re.findall(r"https?://[^\s)]+", section)))
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as workers:
        rows = list(workers.map(verify, urls))
    result = {"checked_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "HTTP, tipo, título y versión observable. El hash corresponde solo a la muestra leída.",
              "sources": rows}
    Path(sys.argv[1]).write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    print(f"{sum(x['verified'] for x in rows)}/{len(rows)} destinos con título o PDF reconocido.")


if __name__ == "__main__":
    main()
