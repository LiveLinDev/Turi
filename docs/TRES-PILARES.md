# Los tres pilares de Turi: diagnóstico, medición y plan

24 de noviembre de 2026. Este documento sustituye a las cifras de portada: explica por qué no convencían, deja un método de medición que cualquiera puede repetir y registra lo que ya se cambió en el compilador. Qué construir a continuación, con archivos, pruebas y objetivos, está en [DISENO.md](DISENO.md).

Los tres pilares son los del proyecto y no se mezclan:

1. **Velocidad.** Más rápido que un C escrito por un experto.
2. **IA.** La IA comete menos errores en Turi que en otros lenguajes y gasta menos tokens.
3. **Memoria.** Automática, segura y eficiente, sin GC, sin control manual y sin reglas que aprender.

## Dónde está cada pilar hoy

| Pilar | Medición nueva (reproducible) | Estado |
|---|---|---|
| Velocidad | `python benchmarks/run.py`: Turi frente a C experto en programas clásicos con tamaño de problema dado en ejecución | **Cumplido en 3 de 6, empate en 2.** Media 1,13× frente a C portable experto (0,49× al empezar). Pierde solo `binarytrees` (0,75×). Falta la comparación con C con intrínsecos. |
| IA: errores | `python tools/aibench/run.py`: 60 tareas × 6 lenguajes con el mismo enunciado y pruebas ocultas, modelo de la DGX Spark | **Cumplido frente a C, C++, Rust y Go** (59 de 60 a la primera, igual que Rust y Go y por delante de C y C++; 60 de 60 compilan a la primera). Falta Python (60 de 60). |
| IA: tokens | Misma herramienta, tokens de la solución correcta con el tokenizador del modelo | **Cumplido frente a C, C++, Rust y Go** (66 a 68 tokens frente a 70 a 120). Con sintaxis de Python compilada por Turi (prototipo): 37 tokens, como Python (36), y el encargo baja de 1.133 a 131 tokens. |
| Memoria | `benchmarks/run.py` (tiempo y memoria pico) más el recuento de marcas de memoria que escribe el programador | **En parte.** Árboles y listas ya se escriben sin nada de memoria (0 marcas) y van 10 veces más rápido que en C++ y Rust; frente a C con arena, 0,75×. En cadenas y mapas (`kmers`, 0 marcas) Turi va 3 veces más rápido que C++ y Rust y queda a 0,84× del C con tabla propia; usa más memoria pico que los cuatro. |

## Por qué otras IAs no se creían los benchmarks publicados

La auditoría del arnés anterior (`bench/amplia`, fuera del repositorio público) encontró motivos concretos:

1. **Las entradas eran una fórmula.** Cada caso generaba sus datos con `(73·i + 101·s) % 1009` dentro de la zona cronometrada. El compilador demostraba que los valores caben en 0..1008 y se repiten cada 1009, y con eso estrechaba tipos y copiaba bloques en vez de calcular. Con datos reales esas optimizaciones se apagan.
2. **El C de la suite de 100 no era experto.** Era una traducción mecánica con `int64_t` en todo, `-O3 -march=x86-64-v3`, sin LTO, PGO ni `restrict`. C, C++, Rust y Zig salían todos casi exactamente igual de lentos: eso mide el optimizador de Turi contra LLVM sobre código ingenuo, no Turi contra C.
3. **Se ajustó y se evaluó sobre los mismos programas.** En la v0.6 Turi perdía en `matrixMultiply` (0,35×), `floydDistances` (0,39×) y `selectionSort` (0,73×). La v0.7 añadió rutinas escritas a mano que reconocen justo esas formas (una con vía especial para `n == 32`, el tamaño del benchmark) y publicó «10/10 victorias».
4. **El arnés no era público.** `.gitignore` excluía `/bench/`, así que nadie podía repetir las cifras.

No se encontró lo que cabía temer: Turi no memoriza resultados entre llamadas cronometradas.

**Qué pases del optimizador son legítimos y cuáles no.** Son generales o aprovechan de verdad la semántica del lenguaje: el probador de rangos, la inferencia de no solapamiento, la reutilización de memoria, el movimiento por último uso, la reducción de módulo y el `if`-conversión. Son reconocimiento de formas de benchmark y deben salir de las cifras públicas (o generalizarse): `tileRepeatPeriodicArrayFill`, `memoizePureSmallDomainLoops`, `unrollRadixDigitSumLoops`, `interchangeRowStripMatMul` → `matmul_mod.u16`, `floyd_step_u16`, `ArgMinU16`, `FindFirst`, `vectorizeBackwardShiftLoops` y `promoteDisjointIndexedAccum`.

## Reglas de medición desde ahora

- El arnés y las fuentes de todos los lenguajes están en el repositorio (`benchmarks/`, `tools/aibench/`).
- El tamaño del problema llega por `argv`. Nada es conocido al compilar.
- Mismo algoritmo y misma salida en todos los lenguajes; el arnés lo comprueba.
- C se compila como lo haría un experto: `-O3 -march=native -flto`. Turi con `veloz build --release --cpu native`, sin opciones por programa.
- Ningún pase del compilador puede depender de la forma de un programa del banco. Cualquier mejora se justifica con una propiedad general y se prueba en programas que no la inspiraron.
- Se publican también las derrotas.

## Pilar 1 · Velocidad

### No hay techo en el empate

Con el mismo algoritmo, Turi y C pasan hoy por el mismo LLVM, así que lo primero es no perder nada frente a C. Pero el empate no es el objetivo ni el límite. Un programa en C es estático, sus tipos y su disposición de datos los fijó el programador, el compilador lo tradujo con heurísticas sin medir nada y está atado a la ABI de la plataforma. Turi puede romper las cuatro cosas:

1. **Saber más.** Sin solapamiento de punteros, con rangos, con pureza y con acumulaciones flotantes que se pueden agrupar. Ya da resultado: `spectralnorm` gana por 2,35× sin tocar el programa.
2. **Buscar más.** El tiempo de compilación no importa: se compilan muchas variantes de cada bucle caliente y se elige la que el procesador real ejecuta más rápido.
3. **Bajar más.** Reescribir el código máquina que deja LLVM (instrucciones, orden, registros, alineación) con búsqueda y demostración de equivalencia.
4. **Adaptarse.** Elegir representación de datos y código según los datos y la máquina reales, en ejecución.

Hay precedentes publicados de que esto supera al código escrito a mano por expertos: búsqueda estocástica de código máquina (STOKE), rutinas de ordenación halladas por búsqueda e integradas en libc++ (AlphaDev), generadores con búsqueda empírica (FFTW, ATLAS, SPIRAL) y núcleos generados en ejecución (libxsmm). El diseño de cada vía está en [DISENO.md](DISENO.md), bloque V. Cada programa del banco que Turi no gane es un defecto abierto.

### Lo que ya se cambió en el compilador (general, sin formas de benchmark)

| Cambio | Efecto |
|---|---|
| `a[i] += v` y `b[i].campo = v` ya no conservan la comprobación de índice cuando el bucle la garantiza. Antes toda escritura compuesta o de campo la pagaba siempre. | Bucles de actualización en sitio sin guardas |
| Se quita el atributo `sanitize_address` del IR en los binarios normales. Hacía que LLVM no especulara cargas (LICM, vectorización). | Afecta a todo el código |
| Fusión `a*b + c` en FMA, como hace C por defecto. | Código flotante |
| Arreglos locales pequeños de tamaño constante que no escapan pasan a la pila. LLVM puede tenerlos en registros. | Mandelbrot: 496 → 215 ms |
| La acumulación `x += e` de flotantes puede vectorizarse (orden de agrupación libre). | Reducciones flotantes |
| Las funciones pequeñas con flotantes y conversiones se integran en quien las llama antes del análisis de rangos. | `spectralnorm`: 0,62× → 2,35× |
| La versión rápida con guarda en ejecución (una comprobación al entrar, ninguna dentro) cubre ahora funciones con arreglos y con llamadas. | Aritmética de índices sin comprobaciones |
| Tipos recursivos directos con celdas de tamaño fijo, lecturas prestadas y una región por vuelta de bucle (pilar 3). | `binarytrees`: 0,14× → 0,40× |
| El resultado que se guarda en una celda se construye dentro de ella; las hojas idénticas comparten una celda constante; una sentencia suelta también tiene región. | `binarytrees`: 0,40× → 0,75× |
| La fusión `a*b ± c` en FMA la decide Turi con regla fija (producto de la izquierda), no la heurística de LLVM. | `nbody`: 0,80× → 1,06×; `mandelbrot`: 0,87× → 0,99× |
| Adelanto de caché en bucles de mapa: se pide la memoria de la clave de 8 vueltas más adelante. | Mapas que no caben en caché: hasta 1,9× |
| El hash de una clave `str` corta se calcula con sus campos, sin releer de memoria lo recién escrito (el procesador no podía reenviar esa lectura y encadenaba cada búsqueda con la anterior). | `kmers`: 0,36× → 1,19× |
| Un temporal que se entrega no se vacía ni se suelta después; la variable que un `return` entrega tampoco. | Menos escrituras en todo código con valores del montón |

### Resultados

Ryzen 7 7700X, Windows, un hilo, mínimo de 5 ejecuciones del proceso completo. «Frente a C» por encima de 1,00× significa que Turi es más rápido. Tabla completa con C++, Rust y Go en [benchmarks/RESULTS.md](../benchmarks/RESULTS.md).

| Programa | Turi (ms) | C experto (ms) | Turi frente a C |
|---|---:|---:|---:|
| spectralnorm (bucles con división) | 89,7 | 213,8 | **2,38×** |
| fannkuch (permutaciones sobre arreglos de enteros) | 142,9 | 155,5 | **1,09×** |
| nbody (arreglo de estructuras, coma flotante) | 482,8 | 520,6 | **1,08×** |
| kmers (subcadenas como claves de mapa) | 39,8 | 40,0 | 1,00× (entre 1,00× y 1,19× según la medición) |
| mandelbrot (8 píxeles por paso) | 181,8 | 175,9 | 0,97× (entre 0,97× y 0,99×) |
| binarytrees (millones de nodos pequeños) | 48,1 | 36,2 | 0,75× |

Media geométrica: **1,13×** (mínimo de 9 ejecuciones). Tres victorias claras, dos empates (dentro del ruido de esta máquina, un 5 %) y una derrota. Evolución de la media con las mismas mediciones: 0,49× al empezar, 0,68×, 0,82× (tipos recursivos), 0,92× (hash por campos), 1,04× (fusión FMA decidida por Turi) y 1,13× (resultado construido en su celda, hojas compartidas y regiones por sentencia).

El C de esta tabla es C portable bien escrito (`restrict`, acumuladores múltiples, `-O3 -march=native -flto`), sin intrínsecos. La versión con intrínsecos de cada programa está por escribir y es la referencia que hay que superar.

En `kmers` Turi es más de 3 veces más rápido que C++ y Rust, 2 veces más que Go y igual o más rápido que el C con tabla propia (entre 1,00× y 1,19× según la medición), sin escribir nada de memoria. En `binarytrees`, escrito ya sin `Pool` ni `Handle`, es 10 veces más rápido que C++ y Rust escritos de la forma habitual y 5 veces más rápido que Go; el C con una reserva por árbol sigue un 33 % por delante.

### Dónde queda margen y qué falta

1. **`mandelbrot` (empate).** La diferencia era qué producto fundía LLVM en la FMA; ya lo decide Turi. Para pasar por delante: entrelazar dos vueltas independientes del bucle exterior, porque el interior está limitado por latencia y no por número de instrucciones (DISENO, V5b). Los bucles de `main` siguen sin versión rápida con guarda (V1a).
2. **`nbody` (ya gana, 1,06×).** Era la misma lotería de la FMA. Queda margen con la búsqueda empírica de variantes (V4).
3. **`kmers` (ya gana, 1,19×).** Queda margen con el mapa de entradas densas (M2); el adelanto de caché ya actúa cuando la tabla no cabe en caché.
4. **Nodo de 40 bytes frente a 16** (`binarytrees`): el opcional de celda ocupa 16 bytes y cada celda lleva contador. Con el opcional en 8 bytes y celdas sin contador para dueño único el nodo queda como el de C; con enlaces de 32 bits, más pequeño (M1, puntos 1, 2 y 4).

## Pilar 2 · IA

### Qué se midió

`tools/aibench` da a cada lenguaje el mismo enunciado en inglés, la firma tipada y dos ejemplos. La respuesta se compila y se ejecuta contra pruebas públicas y ocultas; si falla, el modelo recibe el error del compilador o el ejemplo fallido y tiene dos intentos más. Turi recibe además una tarjeta de referencia, porque el modelo no ha visto nunca el lenguaje; los demás no reciben nada.

Modelo `nvidia/Qwen3.8-Flash-Next-NVFP4` en la DGX Spark, temperatura 0, hasta 3 intentos por tarea, 60 tareas en cada lenguaje. Registros en `tools/aibench/results/`.

| Lenguaje | Compila al primer intento | Correcto al primer intento | Correcto en 3 intentos | Tokens gastados por tarea resuelta | Tokens de la solución correcta (mediana) |
|---|---:|---:|---:|---:|---:|
| Python | 60/60 | 60/60 | 60/60 | 53 | 36 |
| **Turi hoy** (25 nov: biblioteca base, tuplas, tipos recursivos) | 60/60 | 59/60 | 60/60 | 79 | **68** |
| Turi el 24 nov (biblioteca base, tarjeta nueva) | 59/60 | 59/60 | 60/60 | 83 | 66 |
| Rust | 60/60 | 59/60 | 60/60 | 85 | 70 |
| Go | 60/60 | 59/60 | 60/60 | 83 | 72 |
| Turi antes de la biblioteca | 60/60 | 58/60 | 60/60 | 100 | 82 |
| C++ | 59/60 | 57/60 | 60/60 | 118 | 82 |
| C | 59/60 | 57/60 | 60/60 | 175 | 120 |

Cómo se llegó a «Turi hoy»: la primera medición con la biblioteca nueva bajó los tokens de 82 a 66, pero 3 respuestas dejaron de compilar a la primera, porque el modelo empezó a escribir cosas que la biblioteca invitaba a escribir y el compilador no aceptaba (`for c in s.chars() { if c == 'a' ...`, `c - 'a'`, `ord(s[i])`). Se corrigió el compilador, no la tarjeta, y la medición siguiente dio 59 de 60. Ese ciclo (medir, leer los fallos, aceptar lo que el modelo escribe) es el método de trabajo del pilar.

Fallos al primer intento:

- **Turi hoy:** `running_maximum` (el modelo empezó el máximo en 0 y falla con negativos: error de lógica, no del lenguaje; corregido al segundo intento). El `to_hex` de la medición anterior ya compila.
- **C:** `reverse_words` y `second_largest` (resultado incorrecto), `run_length_encode` (falta un `#include`).
- **C++:** `fibonacci` y `pascal_row` (resultado incorrecto), `power_mod` (usó enteros de 128 bits, que este clang para Windows no enlaza; en Linux habría compilado).
- **Rust:** `pascal_row`. **Go:** `second_largest`. **Python:** ninguno.

Lecturas:

- **Errores:** Turi iguala a Rust y Go y supera a C y C++. Solo Python queda por delante, por una tarea. Con 60 tareas una diferencia de una tarea no es significativa.
- **Tokens:** la solución en Turi ocupa menos que en Rust, Go, C++ y C. Python sigue siendo casi la mitad. Además Turi paga la tarjeta en cada petición (unos 1.100 tokens de entrada frente a unos 130).
- **Límites de esta medición:** un solo modelo, tareas escritas para este banco (no un conjunto público), dificultad baja, sin Goose, y la tarjeta y el compilador se ajustaron mirando estas mismas tareas. Hace falta repetirla con un conjunto que no se haya visto.

### Python por fuera, Turi por dentro (medido el 25 de noviembre)

Los estudios publicados coinciden en que un modelo rinde mejor cuanto más ha visto un lenguaje, y en que los lenguajes poco vistos cuestan más tokens por la conducta del modelo (reintentos), no por el tokenizador. Se midió qué pasa si el modelo escribe el Python que ya conoce y lo compila Turi (traductor de prueba `tools/pyfront`):

| | Correcto a la primera | Tokens de la solución | Tokens del encargo |
|---|---:|---:|---:|
| Python real (interpretado) | 60/60 | 36 | 130 |
| **Python compilado por Turi** | 59/60 | 37 | 131 |
| Turi con su sintaxis | 59/60 | 68 | 1.133 |

Mismos errores que hoy, la mitad de tokens en la solución y casi nueve veces menos en el encargo, sin perder velocidad (`spectralnorm` en Python traducido: 95 ms, igual que en Turi; C experto: 231 ms). El único fallo fue un `import re`. Esto contesta la pregunta de diseño: la cara del lenguaje para la IA debe ser la sintaxis de Python, con el compilador y la memoria de Turi debajo. Falta llevarlo al compilador y decidir las diferencias de comportamiento (enteros de 64 bits, orden de los diccionarios, copia en `b = a`, sin excepciones). Detalle en `DECISIONES.md` (Turi 0.9.2).

### Qué se cambió en el lenguaje

Además de lo que sigue, hay ya una biblioteca base de cadenas y arreglos (`split`, `join`, `map`, `filter`, `sum`...), llamadas con punto a cualquier función libre, mapas con corchetes (`m[k] += 1`) e iteración de cadenas. Está descrito en `SPEC.md` y probado en `tests/core_lib.py`.

De 32 grafías habituales en modelos entrenados con otros lenguajes, el compilador rechazaba 26. La auditoría de los registros anteriores mostraba las causas: sintaxis parecida a Rust que Turi no aceptaba (`0..=n`, `let mut`, comas en `match`), parámetros y `let` de solo lectura, nombres con guion bajo y falta de retorno implícito. Ahora:

- **Front-end tolerante:** `;`, `elif`, `let mut`, `x++`, `println`, `and/or/not`, `True/False`, `null`, `snake_case`.
- **Mutabilidad inferida:** un `let` o un parámetro que el programa modifica pasa a ser mutable sin escribir nada.
- **Sintaxis nueva:** `0..=n`, ternario `c ? a : b`, retorno implícito, literales de carácter `'a'`, `"n=" + n`.
- `veloz fmt` reescribe todo a la forma canónica, y `--strict` conserva los errores antiguos.

El detalle está en `SPEC.md` y las pruebas en `tests/tolerant.py`.

### Qué falta

1. **Alcanzar a Python en tokens.** Las tuplas y el retorno múltiple ya existen (25 de noviembre). Quedan `sortBy`, `unique`, `first`/`last`, igualdad estructural y `match` con literales, y hacer genéricos `sum`, `max`, `min` e `indexOf`.
2. **Tarjeta nueva oficial**: sustituir `AGENT_CARD.md` por la de `tools/aibench/langs.py` y acortarla.
3. **Inferencia que evite anotar:** `var v = []` con el tipo deducido del primer `push`, `mapNew()` sin anotación.
4. **Repetir la medición** con el conjunto público MultiPL-E/HumanEval (el arnés ya separa tareas de adaptadores), más modelos y Goose como control de lenguaje no visto.
5. **Mensajes de error en inglés** para el bucle de reparación.

Sobre los tokens: un lenguaje que el modelo no ha visto siempre paga la tarjeta en el prompt. Eso no se puede evitar hoy. Lo que sí se puede medir y bajar son los tokens de la solución, y ahí la biblioteca es la palanca.

## Pilar 3 · Memoria

### Cómo se mide

Tres cosas, con las mismas herramientas para todos los lenguajes:

1. **Coste:** tiempo y memoria pico del proceso (`benchmarks/run.py`), en programas que reservan mucho: árboles (`binarytrees`), cadenas y mapas (`kmers`). Faltan por añadir una lista enlazada, un grafo con ciclos y borrado, y un servidor con carga sostenida.
2. **Facilidad:** cuántos conceptos de memoria escribe el programador por cada 1.000 líneas. En Turi: `mut` en llamadas, `Pool`, `Handle`, `poolAdd`/`poolGet`/`poolRemove`, `Shared`, `freeze`, `Region`, `trusted`/`raw`, `MutPtr`, `free`. En Rust: `&`, `&mut`, tiempos de vida, `Box`, `Rc`, `RefCell`, `clone`. En C++: punteros inteligentes, `new`/`delete`, `std::move`. En Go: punteros (el GC hace el resto).
3. **Errores de IA por reglas de memoria:** cuántos fallos de compilación del banco de IA son de memoria (`E_USE_AFTER_MOVE`, `E_ALIAS_MUT`, `E_TYPE_CYCLE`, `E_SEND` frente a los errores de préstamos de Rust).

### Línea base medida

Mismo equipo y método que en el pilar 1. C++, Rust y Go están escritos de la forma habitual en cada lenguaje (`unique_ptr`, `Box`, punteros con GC); C usa una reserva por árbol.

| Programa | Lenguaje | Tiempo (ms) | Memoria pico (MiB) | Marcas de memoria en el código |
|---|---|---:|---:|---:|
| binarytrees | Turi (`Pool`/`Handle`) | 340 | 21,0 | 23 |
| binarytrees | C (arena) | 38 | 8,1 | 16 |
| binarytrees | C++ (`unique_ptr`) | 494 | 11,8 | 11 |
| binarytrees | Rust (`Box`) | 537 | 12,2 | 13 |
| binarytrees | Go (GC) | 305 | 18,4 | 5 |
| kmers | Turi | 178 | 17,5 | 0 |
| kmers | C (tabla propia) | 42 | 9,4 | 14 |
| kmers | C++ (`unordered_map`) | 112 | 11,2 | 5 |
| kmers | Rust (`HashMap`) | 90 | 14,1 | 7 |
| kmers | Go (`map`) | 94 | 16,1 | 4 |

Las marcas de memoria son un recuento aproximado de lo que el programador escribe para gestionar memoria (`Pool`, `Handle`, `poolAdd`, `mut`, `malloc`, `free`, `*`, `&`, `Box`, `unique_ptr`...).

Dos lecturas:

- **Árboles:** Turi ya es más rápido que C++ y Rust escritos de la forma habitual, pero usa más memoria que todos y es el que más marcas de memoria obliga a escribir (23). Es justo lo contrario del pilar. El C con una reserva por árbol es 9 veces más rápido: ese es el objetivo de la región inferida.
- **Cadenas y mapas:** Turi no obliga a escribir nada de memoria (0 marcas), que es el objetivo, pero paga las copias: es el más lento y el que más memoria usa.

### Estado tras los tipos recursivos directos (25 de noviembre de 2026)

`binarytrees` está escrito ahora como en cualquier lenguaje (`struct Node { left: Node?, right: Node? }`, sin `Pool`, `Handle` ni `mut`):

| Programa | Lenguaje | Tiempo (ms) | Memoria pico (MiB) | Marcas de memoria en el código |
|---|---|---:|---:|---:|
| binarytrees | **Turi (directo)** | 48,1 | 11,6 | **0** |
| binarytrees | C (arena) | 36,2 | 8,1 | 16 |
| binarytrees | C++ (`unique_ptr`) | 555,8 | 11,8 | 11 |
| binarytrees | Rust (`Box`) | 484,6 | 12,2 | 13 |
| binarytrees | Go (GC) | 264,9 | 17,8 | 5 |

- **Facilidad:** de 23 marcas de memoria a 0. Es el único de los cinco que no escribe nada de memoria.
- **Velocidad:** de 0,14× a 0,75× frente a C con arena (48 ms frente a 36); 10 veces más rápido que C++ y Rust y 5 veces más rápido que Go.
- **Memoria pico:** 11,6 MiB (C 8,1; C++ 11,8; Rust 12,2; Go 17,8): ya por debajo de C++, Rust y Go, porque las hojas comparten una celda. El nodo interior sigue ocupando 40 bytes (16 en C).
- **Seguridad:** una lista de un millón de nodos se construye y se libera sin desbordar la pila (antes reventaba); `tests/recursive_types.py`.
- **Cadenas y mapas (`kmers`):** de 178 ms a 54 ms sin cambiar el programa (C con tabla propia 45 ms, Go 97, C++ 161, Rust 173). La memoria pico sigue siendo la más alta (17,5 MiB frente a 9,4 de C).

Cómo se consiguió, sin copiar el modelo de Rust, C++ ni Go: el compilador guarda en una celda el campo recursivo; leer, pasar y examinar con `match` un valor que no cambia no copia ni cuenta referencias; y un árbol que nace y muere dentro de una vuelta de bucle se reserva en una región que se suelta de golpe. El programador no escribe ni ve nada de eso. Detalle en `DECISIONES.md` (Turi 0.9.1).

### Qué impide hoy cumplir el pilar

| Problema | Consecuencia |
|---|---|
| ~~Un `struct` no puede contenerse a sí mismo.~~ Resuelto el 25 de noviembre para árboles y listas. Los grafos con ciclos reales siguen usando `Pool`/`Handle`. | Queda: nodo de 40 bytes, sin modificación en sitio a través de una celda, región solo dentro de bucles. |
| `poolGet`, `mapGet`, `sharedGet` y las subcadenas devuelven copias. | Recorrer una estructura copia; actualizar un valor de mapa copia dos veces. |
| Tope fijo de 256 llamadas anidadas. | Cualquier recursión profunda falla con `R_DEPTH`. |
| No hay destructores definibles. | Las bibliotecas exponen `free` y `MutPtr` (`servidor.free(srv)`). |
| `mut` en la firma y en cada llamada. | Unas 85 marcas por cada 1.000 líneas en TurIDE. |

### Dirección propuesta (sin copiar a Rust, C++ ni Go)

La idea es que el programador escriba solo valores y el compilador elija la representación:

1. **Tipos recursivos directos** (primera mitad hecha el 25 de noviembre; lo que falta está en `DISENO.md`, M1). `struct Node { left: Node?, right: Node? }` debe compilar. El compilador decide: caja única si el valor tiene un solo dueño, región si todo el árbol muere junto (el caso de `binarytrees`: una reserva por árbol, una liberación), y manejador generacional solo si hay ciclos reales.
2. **Lecturas sin copia.** Leer un campo, un elemento de mapa o una subcadena presta en vez de copiar mientras el compilador demuestre que el dueño no cambia.
3. **`mut` inferido en la llamada**, como ya se infiere en `let` y parámetros.
4. **Guarda de pila en vez del tope de 256.**
5. **Destructores** para tipos que envuelven recursos externos.

## Profesional y autoalojado

### Qué falta para dejar de ser experimental

- Versiones etiquetadas e instalador. Hoy hay 0 etiquetas y versiones incoherentes (SPEC 0.5, README 0.8, paquetes 0.1.0).
- Depuración: no se emite información DWARF ni CodeView.
- Un solo idioma en mensajes y documentación.
- Documentación real: `docs/` solo tenía imágenes.
- CI en macOS y ARM, compilación cruzada con `--target`.
- Biblioteca: directorios y rutas, fecha y hora, expresiones regulares, procesos con lista de argumentos, mapa ordenado, cola de prioridad.
- Partir `native_opt.hpp` (813 KB) y `semantics.cpp` (676 KB), y pruebas por pase.

### Camino al compilador escrito en Turi

Bloquean hoy, por orden: el tope de 256 llamadas (un analizador recursivo lo supera), los tipos recursivos (el árbol sintáctico), `match` con `_`, literales y guardas, el acceso sin copia, la iteración determinista de mapas (para comprobar que la etapa 2 y la 3 son idénticas) y lanzar procesos con lista de argumentos.

1. **Etapa 0:** el compilador actual en C++.
2. **Etapa 1:** lexer, parser, comprobador y bajada a LLVM IR escritos en Turi para el núcleo del lenguaje, reutilizando el runtime actual (ya es IR de texto y C). Se valida compilando las pruebas existentes con las dos etapas y exigiendo la misma salida y los mismos diagnósticos.
3. **Punto fijo:** la etapa 1 se compila a sí misma (etapa 2), y esta otra vez (etapa 3); las dos deben ser idénticas byte a byte.
4. Después se portan los pases del optimizador y se retira el C++.

## Orden de trabajo recomendado

El orden detallado, con archivos, pruebas y objetivo medible de cada paso, está en [DISENO.md](DISENO.md). En resumen:

1. Lo que queda de la desventaja frente a C portable: bucles de `main` con versión rápida, división por valores fijos del bucle y contrato con el reservador de memoria (V1 a V3).
2. Memoria: guarda de pila, tipos recursivos con región por raíz y lecturas sin copia (M4, M1, M2).
3. Biblioteca de cadenas y arreglos, tarjeta nueva e inferencia (I1 a I3).
4. Búsqueda empírica de variantes y superoptimizador de código máquina (V4, V5): aquí se pasa a ganar también en bucles aislados.
5. Biblioteca vectorial, especialización en ejecución, ABI propia y paralelismo (V6, V8, V9, V10).
6. Representación adaptativa de datos (V7).
7. Lo profesional y el compilador en Turi, en paralelo.
