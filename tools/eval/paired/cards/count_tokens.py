"""Cuenta tarjetas con el vocabulario y las fusiones del GGUF Qwen2 instalado por Ollama."""
import argparse
import json
import os
from pathlib import Path
import struct

from tokenizers import Regex, Tokenizer, models, normalizers, pre_tokenizers

HERE = Path(__file__).resolve().parent
PRETOKENIZE = r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
PRIMITIVES = {0:1, 1:1, 2:2, 3:2, 4:4, 5:4, 6:4, 7:1, 10:8, 11:8, 12:8}


def vocabulary(path):
    def value(stream, kind, keep=False):
        if kind == 8:
            length = struct.unpack("<Q", stream.read(8))[0]
            if keep:
                return stream.read(length).decode("utf-8")
            stream.seek(length, 1)
        elif kind == 9:
            child, count = struct.unpack("<IQ", stream.read(12))
            if child in PRIMITIVES and not keep:
                stream.seek(count * PRIMITIVES[child], 1)
            elif keep:
                return [value(stream, child, True) for _ in range(count)]
            else:
                for _ in range(count):
                    value(stream, child)
        else:
            stream.seek(PRIMITIVES[kind], 1)

    wanted = {"tokenizer.ggml.pre", "tokenizer.ggml.tokens", "tokenizer.ggml.merges"}
    result = {}
    with path.open("rb") as stream:
        magic, version, _, count = struct.unpack("<4sIQQ", stream.read(24))
        if magic != b"GGUF" or version != 3:
            raise ValueError("Se esperaba GGUF v3")
        for _ in range(count):
            key = value(stream, 8, True)
            kind = struct.unpack("<I", stream.read(4))[0]
            data = value(stream, kind, key in wanted)
            if key in wanted:
                result[key] = data
                if result.keys() == wanted:
                    break
    if result["tokenizer.ggml.pre"] != "qwen2":
        raise ValueError("El GGUF no usa el pretokenizador de Qwen2")
    return result["tokenizer.ggml.tokens"], result["tokenizer.ggml.merges"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--models", type=Path, default=Path(os.environ.get("OLLAMA_MODELS", Path.home()/".ollama/models")))
    args = parser.parse_args()
    tag = args.models / "manifests/registry.ollama.ai/library/qwen2.5/7b"
    info = json.loads(tag.read_bytes())
    digest = next(l["digest"] for l in info["layers"] if l["mediaType"].endswith(".model"))
    tokens, merges = vocabulary(args.models / "blobs" / digest.replace(":", "-"))
    tokenizer = Tokenizer(models.BPE({v:i for i,v in enumerate(tokens)}, [tuple(m.split(" ")) for m in merges]))
    tokenizer.normalizer = normalizers.NFC()
    tokenizer.pre_tokenizer = pre_tokenizers.Sequence([
        pre_tokenizers.Split(Regex(PRETOKENIZE), behavior="isolated"),
        pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=False),
    ])
    counts = {lang: len(tokenizer.encode((HERE / (lang.lower()+".txt")).read_text(encoding="utf-8-sig").strip()).ids)
              for lang in ("Turi", "Go", "Rust")}
    print(json.dumps({"gguf": digest, "cards":counts}))


if __name__ == "__main__":
    main()
