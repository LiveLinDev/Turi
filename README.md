# Turi (`veloz`)

Systems-speed compiled language (**Turi -> Typed Block IR -> LLVM IR -> Native Machine Code**) engineered for **AI agents**, **automatic memory & bounds safety**, **zero-overhead modularity**, and **C/C++-beating execution performance**.

No garbage collector, no manual `malloc`/`free`, no hidden `-ffast-math` numerical drift, and zero unused runtime bloat in compiled binaries.

---

## Why AI Agents & Systems Teams Choose Turi

1. **AI-first toolchain built into `veloz`**: single concise syntax card ([`AGENT_CARD.md`](AGENT_CARD.md)), structured JSON diagnostics with verified `fix` edits, deterministic hash-checked patching (`sig`/`patch`/`patch-body`), memory/check cost auditing (`costs --explain-checks`), and native stdio **`veloz mcp`** + **`veloz lsp`** servers.
2. **Automatic memory & strict safety**: deterministic region/owner/pool/shared management, bounds checks, integer overflow traps, and strict IEEE-754 `f32`/`f64` parity (never unsafe FMA contraction or `-ffast-math`). Checks are eliminated **only** when statically proven unnecessary.
3. **Zero-overhead modular standard library**: require only what you use via `require <pkg> 0.1.0 "std"` in `veloz.pkg`. A non-network/non-GPU binary like [`examples/fibonacci.turi`](examples/fibonacci.turi) emits **0** `@tz_web_`, `@tz_gpu_`, `@tz_crypto_`, `@tz_pg_`, or math-microkernel symbols.
4. **Verified hardware-tuned codegen + `--extreme`**: LLVM backend targets `znver4`/`x86-64-v4` directly (`.ll -> .o`) with verified non-FMA `f32` microkernels, Barrett modular reduction, branchless `u16` `argmin`, OpenCL disk-cached GPU kernels, and optional SHA-256 cached `--extreme` verification.

---

## Quick Start

### Build the Compiler (`veloz`)

**Windows (LLVM 23.1.x + VS Build Tools + CMake + Ninja + Python 3.10+):**

```powershell
.\tools\build.ps1
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
veloz bench src/main.turi --rounds 5
```

---

## Modular Standard Library (`"std"`)

Add only the packages your project needs in `veloz.pkg`:

```text
package myapp
version 0.1.0
require crypto 0.1.0 "std"
require data 0.1.0 "std"
```

| Package | Purpose | Key Modules / APIs | Runtime / Dependency Model |
|---|---|---|---|
| `web` | Async HTTP/1.1 (`IOCP` / `epoll`), WebSockets (RFC 6455), TLS 1.3, HTTPS client, SQLite WAL | `servidor`, `ws`, `cliente`, `tls`, `sqlite`, `http`, `json`, `router`, `mime` | Lazy C injection (`--web` / call-site); dynamically loads `libssl.so.3` / `libssl-3-x64.dll` & `sqlite3` at runtime |
| `gpu` | OpenCL GPU compute + persistent disk binary cache + CPU/GPU auto-dispatch | `gpu.isAvailable`, `matmulF32`, `matmulAutoF32`, `saxpyF32`, `saxpyAutoF32`, `reluF32`, `vecAddF32`, `vecMulF32` | Lazy C injection (`--gpu` / `import gpu`); dynamically loads `OpenCL.dll` / `libOpenCL.so` — **0 symbols** if unused |
| `tensor` | Bounded `TensorF32` CPU arrays & neural/matrix primitives | `zeros`, `ones`, `fromArray`, `add`, `mul`, `relu`, `transpose`, `softmaxRows`, `matmul`, `dot` | Pure Turi; **0 OpenCL dependencies** |
| `crypto` | Native cryptography without OpenSSL compile dependency | `sha256Hex`, `hmacSha256Hex`, `constantTimeEq`, `randomBytes`, `aeadSeal`, `aeadOpen` (ChaCha20-Poly1305) | Pure C99 RFC implementations + OS CSPRNG (`BCryptGenRandom` / `getrandom`); zero extra DLLs |
| `data` | Structured parsing & serialization | RFC 4180 CSV (`parseCsv`, `parseCsvWithHeaders`, `toCsv`), RFC 8259 JSON (`parseJson`, `toJson`, `getStr`, `getInt`, `getBool`) | Pure Turi |
| `cli` | Declarative CLI option parser | `newSpec`, `addBool`, `addStr`, `addInt`, `parseArgs`, `formatHelp` | Pure Turi |
| `postgres` | Parameterized PostgreSQL client returning JSON rows | `available`/`disponible`, `connect`/`conectar`, `queryJson`/`consultarJson`, `exec`/`ejecutar`, `close` | Dynamic `libpq.dll` / `libpq.so.5` (`PG_AVAIL=true` when installed; clean `Err` if absent) |

> **Zero-Overhead Check (`tests/opt_barrett_float.py`, `tests/stdlib_mods.py`, `tests/web.py`, `tests/gpu.py`):** Verified via `veloz emit-ir examples/fibonacci.turi` — emits zero web, GPU, crypto, Postgres, or tensor microkernel runtime symbols.

---

## AI-Native CLI, MCP & LSP

- **Language Card:** [`AGENT_CARD.md`](AGENT_CARD.md) (5,486 chars / ~1.5k tokens, all runnable snippets tested continuously by `tests/agent.py`).
- **MCP Server (`veloz mcp`):** stdio JSON-RPC 2.0 (`2024-11-05`, supports both line-delimited JSON and `Content-Length` framing) exposing 10 tools: `turi_check`, `turi_sig`, `turi_slice`, `turi_types`, `turi_costs`, `turi_fmt`, `turi_fix`, `turi_patch`, `turi_explain`, `turi_run`.
- **LSP Server (`veloz lsp`):** stdio JSON-RPC 2.0 with `didOpen`/`didChange`/`didSave`/`didClose`, live `publishDiagnostics`, type `hover`, and canonical `formatting`.
- **Auditing & Verified Tuning:**
  - `veloz costs <file> --explain-checks` (`veloz costes`) reports every eliminated vs kept bounds/overflow/div-zero/contract guard.
  - `veloz build <file> -o <out> --release --extreme [--extreme-budget=8]` runs bounded offline optimization (verified parity + trap equivalence + held-out checks + SHA-256 disk cache; keeps Python/AI/solvers strictly out of deployed binaries).

---

## Verified Performance Summary (v0.7 · November 2026)

All comparisons match functionality, inputs, checksums, and strict IEEE-754 arithmetic (no `-ffast-math`). Benchmarks and eval suites are kept outside the core-language repository via `.gitignore`; figures below summarize verified v0.7 local runs along with their tradeoffs.

| Suite / Hardware | Workload & Scope | Result vs Strongest Competitor | Tradeoffs & Honest Caveats |
|---|---|---|---|
| **CPU Expert Ceiling**<br>Ryzen 7 7700X (`znver4`) | 10 pinned 1T kernels vs expert `<immintrin.h>` AVX-512 C (+ PGO) | **10/10 wins** (`1.058×` to `3.852×`), exact checksums & `n=1..33` parity | Specialized kernels target common loops; non-matching loops stay safe standard LLVM vectorization |
| **GPU OpenCL + CPU `f32`**<br>RX 9070 XT vs Ryzen 7 7700X | `matmulF32` (`64..1024`) & `saxpyF32` (`16K..16M`) | CPU 1T `matmul 1024`: **`29.400 ms`** vs C++ AVX-512 1T `53.496 ms` (**`1.82×`**); GPU res: **`1.012 ms`** vs C++ OpenCL `1.003 ms`; `saxpy 16M` res: **`0.482 ms`** vs CPU 8T `7.981 ms` (**`16.56×`**) | C++ **8T** OpenMP (`11.142 ms`) beats Turi **1T** CPU (`29.400 ms`) on `1024`; PCIe H2D+D2H exceeds CPU for cold `saxpy <= 16M`; **CUDA, Metal & Intel AVX10 untested** |
| **Web HTTP/1.1 + TLS 1.3**<br>Ryzen 7 7700X (Windows IOCP)<br>+ `dev-adrian` (Linux `epoll`) | `64` conns `pipe=1/8` (`ping`, `estado`, `item`, `calcular`, `valorar`), + TLS 1.3 / WSS E2E | `calcular`: **`160k / 1.23M req/s`**, `valorar`: **`156k / 1.02M req/s`** (> `c_same_rt`, `c_fast`, Rust, Go, Goose); `ping` **`158k / 1.19M`** ties `c_same_rt` (`163k / 1.24M`) | `--web` binary is `369.0 KiB` vs `c_same_rt` `215.5 KiB`; `linux-dev-adrian` TLS 1.3/WSS/stdlib/C++23 verified isolated (`niceness 19`); Linux HTTP saturation skipped due to shared host load |
| **`--extreme` Superoptimizer**<br>Ryzen 7 7700X (`znver4`) | `matmul_n38`, `dot_div_n8192`, `u16_div_clip` + IR proof suite | Cache cuts compile `10.7–13.2s -> 1.46–1.85s`; Barrett & range rules verified in IR | On full workloads where base v0.7 already drops `matmul_n38` to `13.55 ms`, `--extreme` matches within process noise (`0.984×–1.018×`) |
| **Correctness Suites** | Compiler + runtime + sanitizers ([`CMakeLists.txt`](CMakeLists.txt)) | **40/40 CTests PASS** (Release) and **40/40 PASS** (ASan + UBSan) | OpenCL GPU benchmark hardware required only for `rx9070xt` measurements; `gpu.py` safely covers fallback where no GPU is attached |

---

## Documentation

| File | Description |
|---|---|
| [`AGENT_CARD.md`](AGENT_CARD.md) | Concise AI coding & CLI reference |
| [`SPEC.md`](SPEC.md) | Language semantics, types, ownership, safety & IR specification |
| [`ESTADO.md`](ESTADO.md) | Implementation milestones and test matrix |
| [`DECISIONES.md`](DECISIONES.md) | Design decisions and explicit limits |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Contributor guidelines, safety invariants, and verification checklist |
| [`LICENSE`](LICENSE) | MIT License |
