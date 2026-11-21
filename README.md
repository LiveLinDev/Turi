# Turi

Turi compila un programa completo a instrucciones de máquina. `veloz` es su compilador y su intérprete de referencia.

```powershell
.\tools\build.ps1
.\build\veloz.exe check examples\pedidos.turi
.\build\veloz.exe run examples\pedidos.turi
.\build\veloz.exe build examples\pedidos.turi -o build\pedidos.exe
.\build\pedidos.exe
```

Las últimas dos ejecuciones de `pedidos` imprimen `75`. En Windows, el script emplea LLVM 23.1.x, las herramientas C++ y SDK de Visual Studio, CMake, Ninja y Python 3.10 o posterior. Para elegir el driver de LLVM al enlazar un programa usa `VELOZ_CLANG`. El ejecutable generado funciona sin Python, LLVM ni un servicio de IA.

El proyecto CMake también admite una compilación desde una terminal con un compilador C++23 y Clang en el PATH.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

| Documento | Contenido |
|---|---|
| [Tarjeta del lenguaje](AGENT_CARD.md) | Sintaxis, ejemplos y diferencias respecto a Rust y TypeScript |
| [Especificación](SPEC.md) | Semántica que implementa el compilador actual |
| [Estado](ESTADO.md) | Hitos, resultados comprobados y siguientes pruebas |
| [Decisiones](DECISIONES.md) | Elecciones de implementación y límites |
| [Expediente](EXPEDIENTE.md) | Diseño y protocolo de comparación |
| [Piloto de agentes](tools/eval/README.md) | Evaluador local, tareas y respuestas reproducibles |
| [Coste en tokens frente a Go y Rust](tools/eval/paired/cards/README.md) | 12 tareas emparejadas, tarjetas breves y reparaciones |
| [Ensayo local de velocidad](bench/local/RESULTADOS.md) | Turi, C, Goose, Rust y Go en tres cargas con entradas dinámicas |
| [100 programas en Ryzen y DGX Spark](bench/amplia/RESULTADOS.md) | Seis lenguajes, tiempos por programa, memoria y coste del modelo hasta superar pruebas privadas |
| [Mediciones de memoria](tests/MEMORIA.md) | Reservas y pico residente en bucles y retornos tempranos |

La cadena de compilación es `Turi → IR de bloques tipados → LLVM IR → ejecutable`. El intérprete de referencia usa una pila de evaluación independiente. `veloz emit-ir archivo.turi` permite inspeccionar el módulo antes de la optimización.
