# Piloto emparejado de compilación estática

La pregunta es cuántos tokens emplea Qwen 2.5 7B para obtener una función escalar que el compilador acepte en Turi, Go y Rust. La etiqueta de éxito exige el tipo de la función solicitada. Una implementación que siempre devuelve cero puede cumplir este criterio. Los casos ocultos del piloto original quedan fuera de las solicitudes y de la calificación.

## Protocolo fijado antes de las muestras

- Doce tareas del corpus original, con aritmética, condiciones, booleanos y bucles. El archivo tasks.json fija el texto, un ejemplo público, los parámetros y el resultado. Las treinta y seis combinaciones vieron el mismo enunciado y ejemplo por tarea. Solo se tradujeron el nombre del lenguaje y la firma.
- Condición sin tarjeta, documentación adicional ni búsquedas. Se pide una sola función escalar sin auxiliares, imports, macros, directivas ni E/S.
- Qwen 2.5 7B local en Ollama. Temperatura 0, semilla 20261120, contexto de 4096 tokens, salida máxima de 384. Se rota el primer lenguaje de cada tarea entre Turi, Go y Rust. Todas las consultas son secuenciales.
- Se mide el primer intento y el acumulado hasta que compile, con un máximo de tres intentos por combinación. Cada reparación recibe de nuevo el enunciado, la respuesta inmediatamente anterior y el primer diagnóstico resumido a 200 caracteres. El límite de transporte es 120 segundos y el de compilación, diez segundos.
- Cada solicitud conserva mensajes, respuesta íntegra, causa de parada, primer diagnóstico y tokens de entrada y salida reportados por Ollama. Las sumas incluyen reparaciones y tareas fallidas. El servidor no separa tokens de razonamiento.
- Un único bloque Markdown que envuelva toda la respuesta pierde sus cercas. La validación del contrato rechaza contenido ajeno a la función solicitada, como módulos, macros y directivas. Los diagnósticos de esa etapa se distinguen de los del compilador. Los errores de infraestructura se registran y cierran la celda sin reintentar.
- Cada fuente se compila en un directorio temporal independiente. Turi usa emisión de LLVM IR con una función que verifica el contrato. Go usa directamente el compilador del toolchain, sin módulos, ensamblador, generación ni enlazado. Rust emite exclusivamente metadatos de una biblioteca sin Cargo. Los tres verifican los tipos del contrato. Ningún candidato se ejecuta.

Las tareas proceden de un corpus conocido y se usa un modelo por celda, con generación determinista. La evidencia se limita a esta selección y a la compilación. El protocolo de ejecución con pruebas ocultas queda pendiente para una comparación funcional.

## Reproducción

La ejecución original usó el compilador Turi congelado en build/pilot-baseline.exe. El manifiesto registra los hashes SHA-256 y versiones de los tres compiladores, el modelo, su digest, la versión de Ollama y las huellas del arnés y del conjunto de tareas. El registro JSONL conserva un intento por línea.

~~~powershell
python tools/eval/paired/pilot.py verify
python tools/eval/paired/pilot.py run
python tools/eval/paired/pilot.py replay
python tools/eval/paired/pilot.py report
~~~

El comando verify comprueba para cada tarea y lenguaje que la firma adecuada compila y que un nombre equivocado es rechazado. replay recalifica las respuestas guardadas sin usar Ollama. Para elegir el binario congelado en otra ubicación, usa --turi RUTA. El archivo run.lock evita dos escritores. Un manifiesto diferente requiere otra copia del directorio para conservar el registro original.

En el entorno de medición Rust tenía únicamente el destino x86_64-pc-windows-msvc. El destino WASI del compilador Go instalado requiere un runtime con importaciones. La comparación funcional homogénea mediante módulos WebAssembly puros queda sin verificar en este entorno.

## Resultados del 20 de noviembre de 2026

Las 67 solicitudes terminaron con conteos completos de entrada y salida. La recalificación local de las respuestas guardadas reprodujo los 67 resultados. Cada lenguaje recibió las mismas 12 tareas.

| Lenguaje | Compilan al primer intento | Compilan con hasta 3 | Intentos | Entrada inicial | Salida inicial | Entrada acumulada | Salida acumulada | Total acumulado |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Turi | 0/12 | 0/12 | 36 | 1.294 | 464 | 5.857 | 1.402 | 7.259 |
| Go | 9/12 | 10/12 | 17 | 1.300 | 635 | 2.473 | 1.142 | 3.615 |
| Rust | 11/12 | 11/12 | 14 | 1.330 | 749 | 2.123 | 1.239 | 3.362 |

- En Turi, las 12 respuestas iniciales empezaron por `def` y usaron sintaxis de Python. De 36 intentos, 34 fallaron la forma de la función solicitada. Las dos reparaciones restantes definieron `doubleValue` con `fn` y dejaron una expresión final sin `return`, que el parser rechazó.
- En Go, los tres fallos iniciales mezclaron `int` e `int64`. Una reparación de `digitSum` compiló. Cuatro respuestas de reparación para las otras dos tareas incumplieron el formato pedido.
- En Rust, las tres respuestas de `greatest` incumplieron el contrato de devolver exclusivamente la función.
- La versión Go de `greatest` ilustra la diferencia entre las métricas. Compiló, pero al inspeccionarla se observa que, para `a > 0` y `b = 0`, resta cero indefinidamente. Los resultados de la tabla cuentan únicamente compilación.

Los totales incluyen todas las salidas y todas las entradas repetidas durante reparaciones. Los 7.259 tokens de Turi corresponden a intentos que agotaron el límite. Este piloto sin tarjeta y el piloto original con tarjeta tienen distintas tareas y protocolos. Una comparación causal del efecto de la tarjeta requiere aplicar ambas condiciones a las mismas tareas.
