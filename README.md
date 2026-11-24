<div align="center">

<img src="docs/assets/hero.png" alt="Turi (veloz) — Systems-Speed AI-Native Language" width="100%">

<br><br>

[![License: MIT](https://img.shields.io/badge/License-MIT-0EA5E9?style=flat-square)](LICENSE)
[![C++23 & LLVM 23](https://img.shields.io/badge/Toolchain-C%2B%2B23%20%2B%20LLVM%2023-34D399?style=flat-square)](CMakeLists.txt)
[![CTests](https://img.shields.io/badge/CTests-40%2F40%20Release%20%2B%20ASan%2FUBSan-8B5CF6?style=flat-square)](CMakeLists.txt)
[![Platforms](https://img.shields.io/badge/Platforms-Windows%20IOCP%20%7C%20Linux%20epoll-F59E0B?style=flat-square)](#quick-start)
[![AI Native](https://img.shields.io/badge/AI%20Tooling-Native%20MCP%20%2B%20LSP-EC4899?style=flat-square)](#ai-native-toolchain-veloz-mcp--veloz-lsp)

<p>
  <a href="#for-ai-agents-instant-bootstrap">AI Bootstrap</a> •
  <a href="#why-turi">Why Turi</a> •
  <a href="#architecture--compilation-pipeline">Architecture</a> •
  <a href="#automatic-memory-safety--zero-overhead">Memory & Zero Bloat</a> •
  <a href="#verified-performance--honest-benchmarks">Benchmarks</a> •
  <a href="#code-showcase">Examples</a> •
  <a href="#modular-standard-library-std">Modular Stdlib</a> •
  <a href="#ai-native-toolchain-veloz-mcp--veloz-lsp">MCP & LSP</a> •
  <a href="#quick-start">Quick Start</a>
</p>

</div>

---

<div align="center">
  <img src="docs/assets/demo.gif" alt="veloz CLI Live Terminal Workflow" width="88%">
</div>

---

## For AI Agents: Instant Bootstrap

> **Start here if you are an LLM or coding agent writing Turi:**
> 1. **Syntax & types in ~1.5k tokens:** Read [`AGENT_CARD.md`](AGENT_CARD.md) first (`5,486` chars; syntax-checked continuously by `tests/agent.py`).
> 2. **Connect live compiler feedback:** Run `veloz mcp` (stdio JSON-RPC 2.0, `10` tools) or `veloz lsp` before guessing fixes.
> 3. **Audit & edit surgically:** Inspect guards with `veloz costs <file> --explain-checks`, inspect symbols with `veloz sig` / `veloz slice`, and patch safely by content hash with `veloz patch` / `veloz patch-body`.
> 4. **Import only what you need:** Add `require <pkg> 0.1.0 "std"` in `veloz.pkg` (`web`, `gpu`, `tensor`, `crypto`, `data`, `cli`, `postgres`, `lienzo`). Unused packages emit **0 symbols and 0 bytes**.

---

## Why Turi?

Most languages force a tradeoff between **AI reliability**, **memory safety**, and **hand-tuned C/C++ speed**. Turi (`veloz`) is engineered to deliver all three simultaneously without garbage collection pauses, manual `malloc`/`free`, or unsafe `-ffast-math` numerical drift:

| Pillar | What Turi (`veloz`) Delivers | How It Stays Honest & Safe |
|---|---|---|
| **AI-Native by Design** | Single compact spec card ([`AGENT_CARD.md`](AGENT_CARD.md)), structured JSON diagnostics, automatic verified fixes (`veloz fix`), hash-verified symbol patching (`sig`/`patch`), and native **`veloz mcp` + `veloz lsp`**. | Canonical formatting (`veloz fmt`), predictable contracts, and deterministic compiler errors reduce repair loops and token waste. |
| **C/C++-Beating Native Speed** | Direct `.ll -> .o` LLVM lowering (`-O3 -march=znver4|native`), strict non-FMA `f32` GEMM panels, Barrett `u16` modular reduction, vectorized `argmin.u16`, async IOCP/`epoll` HTTP + TLS 1.3, and OpenCL disk caching. | Beats hand-tuned AVX-512 C (`10/10` kernels) and matches/beats C/Rust/Go/Goose HTTP servers under identical functionality & **strict IEEE-754**. |
| **Deterministic Safety & Zero Bloat** | Automatic Region / Owner / Pool lifetime management, 16-byte SSO strings, bounds/overflow/div-zero/call-depth checks, and a modular `"std"` resolver. | Checks are elided **only** when statically proven redundant. Unused `"std"` modules emit **0 IR symbols and 0 bytes**. |

---

## Architecture & Compilation Pipeline

<div align="center">
  <img src="docs/assets/architecture.png" alt="Figure 1: Turi Compilation, Safety Verification & Zero-Overhead Lowering Pipeline" width="100%">
</div>

Turi lowers source code cleanly through typed safety verification into target-specific machine code, keeping offline search and unused runtimes strictly out of production binaries:

```mermaid
flowchart LR
    Src["Turi Source (.turi + veloz.pkg)"] --> FE["Frontend & AI Surface<br>(Lexer, Parser, Generics, MCP/LSP)"]
    FE --> Safe["Safety & Region Verifier<br>(Ownership, Bounds, Overflow, Depth)"]
    Safe --> IR["Typed Block IR<br>(Proven Guard Elision + Microkernels)"]
    IR --> LLVM["LLVM Codegen (.ll to .o)<br>(-O3 -march=cpu, optional PGO)"]
    LLVM --> Bin["Zero-Bloat Native Binary<br>(No GC, No Python/Solvers)"]
    Ext["Offline --extreme Pass<br>(Parity + Trap Proofs + SHA-256 Cache)"] -.-> LLVM
    Std["Lazy std IR Scanner<br>(web, gpu, crypto, postgres, tensor, data, cli)"] -.-> Bin
```

---

## Automatic Memory Safety & Zero-Overhead

<div align="center">
  <img src="docs/assets/memory-model.png" alt="Figure 2: Deterministic Memory Model & Modular Binary Footprint" width="100%">
</div>

Unlike tracing garbage collectors (which inflate binary sizes and heap RSS) or manual C/C++ pointers (which risk use-after-free and silent out-of-bounds UB), Turi tracks memory deterministically via regions, owners, pools, and 16-byte Small String Optimization (SSO). Every remaining runtime guard is transparent and auditable:

```json
$ veloz costs examples/fibonacci.turi --explain-checks
{"function":"fib","kind":"check","check":"overflow","status":"kept","file":"examples/fibonacci.turi","start":147,"end":152,"reason":"Posible desbordamiento en '+'; acota con 'requires' o usa wrappingAdd/Sub/Mul."}
{"function":"main","kind":"check","check":"depth","status":"eliminated","file":"examples/fibonacci.turi","start":228,"end":235,"reason":"Profundidad máxima de llamada acotada estáticamente (< 256 marcos)."}
```

And verifying zero bloat on [`examples/fibonacci.turi`](examples/fibonacci.turi):
- `veloz bench examples/fibonacci.turi --rounds 5` produces a **`115,712`-byte (`113.0 KiB`)** native executable (`17.944 ms` median run).
- `veloz emit-ir examples/fibonacci.turi` emits **0** `@tz_web_`, `@tz_gpu_`, `@tz_crypto_`, `@tz_pg_`, `@tz.gemm_f32_panel`, `@tz.matmul_mod`, or `@tz.argmin` symbols.

---

## Verified Performance & Honest Benchmarks

<div align="center">
  <img src="docs/assets/benchmarks.png" alt="Figure 3: Verified v0.7 Performance on Ryzen 7 7700X and RX 9070 XT" width="100%">
</div>

All comparisons match functionality, checksums, and strict IEEE-754 arithmetic (`no -ffast-math`). Detailed raw benchmark harnesses and evaluation suites are excluded from the public git tree via `.gitignore`.

### 1. CPU Expert Ceiling (1T, Ryzen 7 7700X `znver4`)
**10 / 10 wins** vs expert hand-written `<immintrin.h>` AVX-512 C (+ PGO) with exact checksums and `n = 1..33` parity:

| Kernel | Turi v0.7 | Expert AVX-512 C | Speedup |
|---|---:|---:|---:|
| `dotProduct` | **862.4 ns** | 3,322.0 ns | **3.852×** |
| `integerVariance` | **688.6 ns** | 1,827.6 ns | **2.654×** |
| `maxPool` | **602.1 ns** | 1,379.3 ns | **2.291×** |
| `fivePoint` | **754.7 ns** | 1,326.6 ns | **1.758×** |
| `sparsePages` | **2,503.6 ns** | 3,898.6 ns | **1.557×** |
| `matrixMultiply` | **1,401.9 ns** | 2,168.2 ns | **1.547×** |
| `sobelHorizontal` | **1,220.5 ns** | 1,674.6 ns | **1.372×** |
| `floydDistances` | **818.0 ns** | 992.1 ns | **1.213×** |
| `roundRobin` | **9,048.2 ns** | 10,363.6 ns | **1.145×** |
| `selectionSort` | **1,947.1 ns** | 2,059.7 ns | **1.058×** |

### 2. Web HTTP/1.1 + TLS 1.3 (`64` connections, Windows IOCP, medians, `0` errors)

| Runtime | `ping` (`pipe=1 / 8`) | `estado` (`pipe=1 / 8`) | `item` (`pipe=1 / 8`) | `calcular` (`pipe=1 / 8`) | `valorar` (`pipe=1 / 8`) | Binary Size | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Turi v0.7** | 158k / 1,188k | 159k / 1,107k | 158k / **1,226k** | **160k / 1,227k** | **156k / 1,019k** | 369.0 KiB | 6.95 MiB |
| **C (`same_rt`)** | **163k / 1,236k** | **161k / 1,205k** | **160k** / 1,198k | 155k / 1,167k | 150k / 992k | 215.5 KiB | 6.39 MiB |
| **C (`fast`) / C++ (`same_rt`)** | — | — | — | — | 992k / 951k (`pipe=8`) | 165.0 / 219.0 KiB | — / 6.42 MiB |
| **Rust** | — | — | — | — | 329k (`pipe=8`) | 696.5 KiB | 8.60 MiB |
| **Goose** | — | — | — | — | 78k (`pipe=8`) | 164.0 KiB | 4.48 MiB |
| **Go** | — | — | — | — | 53k (`pipe=8`) | 8,622.5 KiB | 22.77 MiB |

### 3. GPU OpenCL (RX 9070 XT) + CPU `f32` (`checksum_diff = 0.0`)

| Workload | Turi CPU 1T | C++ AVX-512 1T | C++ AVX-512 8T | Turi GPU (Cold / Res) | C++ OpenCL (Cold / Res) |
|---|---:|---:|---:|---:|---:|
| `matmul` (`N=64 / 256 / 512 / 1024`) | **0.006 / 0.375 / 2.877 / 29.400 ms** | 0.017 / 0.862 / 6.423 / 53.496 ms | 0.395 / 0.509 / **1.771 / 11.142 ms** | 0.267–3.555 / **0.116–1.012 ms** | 0.255–3.550 / 0.104–1.003 ms |
| `saxpy` (`16K / 1M / 4M / 16M`) | 0.002 / 0.041 / 1.697 / 8.482 ms | 0.006 / 0.037 / 1.784 / 8.248 ms | 0.007 / 0.439 / 1.745 / 7.981 ms | 1.783–17.138 / **0.129–0.482 ms** | — |

Binary sizes: `turi_gpu 196.0 KiB`, `turi_cpu 210.0 KiB`, `cpp_opencl 161.0 KiB`, `cpp_cpu_avx512 196.0 KiB`.  
Offline `--extreme` superoptimizer: SHA-256 disk cache cuts recompile times from `10.69–13.23 s -> 1.46–1.85 s`, while full workloads already lowered by v0.7 base microkernels (`matmul_n38` `13.55 ms -> 13.31 ms`) fall within `0.984×–1.018×` process noise.

> [!NOTE]
> **Known Limits & Honest Caveats**
> - **Multi-thread CPU:** C++ **8-thread** OpenMP (`11.142 ms`) beats Turi **1-thread** CPU (`29.400 ms`) on `matmul N=1024`.
> - **PCIe Cold Transfers:** Cold `saxpy <= 16M` copies over PCIe (`17.138 ms`) are slower than CPU (`8.482 ms`), so `saxpyAutoF32` dispatches thoughtfully.
> - **Specialized Kernels:** CPU microkernels target specific common loop patterns; general loops rely on safe standard LLVM vectorization.
> - **Untested Backends & Linux Saturation:** **CUDA, Metal, and Intel AVX10** have not been tested/implemented. Isolated Linux (`dev-adrian`, `g++ 13.3.0`) passed `semantics` (`3,405` programs), `stdlib`, `mcp_lsp`, TLS 1.3/WSS, and `PG_AVAIL=true`, while Linux HTTP saturation stress testing was intentionally skipped due to shared host load (`load average ~10.65`) and a live production app (`inventario-app`).

---

## Code Showcase

### 1. Deterministically Checked Native Logic ([`examples/fibonacci.turi`](examples/fibonacci.turi))

```rust
fn fib(n: int) -> int {
    var a = 0
    var b = 1
    for i in 0..n {
        let next = a + b
        a = b
        b = next
    }
    return a
}

fn main() {
    print(fib(10))
}

test "décimo término" {
    assert fib(0) == 0
    assert fib(10) == 55
}
```

### 2. Modular Async Web Server (`veloz new api --template web`)

```rust
import servidor

extern "C" fn apiHello(ctx: MutPtr<void>) {
    servidor.jsonStart(ctx)
    servidor.jsonStr(ctx, "app", "api")
    servidor.jsonBool(ctx, "ok", true)
    servidor.jsonEnd(ctx, 200)
}

fn routeSetup(srv: servidor.Servidor) {
    servidor.staticRoute(srv, "/health", 200, "text/plain; charset=utf-8", "ok")
    servidor.getRoute(srv, "/", fnPtr(apiHello))
}

fn main() {
    let srv = servidor.newServer("127.0.0.1", 0, 2)
    routeSetup(srv)
    match servidor.startBg(srv) {
        Ok(p) => {
            print("Listening on port " + str(p))
            servidor.stop(srv)
        }
        Err(e) => eprint(e)
    }
    servidor.free(srv)
}
```

### 3. OpenCL GPU Auto-Dispatch (`veloz new compute --template gpu`)

```rust
import gpu

fn main() {
    let a = [f32(1.0), f32(2.0), f32(3.0), f32(4.0)]
    let b = [f32(2.0), f32(0.0), f32(1.0), f32(2.0)]
    var c = repeat(f32(0.0), 4)
    let _ = gpu.matmulAutoF32(a, b, mut c, 2, 2, 2)
    print(str(c.len()) + " GPU=" + str(gpu.isAvailable()))
}

test "small_matmul" {
    var out = [f32(0.0)]
    let r = gpu.matmulAutoF32([f32(1.0), f32(2.0)], [f32(3.0), f32(4.0)], mut out, 1, 2, 1)
    assert r.isOk() && out[0] == f32(11.0)
}
```

---

## Modular Standard Library (`"std"`)

Declare only what your project uses inside `veloz.pkg`:

```text
package myapp
version 0.1.0
require crypto 0.1.0 "std"
require data 0.1.0 "std"
```

| Package | Purpose | Key APIs / Modules | Runtime & Dependency Model |
|---|---|---|---|
| `web` | Async HTTP/1.1 (`IOCP` / `epoll`), WebSockets (RFC 6455), TLS 1.3, HTTPS client, SQLite WAL | `servidor`, `ws`, `cliente`, `tls`, `sqlite`, `http`, `json`, `router`, `mime` | Lazy IR injection (`--web` or call-site); dynamically loads OpenSSL 3 (`libssl-3-x64.dll` / `libssl.so.3`) & `sqlite3` |
| `gpu` | OpenCL GPU compute + persistent binary disk cache + CPU/GPU auto-dispatch | `isAvailable`, `matmulF32`, `matmulAutoF32`, `saxpyF32`, `saxpyAutoF32`, `reluF32`, `vecAddF32`, `vecMulF32` | Lazy IR injection (`--gpu` or `import gpu`); dynamically loads `OpenCL.dll` / `libOpenCL.so` — **0 symbols** if unused |
| `tensor` | Bounded `TensorF32` CPU arrays & neural/matrix primitives | `zeros`, `ones`, `fromArray`, `add`, `mul`, `relu`, `transpose`, `softmaxRows`, `matmul`, `dot` | Pure Turi; **0 OpenCL dependencies** |
| `crypto` | Native cryptography without OpenSSL compile dependency | `sha256Hex`, `hmacSha256Hex`, `constantTimeEq`, `randomBytes`, `aeadSeal`, `aeadOpen` (ChaCha20-Poly1305) | Pure C99 RFC implementations + OS CSPRNG (`BCryptGenRandom` / `getrandom`); **0 extra DLLs** |
| `data` | Structured parsing & serialization | RFC 4180 CSV (`parseCsv`, `parseCsvWithHeaders`, `toCsv`), RFC 8259 JSON (`parseJson`, `toJson`, `getStr`, `getInt`, `getBool`) | Pure Turi |
| `cli` | Declarative CLI option parser | `newSpec`, `addBool`, `addStr`, `addInt`, `parseArgs`, `formatHelp` | Pure Turi |
| `postgres` | Parameterized PostgreSQL client returning JSON rows | `available`/`disponible`, `connect`/`conectar`, `queryJson`/`consultarJson`, `exec`/`ejecutar`, `close` | Dynamic `libpq.dll` / `libpq.so.5`; clean `Err` if client library is absent |
| `lienzo` | Canvas / font-backed rendering helpers | 2D canvas & bundled font resources (`lib/lienzo/fuentes`) | Pure Turi module; only linked when imported |

---

## AI-Native Toolchain (`veloz mcp` & `veloz lsp`)

Drop `veloz` directly into any MCP-compatible AI coding environment (Claude Code, Cursor, Antigravity, etc.):

```json
{
  "mcpServers": {
    "turi": {
      "command": "veloz",
      "args": ["mcp"]
    }
  }
}
```

- **MCP Server (`veloz mcp`):** stdio JSON-RPC 2.0 (`2024-11-05`, supporting both line-delimited JSON and `Content-Length` headers) exposing 10 deterministic tools:
  - `turi_check` · `turi_sig` · `turi_slice` · `turi_types` · `turi_costs` · `turi_fmt` · `turi_fix` · `turi_patch` · `turi_explain` · `turi_run`
- **LSP Server (`veloz lsp`):** stdio JSON-RPC 2.0 with `didOpen`/`didChange`/`didSave`/`didClose`, live `publishDiagnostics`, type `hover`, and canonical document `formatting`.

---

## Quick Start

### Build `veloz` from Source

**Windows (LLVM 23.1.x + VS Build Tools + CMake + Ninja + Python 3.10+):**

```powershell
# Release build + full CTest suite
.\tools\build.ps1

# AddressSanitizer + UndefinedBehaviorSanitizer build & test
.\tools\build.ps1 -BuildDir build-san -Sanitize
```

**Linux / macOS (Clang + lld + CMake + Ninja + Python 3.10+):**

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Scaffold, Check, Run, Build & Benchmark

```sh
veloz new myapp --template cli     # templates: cli | web | gpu | lib
cd myapp
veloz check src/main.turi          # type, lifetime, effect & test check
veloz run src/main.turi            # reference checked interpreter
veloz build src/main.turi -o myapp --release --cpu native
veloz costs src/main.turi --explain-checks
veloz bench src/main.turi --rounds 5
```

---

## Documentation & License

| File | Purpose |
|---|---|
| [`AGENT_CARD.md`](AGENT_CARD.md) | Concise AI coding syntax & CLI reference (`~1.5k` tokens) |
| [`AGENTS.md`](AGENTS.md) | Repository architecture map for automated contributors |
| [`SPEC.md`](SPEC.md) | Formal language semantics, types, ownership, safety & IR specification |
| [`ESTADO.md`](ESTADO.md) | Implementation milestones and verification matrix |
| [`DECISIONES.md`](DECISIONES.md) | Architectural design decisions and explicit invariants |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Safety invariants, zero-overhead rules & contributor checklist |
| [`CMakeLists.txt`](CMakeLists.txt) | Cross-platform C++23 build & CTest configuration |
| [`LICENSE`](LICENSE) | MIT License |
