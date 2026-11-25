# Diseño de implementación: superar a C, no empatar

24 de noviembre de 2026. Este documento es la especificación de trabajo. El diagnóstico y las cifras medidas están en [TRES-PILARES.md](TRES-PILARES.md); aquí está **qué construir, en qué orden, en qué archivos y cómo se comprueba que quedó hecho**.

## Reglas para quien implemente

1. Cada cambio pasa `tools/build.ps1` completo (hoy 43 pruebas) sin avisos del compilador, y añade su propia prueba en `tests/`.
2. Ningún pase puede depender de la forma de un programa del banco. Se justifica con una propiedad general y se prueba con programas que no lo inspiraron.
3. Toda cifra sale de `python benchmarks/run.py` o de `python tools/aibench/run.py`. Tras cada bloque se regenera `benchmarks/RESULTS.md` y se publican también las derrotas.
4. El modo estricto (`--strict`) conserva los diagnósticos de la grafía canónica. Lo nuevo se documenta en `SPEC.md` y `DECISIONES.md`.
5. Leer `AGENT_MEMORY.md` antes de tocar C++, IR o pruebas (diferencias Windows/Linux y LLVM 21/23).
6. Al añadir un benchmark, escribir también la versión en C **con intrínsecos** además de la portable. El objetivo declarado es ganar a las dos.

## Estado del árbol al entregar este diseño

- Todo lo descrito como hecho está en el árbol de trabajo de `F:\Turi`, **sin commit**.
- Compilar y probar: `powershell -File tools/build.ps1 -BuildDir build-dev2`. El compilador del usuario (`build/`) no se ha tocado.
- Banco de velocidad y memoria: `python benchmarks/run.py --veloz build-dev2/veloz.exe --markdown benchmarks/RESULTS.md`.
- Banco de IA: `python tools/aibench/run.py --ssh spark --model nvidia/Qwen3.8-Flash-Next-NVFP4 --compiler build-dev2/veloz.exe --out tools/aibench/results/NOMBRE.jsonl` y `python tools/aibench/report.py ARCHIVO.jsonl`. El modelo de la Spark se comparte con producción: el arnés ya va de una petición en una y cede el paso.
- Variables de diagnóstico del compilador: `VELOZ_KEEP_STAGE=DIR` (conserva el IR y los runtimes), `VELOZ_CLANG_FLAGS` (opciones extra a clang), `VELOZ_STRICT=1`, `VELOZ_NO_STACK_ARRAYS=1`.
- Ya implementado de este documento: V1 para funciones (falta V1a, V1b y V1c), las mejoras generales listadas en `TRES-PILARES.md`, I1 (biblioteca base y tuplas), la primera mitad de M1 (tipos recursivos directos, lecturas prestadas, región por vuelta de bucle, liberación iterativa) y de M2 solo `m[k] op= d`. Todo lo demás está por hacer.
- Lista de verificación del proyecto (`AGENT_MEMORY.md`, local): 0 advertencias, `tools/build.ps1 -BuildDir build-san -Sanitize` al 100 % cuando se toca C++, IR o runtimes, y ningún símbolo de runtime nuevo en `veloz emit-ir examples/fibonacci.turi`.

## Por qué «empatar» no es el techo

La idea de que frente a un C con intrínsecos solo se puede empatar supone que ese C ya es el mejor código máquina posible. No lo es. Un programa en C es:

- **Estático.** Se escribe una vez y se compila una vez. No conoce los datos ni la máquina donde correrá.
- **Fijado por el programador.** El ancho de cada entero, la disposición de cada `struct` y el orden de cada suma los decide una persona de antemano y el compilador no puede tocarlos.
- **Compilado con prisa.** El compilador elige instrucciones, registros y orden con heurísticas rápidas. Nunca mide.
- **Atado a la ABI.** Convención de llamadas, alineación y relleno son los de la plataforma.

Turi puede romper las cuatro cosas. Hay precedentes publicados de que eso basta para superar código escrito a mano por expertos: la búsqueda estocástica de código máquina (STOKE), las rutinas de ordenación y hash halladas por búsqueda (AlphaDev, integradas en libc++), los generadores con búsqueda empírica (FFTW, ATLAS, SPIRAL) y la generación de núcleos en ejecución (libxsmm frente a BLAS estático en matrices pequeñas).

Las vías, de menos a más profundas:

| Vía | Qué hace | Por qué C no puede |
|---|---|---|
| **Saber más** | Usa lo que la semántica garantiza: no hay solapamiento, hay rangos, hay pureza, las acumulaciones flotantes se pueden agrupar | El estándar de C lo prohíbe o no lo puede demostrar |
| **Buscar más** | Compila muchas variantes de cada bucle caliente y se queda con la que el procesador real ejecuta más rápido | Nadie espera horas a un compilador de C; aquí el tiempo de compilación no importa |
| **Bajar más** | Reescribe el código máquina que deja LLVM: instrucciones, orden, registros, alineación | Los compiladores de C no lo hacen; los expertos solo lo hacen a mano en un puñado de rutinas |
| **Adaptarse** | Elige representación y código según los datos y la máquina reales, en ejecución | Un binario de C no cambia después de compilado |

El marcador decide. Cada programa del banco donde Turi no gane al mejor C que sepamos escribir es un defecto abierto, no un límite aceptado.

### Evidencia medida en esta máquina

| Experimento | Resultado |
|---|---|
| `spectralnorm` sin tocar su código, tras integrar funciones flotantes pequeñas y permitir agrupar la acumulación | 0,62× → **2,35×** frente a C experto portable |
| `mandelbrot` tras pasar a la pila los arreglos pequeños | 496 → 215 ms |
| Bucle con división por un valor fijo en ejecución, recíproco precalculado una vez | **1,86×** frente al C compilado con `-O3 -march=native` |
| `nbody` cambiando solo una opción del vectorizador | 8 % más rápido: la heurística por defecto no elige la mejor variante |

## Bloque V · Velocidad

### V1 · Una guarda fuera, cero comprobaciones dentro

**Estado:** hecho para funciones con parámetros enteros o arreglos, con o sin llamadas dentro (`optimizeIR`, `trampoline`; prueba en `tests/fast_paths.py`). Falta lo siguiente.

- **V1a · Bucles dentro de `main` y de funciones sin parámetros útiles.** Pase nuevo `outlineHotLoops` en `src/native_opt.hpp`, antes de crear las versiones rápidas: extrae cada bucle exterior a una función interna cuyos parámetros son las variables vivas a la entrada (por referencia las que el bucle escribe). Después se aplica el mecanismo existente sin cambios.
- **V1b · Umbral mínimo suficiente.** En vez de suponer siempre «enteros < 2²⁰, longitudes < 2³⁰», probar 2³¹, 2²⁶ y 2²⁰ y quedarse con la suposición más débil que elimine todas las comprobaciones. Hoy un contador de 20 millones cae en la versión lenta.
- **V1d · Quitar el tope de 2²⁴ elementos por arreglo.** Hoy `maxArrayLength = 1 << 24` (16,7 millones) es un límite del lenguaje y, a la vez, lo que permite al probador demostrar que `i * j` no desborda. Con la versión rápida con guarda esa cota pasa a ser la suposición de la vía rápida (`kFastArrayLenMax`) y el límite del lenguaje puede subir a 2⁴⁷ elementos. Un arreglo mayor que la suposición sigue funcionando, por la vía completa.
- **V1c · Llamadas dirigidas.** Dentro de una versión rápida, si el probador demuestra que los argumentos de una llamada cumplen la suposición de la función llamada, llamar directamente a su `.fast` sin pasar por el trampolín.

*Prueba:* ampliar `tests/fast_paths.py` (ya cubre enteros, arreglos por valor y por referencia, llamadas y recursión, dentro y fuera del rango supuesto) con bucles en `main`, contadores grandes y arreglos de más de 2²⁴ elementos.
*Objetivo:* `mandelbrot` ≥ 1,00× frente a C portable.

### V2 · División y módulo por un valor fijo del bucle

Pase nuevo `hoistInvariantDivisors`: cuando el divisor de `/` o `%` no cambia dentro del bucle, calcular antes del bucle el multiplicador (método de Lemire para 32 bits; Granlund-Montgomery para 64) y sustituir la división por multiplicaciones. Requiere dividendo y divisor no negativos, que el probador de rangos ya conoce; si no lo demuestra, se deja la división.

*Archivos:* `src/native_opt.hpp` (pase), `src/native.cpp` (bajada con `mul nuw` y `lshr` sobre `i128`).
*Prueba:* `tests/invariant_div.py`: todos los divisores de 1 a 1000 más potencias de dos y valores límite, contra el intérprete.
*Objetivo:* el experimento `divinv` reproducido en Turi con ≥ 1,7×.

### V3 · Contrato con el reservador de memoria

El compilador controla la reserva de todos los arreglos, así que puede garantizar lo que C no puede suponer de un puntero:

- **Alineación a 64 bytes** del primer elemento.
- **Relleno legible hasta el siguiente múltiplo de 64 bytes** después del último elemento.

Con eso son válidas tres cosas que LLVM no hace con código C:

1. **Vectorizar bucles con salida temprana** (`buscar`, `todos`, `alguno`, comparar, contar hasta): leer un vector entero más allá del elemento donde se sale nunca falla.
2. **Eliminar el bucle de resto**: la cola se procesa con el mismo código vectorial enmascarado.
3. **Cargas alineadas** sin bucle previo de ajuste.

*Archivos:* `src/native_arrays.hpp` (`tz.allocBuf`, `tz.clone`, crecimiento), `src/native.cpp` (atributos `align 64` y `dereferenceable` en la base de los arreglos; bajada vectorial explícita para bucles de salida temprana en un pase nuevo `vectorizeEarlyExitLoops`).
*Prueba:* `tests/alloc_contract.py` bajo ASan con arreglos de longitud 0 a 200; programas `find`, `equal`, `count_until`.
*Benchmark nuevo:* `benchmarks/game/search` (búsqueda lineal y comparación de arreglos) contra C con `memchr`/intrínsecos.

### V4 · Búsqueda empírica de variantes

Extiende `--extreme` (`tools/superopt/superopt.py`) y lo activa por defecto en `--release` cuando hay con qué entrenar.

1. **Entrenamiento.** Se ejecutan las pruebas `test` del propio programa y, si se dan, `--train ARGS`. Durante esa ejecución se **graban los argumentos** de las llamadas a las funciones calientes.
2. **Repetición aislada.** Cada función caliente se cronometra por separado repitiendo sus argumentos grabados miles de veces (`rdtsc`, afinidad a un núcleo, mínimo de N).
3. **Espacio de variantes por función**, no global: ancho de vector (128/256/512), entrelazado (1/2/4/8), desenrollado, SLP sí o no y su ancho máximo, conversión de `if`, distribución y fusión de bucles, distancia de prebúsqueda, escrituras sin caché para rellenos grandes, alineación del bucle (16/32/64).
4. **Descenso por coordenadas** con presupuesto de tiempo; el resultado se guarda en caché por huella de la función y modelo de procesador.
5. **PGO siempre**: el perfil del entrenamiento se usa en la compilación final.

*Archivos:* `tools/superopt/superopt.py`, `src/driver.cpp` (una unidad de compilación por función caliente para poder variar opciones), `src/native.cpp` (metadatos `llvm.loop.*` por bucle).
*Prueba:* `tests/extreme.py` ampliado: la variante elegida nunca es más lenta que la base en el entrenamiento y la salida es idéntica.
*Objetivo:* `nbody` ≥ 1,05× frente a C portable.

### V5 · Superoptimizador de código máquina (por debajo de LLVM)

Trabaja sobre el ensamblador que LLVM emite para el cuerpo de cada bucle interior caliente.

**Estado (25 de noviembre de 2026):** hecho el paso 0, un auditor (`tools/mcaudit/mcaudit.py`) que lee el desensamblado del ejecutable y señala trampas del procesador que LLVM no modela. Primera regla: lecturas que no se pueden reenviar desde escrituras recientes (bytes de dos escrituras distintas, o lectura más ancha que la escritura). Esa trampa era la causa de que `kmers` perdiera 3 a 1; arreglada en el runtime. Siguientes reglas del auditor, por orden: llamadas no integradas dentro de bucles interiores, dependencias falsas de registro (`popcnt`/`lzcnt`/`tzcnt`, conversiones a flotante sin limpiar el registro destino), saltos difíciles de predecir sustituibles por `cmov`, bucles que no caben en la caché de microoperaciones y cruces de línea de 64 bytes en el salto de vuelta. Cada regla que encuentre algo en el banco se convierte en una corrección general del generador, y después en una reescritura verificada de los pasos 1 a 3.

1. **Reglas locales verificadas.** Base de reescrituras de secuencias cortas (2 a 6 instrucciones), cada una demostrada equivalente para todas las entradas con un demostrador de vectores de bits (Z3, solo en tiempo de compilación). Incluye lo que los compiladores suelen dejar: romper dependencias falsas antes de conversiones, reutilizar banderas, fusionar comparación y salto, modos de direccionamiento que no deslaminan, `lea` frente a sumas encadenadas.
2. **Búsqueda estocástica.** Mutaciones aleatorias (sustituir, intercambiar, borrar instrucciones) aceptadas según un coste que combina fallos en vectores de prueba y **ciclos medidos** en el procesador. Las candidatas que pasan las pruebas van al demostrador; solo las demostradas se aceptan.
3. **Planificación y colocación.** Reordenar instrucciones independientes para equilibrar puertos de ejecución, alinear el bucle según lo que mida mejor y mantenerlo dentro de la caché de microoperaciones.
4. **Coma flotante.** Solo transformaciones exactas en IEEE-754, salvo en las operaciones que Turi marca como reagrupables.
5. **Salida.** El cuerpo optimizado sustituye al de LLVM como ensamblador; si algo falla, queda el de LLVM.

*Archivos nuevos:* `tools/superopt/asm/` (analizador de ensamblador x86-64, semántica de instrucciones para Z3, búsqueda, medidor). ARM64 después.
*Prueba:* `tests/asm_superopt.py`: toda reescritura aceptada lleva su prueba; un proponente hostil (reglas falsas a propósito) no consigue pasar ninguna.
*Objetivo:* nunca más lento que LLVM en ningún bucle del banco y mejora medible en al menos la mitad.

### V5b · Entrelazar vueltas independientes de un bucle limitado por latencia

Medido en `mandelbrot`: el bucle interior (50 vueltas de 5 instrucciones vectoriales) no está limitado por el número de instrucciones sino por la cadena `zr -> zi -> zr` (9 ciclos por vuelta); el procesador podría llevar dos o tres cadenas a la vez. Las vueltas del bucle exterior son independientes: los arreglos `cr`, `zr`, `zi` se escriben enteros al empezar cada una. Turi lo puede demostrar (son locales con semántica de valor y ya existe el análisis de «primera escritura completa»); un compilador de C no, porque el programador los declaró fuera del bucle. Transformación: procesar dos (o cuatro) vueltas exteriores a la vez con copias privadas de los arreglos y los bucles interiores fundidos, más un resto con el bucle original. Ganancia esperada en `mandelbrot`: cerca de 1,7× sobre C. Es el mismo tipo de análisis que necesita V10 (paralelismo automático).

*Archivos:* `src/native_opt.hpp` (dependencias entre vueltas, privatización), `src/native_ir.hpp`.
*Prueba:* salida idéntica bit a bit con y sin la transformación en programas con y sin dependencias entre vueltas.

### V6 · SIMD por clases de algoritmo

LLVM no vectoriza estas clases; se bajan con código vectorial propio desde operaciones de la biblioteca (bloque I1), no desde formas de bucle:

| Operación de biblioteca | Técnica |
|---|---|
| `filter`, `partition` | compresión con máscara (`vpcompress`) |
| `sort` | quicksort vectorial con compresión y redes de ordenación para tramos pequeños |
| `countBy`, histogramas | detección de conflictos (`vpconflict`) o tablas partidas |
| `scan` (suma acumulada) | prefijo en registro |
| `find`, `contains`, `indexOf`, `split` | máscara + recuento de ceros (usa V3) |
| `parseInt`, `str(n)` | conversión por lotes |
| `Map`, `Set` | sondeo por grupos de 16 con comparación vectorial |

Cada rutina tiene varias variantes y V4 elige la mejor en la máquina.

*Archivos:* `src/native_collections_rt.hpp`, `lib/core/`.
*Benchmarks nuevos:* `sort`, `filter`, `wordfreq`, contra C con `qsort`, contra C++ `std::sort` y contra C con intrínsecos.

### V7 · Representación adaptativa de datos

- **Ancho de elemento dinámico.** Un `[int]` se guarda físicamente con 8, 16, 32 o 64 bits según los valores que contiene. La cabecera del arreglo lleva el ancho; al escribir un valor que no cabe se promueve. Los bucles se generan una vez por ancho y se elige al entrar. En cargas limitadas por memoria caben de 2 a 8 veces más elementos por vector y por línea de caché. El experto en C solo puede hacerlo si conoce el rango al diseñar; Turi lo hace con el rango real.
- **Estructura de arreglos automática.** `[Struct]` que no escapa por puntero se guarda por campos cuando el perfil muestra recorridos por campo.
- **Campos fríos aparte** e **índices de 32 bits** en lugar de punteros de 64 en estructuras enlazadas.

Es el cambio más profundo del bloque: se hace después de V1 a V6 y por etapas, empezando por el ancho de `[int]` creado desde entradas cuyo máximo se conoce al crearlo.

*Prueba:* diferencial contra el intérprete con valores en cada frontera de ancho y promociones a mitad de bucle.

### V8 · Especialización en ejecución

Para bucles cuyos invariantes solo se conocen al correr (tamaños pequeños, pasos, parámetros de configuración):

- **Plantillas con huecos.** En compilación, LLVM genera el cuerpo del bucle como plantilla de código máquina con huecos para las constantes. En ejecución se copian y se rellenan (técnica de copiar y parchear). No hay LLVM dentro del binario y el coste añadido es de pocos kilobytes, solo en programas que lo usan.
- **Selección en línea.** El binario lleva dos o tres variantes de un bucle caliente; las primeras llamadas las cronometran y el resto usa la más rápida para ese tamaño de datos en esa máquina.

*Archivos:* `src/native_spec_rt.hpp` (nuevo), `src/driver.cpp`.
*Objetivo:* `nbody` con el número de cuerpos leído en ejecución tan rápido como con el número fijo al compilar.

### V9 · ABI y colocación propias

- Convención de llamadas interna sin registros preservados por el llamado y con retorno múltiple en registros (`preserve_none`, `tailcc`).
- Sin `vzeroupper`: todo el programa es código propio.
- Sin tablas de desenrollado ni protector de pila: un error de Turi termina el proceso.
- Reordenación de funciones y bloques por perfil, y separación de código frío, en el enlazado.
- Páginas grandes para el código y para el montón cuando el sistema las ofrece.

*Archivos:* `src/native.cpp`, `src/driver.cpp`.

### V10 · Paralelismo automático

Un bucle exterior se reparte entre hilos cuando el compilador demuestra que sus iteraciones son independientes: escrituras a índices disjuntos, lecturas de datos que no cambian y acumulaciones reagrupables. El reparto usa el planificador con robo de trabajo que ya existe. La decisión de paralelizar o no para un tamaño dado la toma la selección en línea de V8.

*Archivos:* `src/native_opt.hpp` (`autoParallelizeOuterLoops`), `src/native_conc_rt.hpp`.
*Prueba:* mismos resultados con `TURI_THREADS=1` y con todos los hilos; ThreadSanitizer limpio.

### Asignación por programa del banco

| Programa | Hoy frente a C portable | Qué lo lleva por encima del C con intrínsecos |
|---|---:|---|
| spectralnorm | 2,46× | V5, V10 |
| fannkuch | 1,07× | V7 (permutación de 16 bytes en un registro), V8 |
| nbody | 0,83× | V4, V5, V8 |
| mandelbrot | 0,89× | V1a, V5, V10 |
| kmers | 0,84× (era 0,37×) | M2: prefetch con adelanto y mapa de entradas densas; V6 (tabla con sondeo vectorial) |
| binarytrees | 0,38× a 0,40× (era 0,14×) | M1 puntos 1 a 4: opcional de celda en 8 bytes, celdas sin contador, región por raíz y enlaces de 32 bits |

## Bloque I · IA

### I1 · Biblioteca de cadenas y arreglos

**Estado:** primera versión hecha en `src/prelude.hpp` (fuente Turi inyectada solo si se usa) con UFCS general y mapas con corchetes; prueba en `tests/core_lib.py`. Falta de la lista siguiente: `toInt`, `padLeft`, `sortBy`, `unique`, `first`, `last`, iterar un `str` con `for`, igualdad estructural y `match` con literales. `sum`, `max`, `min` e `indexOf` solo cubren `[int]` (y `[float]`, `[str]` donde se indica); hay que hacerlos genéricos cuando los genéricos admitan comparación.

Escrita en Turi e importada sin declarar nada. Nombres iguales a los de Python y JavaScript, porque son los que el modelo ya conoce.

- **`str`:** `split(sep)`, `join(sep)` sobre `[str]`, `trim()`, `upper()`, `lower()`, `replace(a, b)`, `repeat(n)`, `reverse()`, `chars()` → `[str]`, `bytes()` → `[int]`, `toInt()` → `int?`, `find(t)`, `count(t)`, `padLeft(n, c)`, `isDigit()`, `isAlpha()`. Iteración `for c in s` (cada `c` es un `str` de un carácter).
- **`[T]`:** `sum()`, `min()`, `max()`, `map(f)`, `filter(f)`, `any(f)`, `all(f)`, `count(f)`, `indexOf(x)`, `contains(x)`, `reverse()`, `concat(b)`, `sortBy(f)`, `unique()`, `first()`, `last()`.
- **Igualdad estructural** `==` y `!=` para `struct`, arreglos y opcionales.
- **`match` con literales** enteros y de cadena, y brazo `_`.

*Prueba:* `tests/core_lib.py`, una tabla de casos por función en intérprete y nativo.
*Objetivo medido:* mediana de tokens de la solución correcta ≤ la de Go en `tools/aibench` (**alcanzado: 66 frente a 72**), y después ≤ la de Python (36).

### I0 · Sintaxis de Python como cara principal (prioridad del bloque)

**Estado (25 de noviembre de 2026):** medido con un traductor de prueba (`tools/pyfront/py2turi.py`, adaptador `turipy` del banco). Resultado: 59 de 60 a la primera con 37 tokens por solución y 131 de encargo, frente a 59, 68 y 1.133 con la sintaxis propia. Ver `DECISIONES.md`, Turi 0.9.2.

Trabajo para llevarlo al compilador:

1. **Lector de Python en `src/parser.cpp`**: `def`, `:` y sangría, `elif`, `for x in`, `range`, comprensiones, cortes, `in`, `not`, cadenas f, tuplas. Produce el mismo árbol que la sintaxis actual; se elige por extensión de archivo o por detección.
2. **Declaración por primera asignación** y ámbito de función (hoy lo hace el traductor): en el comprobador, con deducción de tipos para `[]`, `{}` y `set()` a partir del primer uso.
3. **Semántica que el modelo da por hecha:** `//` y `%` de Python, índices negativos, cortes sin error fuera de rango, `s[i]` como cadena, orden de inserción en diccionarios (enlaza con el mapa de entradas densas de M2), parámetros de lista modificados visibles para quien llama (`mut` deducido en la llamada, M3).
4. **Lo que no se copia:** tipos dinámicos, `eval`, herencia dinámica, excepciones como control de flujo. Se rechazan con un mensaje corto y un arreglo sugerido; nunca se explican por adelantado en el encargo (una tarjeta de avisos subió la solución de 36 a 66 tokens).
5. **Enteros:** decidir entre desbordamiento detectado (hoy) y enteros grandes automáticos cuando el compilador no pueda acotar el valor.
6. **Biblioteca:** `re`, `math`, `collections` (`Counter`, `defaultdict`, `deque`), `itertools` básicos, `sorted(key=...)`, escritos en Turi.
7. **Medición:** repetir `turipy` con un conjunto público (MultiPL-E/HumanEval) y más modelos, y usar `tools/pyfront/replay.py` como prueba diferencial del compilador (ya encontró un fallo de compilación).

### I2 · Tarjeta nueva

Una sola fuente (`docs/card.md`) de la que se generan `AGENT_CARD.md` y la tarjeta de `tools/aibench/langs.py`. En inglés, ≤ 700 tokens, con la forma más corta de cada cosa. `tests/agent.py` compila todos sus ejemplos.

### I3 · Inferencia que evita anotar

`var v = []` toma el tipo del primer uso; `mapNew()` y `setNew()` sin anotación; literales vacíos como argumento toman el tipo del parámetro.

### I4 · Diagnósticos en inglés con arreglo incluido

Mensaje, ejemplo y edición sugerida en inglés por defecto (`--lang es` conserva el español). Todo diagnóstico de nombre desconocido propone el nombre más cercano.

### I5 · Medición

- Importar MultiPL-E/HumanEval como tareas de `tools/aibench` (el arnés ya separa tareas de adaptadores). Requiere permiso para descargar el conjunto.
- Adaptador de Goose como control de lenguaje no visto.
- Al menos dos familias de modelos y tres repeticiones con semillas distintas.
- Informe por categoría de error.

## Bloque M · Memoria

### M1 · Tipos recursivos directos con región por raíz

**Estado (25 de noviembre de 2026):** hecha la primera mitad. `struct Node { left: Node?, right: Node? }`, listas y enumeraciones recursivas compilan y funcionan en el intérprete y en nativo (`tests/recursive_types.py`). Lo que hay:

- El campo que cierra el ciclo se guarda en una celda (`boxRecursiveFields` en `src/semantics.cpp`); el programador nunca escribe la caja. Conversiones automáticas `T` ↔ celda y `T?` ↔ celda opcional.
- Reservador de celdas de tamaño fijo con liberación con tamaño (clases de 8 bytes hasta 256), sin cabecera de `malloc`.
- Lecturas prestadas: leer un campo a través de una celda, pasar el contenido de una celda como argumento y hacer `match` sobre un sitio que no cambia no copian ni cuentan referencias (`sharedPeek`, `stablePlace` en `src/native_ir.hpp`). Solo cuando el programa no modifica en su sitio celdas de ese tipo (`Program::mutatedCells`).
- **Región por vuelta de bucle** (`cellRegionFits`): si lo que se crea dentro de una vuelta no puede seguir vivo fuera, las celdas salen de una región (incremento de puntero) y la vuelta la suelta entera sin recorrer nada. Condiciones comprobadas: el cuerpo no nombra variables de fuera que puedan contener celdas, todo lo que ejecuta está en la lista de operaciones seguras (`cellRegionBuiltin`), y no puede devolver celdas con `return` o `?`.
- Liberación iterativa: soltar una lista de 5·10⁶ nodos no consume pila (lista de pendientes enlazada por el propio contador de la celda).
- La región reutiliza su basura (lo que muere dentro vuelve a una lista propia de la región) y también se abre para una sentencia suelta (`print(check(build(n)))`).
- El resultado de una función que se guarda en una celda se construye directamente en ella (variante `f.cell`).
- Las celdas con contenido constante todo ceros (las hojas) son una sola celda estática compartida.

Lo que falta para llegar al objetivo (≥ 1,00× frente a C con arena), por orden de retorno:

1. **Opcional de celda en 8 bytes.** `Node?` en celda ocupa hoy 16 bytes (etiqueta + puntero); con el puntero nulo como `None` el nodo pasa de 32 a 16 bytes, vuelve por registros en vez de por memoria y `check` hace la mitad de cargas. Toca `src/layout.hpp` y los ~40 sitios de `src/native.cpp` que leen la etiqueta.
2. **Celdas sin contador** para tipos cuyas celdas nunca se clonan en todo el programa (dueño único demostrado): 8 bytes menos por nodo y una escritura menos. Con el punto anterior el nodo queda en 16 bytes, igual que el de C.
3. **Región por raíz** para árboles guardados en una variable que vive más que una sentencia o una vuelta (el árbol largo de `binarytrees`): hoy usan celdas normales y se liberan nodo a nodo. Caso fácil pendiente: no soltar la memoria de las variables de `main` al terminar el programa.
4. **Enlaces de 32 bits** dentro de una región (nodo de 8 bytes): aquí es donde se pasa por delante del C con punteros.
5. **Pasar el nodo por puntero** a funciones que solo lo leen (`check(n: Node)` recibe hoy 32 bytes por valor: cuatro cargas por hijo en vez de dos).
6. **Modificación en sitio a través de celdas** (`t.left.key = 3`): hoy un árbol se actualiza reconstruyendo el camino (el compilador reutiliza la celda si es el único dueño).
7. Bucles que leen un árbol de fuera y crean temporales dentro: hoy no abren región (regla conservadora).

Diseño original del apartado:

- **Representación.** Un campo que cierra un ciclo de tipos se guarda como puntero a una caja.
- **Dueño.** Cada valor raíz (la variable local, el campo o el elemento que contiene el árbol) posee una región. Todas las cajas alcanzables desde la raíz se reservan en su región con un incremento de puntero.
- **Liberación.** Al morir la raíz se libera la región entera en una operación. No hay recorrido ni liberación nodo a nodo.
- **Mover un subárbol a otra raíz** lo copia a la región de destino. Si el análisis ve que un programa lo hace dentro de un bucle, usa cajas individuales para ese tipo.
- **Ciclos reales** (un nodo alcanzable por dos caminos) siguen necesitando `Pool`/`Handle`; el compilador lo dice en el diagnóstico solo cuando el programa intenta construir uno.

No es el modelo de Rust (no hay préstamos ni tiempos de vida escritos), ni el de C++ (no hay punteros inteligentes), ni el de Go (no hay recolector).

*Archivos:* `src/semantics.cpp` (aceptar el ciclo por `T?`), `src/layout.hpp`, `src/native_ir.hpp`, `src/native.cpp`, intérprete.
*Prueba:* `tests/recursive_types.py`: lista, árbol, árbol de expresiones; ASan sin fugas; liberación de una lista de 10⁶ nodos sin desbordar la pila.
*Objetivo:* `binarytrees` escrito sin `Pool` ni `Handle`, 0 marcas de memoria, ≥ 1,00× frente a C con arena.

### M2 · Lecturas sin copia

**Estado:** hecho `m[k] op= d` con una sola búsqueda, el LTO del runtime y el hash por campos de las cadenas cortas (`kmers` de 0,36× a 0,84×; ver `DECISIONES.md`). Falta todo lo demás de este apartado, y dos cosas nuevas que salen de medir `kmers`:

- **Prefetch con adelanto en bucles de mapa (hecho el 25 de noviembre para claves `int` aritméticas y cortes de `str`; falta `m[a[i]]` y claves compuestas).** En `for i in a..b { m[clave(i)] op= d }` con `clave` sin efectos, calcular el hash de `clave(i + D)` y pedir sus líneas de caché D vueltas antes. C no puede hacerlo solo (no sabe qué es una tabla hash). Condición de seguridad que hay que respetar: la clave adelantada se evalúa **sin poder fallar** (índices y cortes fuera de rango se descartan en vez de abortar), porque la vuelta `i + D` puede no llegar a ejecutarse (un `break`, un error anterior). Solo se activa con tablas que no caben en la caché L2.
- **Mapa con entradas densas en orden de inserción** (índice pequeño + arreglo `{clave, valor}` contiguo). Una búsqueda toca una línea de caché de datos en vez de dos o tres, recorrer `keys()`/`values()` es lineal, y el orden de recorrido pasa a ser el de inserción, que es lo que un modelo entrenado con Python o JavaScript da por hecho (pilar de IA: hoy ese supuesto falla en silencio). Toca el runtime C, los auxiliares en IR y el espejo del intérprete.

- `s[a..b]` es una vista (puntero y longitud) mientras el dueño viva y no cambie; se copia solo si escapa.
- `m.get(k)`, `poolGet`, `sharedGet` y `match` sobre un valor prestado dan acceso sin copia.
- Actualización en sitio: `m[k] += 1`, `m.update(k, f)`.

*Prueba:* `tests/copy_audit.py` ampliado a cadenas y mapas.
*Objetivo:* `kmers` ≥ 1,00× frente a C++ y Rust, y después frente a C.

### M3 · `mut` inferido en la llamada

`f(v)` basta cuando el parámetro es `mut`; `fmt` añade `mut` en la forma canónica. Las reglas de exclusividad (`E_ALIAS_MUT`) no cambian.

### M4 · Guarda de pila en lugar del tope de 256 llamadas

Cada función compara su dirección de marco con el límite de pila del hilo. El hilo principal arranca con 256 MiB de pila reservada. `R_DEPTH` se conserva como error cuando se alcanza el límite real.

*Archivos:* `src/native.cpp` (prólogo), `src/native_runtime.hpp`, `src/driver.cpp` (tamaño de pila al enlazar), intérprete.
*Prueba:* recursión de 10⁶ niveles; recursión infinita termina con `R_DEPTH` y no con un fallo del sistema.

### M5 · Destructores

`impl Tipo { fn drop(mut self) { ... } }` se ejecuta cuando el valor muere. Las bibliotecas dejan de exponer `free` y `MutPtr`.

### M6 · Medición

Añadir a `benchmarks/game`: lista enlazada de 10⁶ nodos, grafo con ciclos y borrado, análisis de un JSON grande y un servidor bajo carga durante 10 minutos. Script `benchmarks/memmarks.py` que cuenta las marcas de memoria por lenguaje con las mismas reglas.

## Bloque P · Profesional y autoalojado

| Paso | Contenido | Hecho cuando |
|---|---|---|
| P1 | Versión única `0.9.0` en `CMakeLists.txt`, `SPEC.md`, README y paquetes; `CHANGELOG.md`; etiqueta | `veloz --version` y los documentos coinciden |
| P2 | Información de depuración (DWARF y CodeView) desde los `Span` | Un depurador para en la línea correcta |
| P3 | Documentación en inglés: guía, referencia y biblioteca | Publicada desde `docs/` |
| P4 | CI en macOS y Linux ARM64; `--target` | Los tres sistemas en verde |
| P5 | Partir `native_opt.hpp` y `semantics.cpp` por pase | Ningún archivo supera 5.000 líneas |
| P6 | Mapas con orden de inserción | Dos compilaciones dan el mismo binario |
| P7 | Proceso con lista de argumentos, directorios y rutas | El driver puede escribirse en Turi |

**Compilador en Turi** (necesita M1, M4, I1 y P6):

1. `veloz tokens` y `veloz ast --json` en el compilador actual, para tener con qué comparar.
2. `selfhost/lexer.turi`: mismos tokens que el C++ en todo el corpus de pruebas.
3. `selfhost/parser.turi`: mismo árbol.
4. `selfhost/check.turi`: mismos diagnósticos (identificador y posición).
5. `selfhost/lower.turi`: mismo IR de texto.
6. Punto fijo: la etapa 1 se compila a sí misma dos veces y el resultado es idéntico.

## Orden

1. **V1a, V1b, V2, V3** — quitan lo que queda de la desventaja frente a C portable.
2. **Resto de M1, M2, M4** — memoria. La mayor derrota vuelve a ser `binarytrees`: disposición del nodo (M1, puntos 1 y 2). Después el mapa de entradas densas con prefetch (M2) y la guarda de pila.
3. **I1, I2, I3** — tokens y errores; medir con `tools/aibench`.
4. **V4, V5** — búsqueda empírica y superoptimizador: aquí se pasa del empate a la ventaja en bucles aislados.
5. **V6, V8, V9, V10** — biblioteca vectorial, especialización en ejecución, ABI propia y paralelismo.
6. **V7** — representación adaptativa.
7. **P1 a P7** y el compilador en Turi, en paralelo desde el paso 2.
