# Piloto de un intento

El piloto original emplea la tarjeta y el núcleo anteriores al soporte de arreglos. El commit `c9d3a9f` contiene los fuentes, la tarjeta utilizada y los registros originales. El SHA-256 del ejecutable y de la tarjeta acompaña cada fila. Para reproducir la condición original se puede crear un worktree local con `git worktree add --detach build-piloto c9d3a9f` y compilar desde allí.

Cincuenta tareas de funciones puras ejercitan enteros, bucles, texto UTF-8, registros, variantes y selección. Cada tarea tiene un ejemplo visible, al menos dos casos ocultos y una solución de referencia. Los casos ocultos y las soluciones están publicados aquí para poder auditar la medición. El cliente envía al modelo únicamente la tarjeta, el enunciado, la firma, los tipos necesarios y el ejemplo visible.

El piloto llama a Ollama en `127.0.0.1:11434`, sin descargas, cuentas ni servicios externos. Compara dos tamaños de Qwen 2.5 que ya estaban instalados. La prueba multilenguaje y los modelos de familias distintas del hito 7 requieren un experimento aparte.

```powershell
python tools/eval/pilot.py verify build/veloz.exe
python tools/eval/pilot.py run build/veloz.exe qwen2.5:3b tools/eval/results/qwen3b.jsonl
python tools/eval/pilot.py run build/veloz.exe qwen2.5:7b tools/eval/results/qwen7b.jsonl
python tools/eval/pilot.py report tools/eval/results/qwen3b.jsonl tools/eval/results/qwen7b.jsonl
python tools/eval/pilot.py replay build/veloz.exe tools/eval/results/qwen3b.jsonl
```

Se ejecuta primero `verify` sobre una copia fija del compilador. El protocolo llama una vez al modelo por tarea, con temperatura cero, semilla 20261120, contexto de 4096 tokens y límite de salida de 384 tokens. Cada consulta dispone de 120 segundos. Una respuesta envuelta por completo en un único bloque Markdown pierde únicamente ese envoltorio. La respuesta original queda en el JSONL.

El evaluador añade el arnés al código y usa `veloz emit-ir` para medir si compila con la llamada pública. Si compila, ejecuta `veloz check` por separado con los casos públicos y los ocultos. Las suites ocultas agregan todos sus casos sin conceder crédito parcial. Cada proceso del compilador dispone de diez segundos. El paso de evaluación usa exclusivamente el compilador local de Turi.

Cada fila conserva la tarea, los identificadores del modelo y del protocolo, hashes SHA-256 del compilador, la tarjeta, la suite y el evaluador, condición, salida original, causa de parada, tiempos, diagnósticos y cuentas de tokens que devuelve Ollama. El servidor informa tokens de entrada y de generación, sin desglose separado de razonamiento. Un fallo de transporte se registra con el tiempo transcurrido y los tokens quedan como desconocidos. El resumen incluye todos los intentos y el número de intentos con cuentas de tokens.

El mismo comando reanuda el archivo después de la última fila escrita. Las filas con errores o timeouts cuentan como intentos completos. Ante un cierre abrupto, comprueba que el PID registrado en el archivo `.lock` terminó antes de retirar ese archivo. Si la última línea JSON quedó incompleta, conserva una copia y retira únicamente esa línea antes de reanudar. Un cambio de versión, opciones o hashes requiere un archivo nuevo. `replay` vuelve a calificar las respuestas guardadas sin volver a solicitar generación.

## Resultado del 20 de noviembre de 2026

| Modelo local | Compilaron al primer intento | Pasaron el ejemplo público | Pasaron los casos ocultos | Pasaron ambos | Tokens de entrada | Tokens de salida |
|---|---:|---:|---:|---:|---:|---:|
| Qwen 2.5 3B | 31/50 | 27/50 | 25/50 | 25/50 | 69.235 | 4.032 |
| Qwen 2.5 7B | 28/50 | 26/50 | 25/50 | 25/50 | 69.235 | 2.622 |

La mediana de inferencia local fue 0,531 s para 3B y 1,758 s para 7B. Los 100 intentos completaron la consulta y tienen conteos de tokens. Los registros guardados reprodujeron las 100 calificaciones al reevaluarlos con la misma versión del compilador.

| Primera causa observada | 3B | 7B |
|---|---:|---:|
| Sintaxis `E_PARSE` | 9 | 9 |
| Modificación de valor inmutable `E_IMMUTABLE` | 3 | 7 |
| Llamada desconocida `E_FUNCTION` | 2 | 3 |
| Carácter no admitido `E_CHAR` | 3 | 1 |
| Nombre inválido `E_NAME` | 1 | 2 |
| Punto y coma `E_SEMICOLON` | 1 | 0 |
| Aserción del ejemplo público | 4 | 2 |
| Aserción oculta tras pasar el ejemplo público | 2 | 1 |

El objetivo provisional del 90 % de compilación al primer intento sigue pendiente. La suite cubre funciones aisladas, con una sola generación por tarea. Estos resultados describen las 50 tareas y los dos tamaños de Qwen evaluados. La eficacia comparativa frente a otros lenguajes requiere sus propias mediciones con condiciones equiparadas.

Los archivos `results/qwen3b.jsonl` y `results/qwen7b.jsonl` publican cada intento completo. La tarjeta empleada puede recuperarse en Git en `23bfa53:AGENT_CARD.md`.
