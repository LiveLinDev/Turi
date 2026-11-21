# Turi

Lee `ESTADO.md`, `SPEC.md` y `DECISIONES.md` al empezar. El contrato publicado está en `SPEC.md`. `EXPEDIENTE.md` fija la hoja de ruta y el protocolo de medición. `AGENT_CARD.md` contiene el contexto breve para escribir programas Turi.

| Tarea | Entrada |
|---|---|
| Compilar y verificar Windows | `powershell -File tools/build.ps1` |
| Sintaxis, generación restringida y formato | `src/parser.cpp`, `src/lexer.hpp`, `src/format.cpp`, `grammar/` |
| Tipos, instanciación y ejecución de referencia | `src/semantics.cpp`, `src/generics.cpp` |
| IR, LLVM y código de apoyo | `src/native_ir.hpp`, `src/native.cpp`, `src/native_runtime.hpp` |
| CLI, archivos y ediciones por símbolo | `src/main.cpp`, `src/agent.cpp`, `src/driver.cpp` |
| Pruebas ejecutables y diferencial | `examples/`, `tests/` |
| Evaluación con modelos locales | `tools/eval/` |
| Tarjeta mínima medida para funciones escalares | `tools/eval/paired/cards/turi.txt` (279 tokens de Qwen 2.5) |
| Rendimiento local | `bench/local/PROTOCOLO.md`, `bench/local/resultados/` |

Conserva en los informes los fallos, timeouts y entradas de reproducción. Mantén separados los resultados funcionales y las comparaciones de velocidad del expediente.
