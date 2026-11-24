# Turi

Lee `AGENT_MEMORY.md` (si existe localmente, ignorado en git), `ESTADO.md`, `SPEC.md` y `DECISIONES.md` al empezar. El contrato publicado está en `SPEC.md`. `AGENT_CARD.md` contiene el contexto breve para escribir programas Turi.

| Tarea | Entrada |
|---|---|
| Memoria local CI y portabilidad (Windows/Linux/Clang 21-23) | `AGENT_MEMORY.md` *(local, en `.gitignore` — leer antes de editar C++/IR/tests)* |
| Compilar y verificar Windows | `powershell -File tools/build.ps1` |
| Sintaxis, generación restringida y formato | `src/parser.cpp`, `src/lexer.hpp`, `src/format.cpp`, `grammar/` |
| Tipos, instanciación y ejecución de referencia | `src/semantics.cpp`, `src/generics.cpp` |
| Paquetes locales y grafo de módulos | `src/project.hpp`, `src/project.cpp`, `tests/project.py` |
| IR, LLVM y código de apoyo | `src/native_ir.hpp`, `src/native.cpp`, `src/native_runtime.hpp` |
| CLI, archivos y ediciones por símbolo | `src/main.cpp`, `src/agent.cpp`, `src/driver.cpp` |
| Pruebas ejecutables y diferencial | `examples/`, `tests/` |
| Librería estándar modular (`"std"`) | `lib/` (`web`, `gpu`, `tensor`, `crypto`, `data`, `cli`, `postgres`, `lienzo`) |
| MCP, LSP y comandos CLI (`new`/`bench`/`costs`) | `src/tooling.hpp`, `src/tooling.cpp`, `tests/mcp_lsp.py`, `tests/cli_tools.py` |
| Superoptimizador opcional (`--extreme`) | `tools/superopt/superopt.py`, `tests/extreme.py` |
