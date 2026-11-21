# Estado · 20 de noviembre de 2026

| Hito | Estado comprobado |
|---|---|
| 1 · Esqueleto | Núcleo C++23, gramáticas Lark y GBNF, intérprete, pruebas y diagnósticos JSON. |
| 2 · Binario nativo | IR de bloques y LLVM. Funciones, bucles, structs, enums, match exhaustivo y funciones genéricas pasan por compilación nativa y comparación diferencial. |
| 3 · Bucle del agente | Tarjeta, corpus, sig, explain, types, patch y fmt. Piloto local de 50 tareas por modelo completado. |
| 4 · Memoria, en curso | Arreglos de enteros con índices comprobados, semántica de valor, regiones y dueños únicos. `costes` muestra las decisiones del compilador. |
| 5–7 · Biblioteca, optimización y comparación amplia | Entrada binaria por byte y primera optimización de arreglos integradas. Falta la biblioteca completa y la suite competitiva del expediente. |

Qwen 2.5 3B alcanzó 31/50 al primer intento de compilación a LLVM IR y Qwen 2.5 7B, 28/50. Cada modelo resolvió 25/50 incluyendo pruebas ocultas. Los totales de entrada y salida fueron 73.267 y 71.857 tokens. Los registros de los dos modelos están en `tools/eval/results`. Las pruebas emparejadas con Go y Rust están en `tools/eval/paired/`. Su métrica es compilación estática, mientras que el primer piloto también evalúa el comportamiento.

Siete grupos de CTest pasaron en Windows x64, tanto en release como con el compilador instrumentado con ASan y UBSan. La suite semántica contiene 3.405 programas. Se realizaron 40.000 ejecuciones con mutación guiada por cobertura del núcleo previo a los arreglos. Véanse `tests/SANITIZERS.md` y `tests/MEMORIA.md`.

**Rendimiento local.** En el Ryzen 7 7700X se verificaron 693 ejecuciones en la serie integrada. Turi aventajó a C y Goose en el escalar y con arreglos temporales. En el recorrido de arreglo la diferencia fue inconclusa. Rust ganó en escalar. La prueba reservada para la eliminación del llenado dio 1,52× frente al compilador Turi anterior. Fuentes, datos y límites en `bench/local/RESULTADOS.md`.

**Siguiente cuello de botella.** Ampliar la propiedad de arreglos a campos de agregados, con destrucción en todos los caminos. Seguir con pools generacionales, recursos de ámbito y cargas de larga vida según `bench/HITO4.md`. El benchmark formal requiere las ocho implementaciones, sus controles de memoria y una máquina física adecuada al protocolo. La reducción de tokens hasta un programa funcional frente a Go y Rust sigue abierta.
