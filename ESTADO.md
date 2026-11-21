# Estado · 20 de noviembre de 2026

| Hito | Estado comprobado |
|---|---|
| 1 · Esqueleto | Núcleo C++23, gramáticas Lark y GBNF, intérprete, pruebas y diagnósticos JSON. |
| 2 · Binario nativo | IR de bloques y LLVM. Funciones, bucles, structs, enums, match exhaustivo y funciones genéricas pasan por compilación nativa y comparación diferencial. |
| 3 · Bucle del agente | Tarjeta, corpus, sig, explain, types, patch y fmt. Piloto local de 50 tareas por modelo completado. |

Qwen 2.5 3B alcanzó 31/50 al primer intento de compilación a LLVM IR y Qwen 2.5 7B, 28/50. Cada modelo resolvió 25/50 incluyendo pruebas ocultas. Los totales de entrada y salida fueron 73.267 y 71.857 tokens. Las dos ejecuciones fueron de un intento, dentro de una misma familia de modelos. La comparación de tokens frente a Go y Rust y el experimento multivuelta quedan abiertos. Los registros completos están en `tools/eval/results`.

La compilación Windows x64 instrumentada con ASan y UBSan pasó cinco grupos de CTest. La suite semántica contiene 3.405 programas. Se realizaron 40.000 ejecuciones con mutación guiada por cobertura en dos configuraciones documentadas en `tests/SANITIZERS.md`.

El hito 4 está en curso. El protocolo para sus ocho cargas está en `bench/HITO4.md`. La máquina de desarrollo es un Ryzen 7 7700X con Windows. Se mantienen separados los ensayos locales y la comparación experimental completa.
