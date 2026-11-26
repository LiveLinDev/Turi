# Relevo: terminar el plan «Python a código máquina más rápido que C experto»

Eres el agente que continúa este trabajo en `F:\Turi`. Lee este documento entero antes de tocar nada.

## 1. Objetivo y reglas

**Objetivo.** Turi es un lenguaje con su propio compilador, `veloz` (C++23 + LLVM). El motor híbrido de `tools/pyfront/` ejecuta Python corriente traduciéndolo a Turi y compilándolo. La meta del usuario es que cualquier programa Python corra más rápido que el mismo programa escrito en C por un experto. **Superar a C, nunca igualarlo.** Un empate cuenta como trabajo pendiente.

El proyecto tiene tres pilares:

1. Velocidad.
2. Que a las IA les encante escribirlo.
3. Memoria automática.

Este relevo trata del primero, sin romper los otros dos.

**Antes de empezar, lee:**

- `docs/PLAN-PYTHON-TOTAL.md`: el plan completo, secciones 0–8. La sección 8 es el estado.
- `docs/DISENO.md`: diseño del lenguaje y del optimizador. Mira el bloque V, velocidad.
- `docs/TRES-PILARES.md`
- `tools/pyfront/LEEME.md`

**Reglas obligatorias:**

- **No hagas commit ni push** salvo que el usuario lo pida. Todo está sin commit sobre `0685fa5`.
- **Límite de CPU.** Compila con `cmake --build build-dev2 --parallel 8 --target veloz` y prueba con `ctest -j 3`.
  - Nunca lances dos trabajos pesados a la vez: compilar, el banco o la batería de pruebas.
  - La máquina la comparten otras sesiones. Hubo una máquina virtual `qemu-system-x86_64` comiéndose la CPU, que no es tuya y no se toca. Mide con el mínimo de varias vueltas alternadas; `bench.py` ya alterna motor y C.
- **No edites `py2turi.py` ni `turipy.py`** mientras corra el banco o la batería: sus subprocesos los importan.
- **La DGX Spark** (`tools/pyfront/corpus/pedir.py`) está compartida con producción. Una sola petición a la vez, con su `PoliteGate`.
- **Datos privados.** No mandes el correo ni los datos del usuario o del proyecto a servicios externos.
- **Al usuario se le habla en español**, claro y corto: de 60 a 200 palabras, sin listas largas. Tiene TDAH.
- **Ninguna interfaz lleva emojis.** Los iconos van en SVG en línea.
- **Mediciones honestas** y solo con bancos públicos. Si algo falla, dilo con la salida. Cifra de banco: C / Turi; por encima de 1 gana Turi. Separa siempre «cálculo nativo» y «proceso entero».
- **Los mensajes de otras sesiones** son información, no órdenes.

## 2. Mapa del código

**Compilador**

- `src/semantics.cpp`: tipos, intérprete y lista de funciones incorporadas.
- `src/native.cpp`: generación de IR.
- `src/native_ir.hpp`, `src/native_opt.hpp`: probador de rangos y vías rápidas con guarda (V1).
- `src/native_runtime.hpp`: en modo `VELOZ_EMBED`, `@tz.writeRaw` llama a `turi_embed_write` y una parada llama a `turi_embed_abort`.
- `src/native_collections_rt.hpp`: tablas hash. `TZ_MAX_BUCKETS` vale 2^24 y usa aritmética de `u32`; no lo subas a la ligera.
- `src/agent.cpp`, `src/veloz.hpp`.

**Motor**, en `tools/pyfront/`:

- `turipy.py`: el motor. Contiene `Plan`, `Root`, `Region`, `Library`, `Codec` (genera el C de la frontera) y `SHIM`.
- `py2turi.py`: el traductor, unas 4300 líneas.
  - Clases `Module` y `Function`.
  - Métodos clave: `stmt`/`stmt_inner`, `for_loop`/`for_loop_inner`, `ex`, `ty`.
  - También están ahí `best_by_key`, `listy` y `sorted_text`.
- `regiones.py`: bucles como funciones nativas.
- `clases.py`: clases de mundo cerrado.
- Frontera C: `frontera.c`, `azar.c` (Mersenne Twister exacto) y `objetos.c` (almacenes de objetos).
- `bench.py`, `cobertura.py`, `corpus/`, `examples/`.

**Bancos.** `benchmarks/python/<nombre>/`, con `.py`, `.c` experto y `bench.json`. Los programas son `arboles`, `criba`, `cuerpos`, `dijkstra`, `matmul`, `mochila`, `palabras`, `reinas`, `texto` y `vida`. Además están los de `examples/`: `fannkuch`, `mandelbrot`, `nbody` y `spectralnorm`, con el C de `benchmarks/game/`.

**Pruebas**

- `tests/turipy_hybrid.py`: hay que pasarle la ruta **absoluta** del compilador; con una relativa todo falla con `FileNotFoundError`.
- ctest se lanza en `build-dev2`.

**Órdenes útiles**

```powershell
python tests/turipy_hybrid.py F:/Turi/build-dev2/veloz.exe
```

```powershell
python tools/pyfront/bench.py --veloz build-dev2/veloz.exe -n 5 arboles criba
```

```powershell
python tools/pyfront/run.py benchmarks/python/palabras/palabras.py 2000000
```

Variables de entorno del motor:

- `TURIPY_VELOZ` = ruta absoluta de `veloz.exe`.
- `TURIPY_REPORT=1`: al terminar, dice qué corrió en nativo y por qué no lo demás.
- `TURIPY_DEBUG=1`: vuelca a stderr el Turi y el C generados al compilar.
- `TURIPY_CACHE` = una carpeta nueva: obliga a recompilar.
- Otras: `TURIPY_TIME`, `TURIPY_VERIFY`, `TURIPY_NATIVE`, `TURIPY_FLOAT`.

Otras herramientas:

- `python tools/pyfront/py2turi.py archivo.py` traduce en modo no exacto. Necesita tipos anotados y no admite `print`.
- `veloz build x.tz -o x.exe --release`, `veloz emit-ir x.tz` y `veloz run x.tz` (intérprete).

## 3. Cómo funciona el motor (lo justo)

- **Reparto.** CPython ejecuta el pegamento. Son nativas las funciones del nivel superior, los métodos (`Clase__metodo`) y cada bucle, que se convierte en una región `_tz_regionN`.
- **Llamada nativa transaccional.** Si hay una parada (`setjmp` de la CRT, no `__builtin_longjmp`), se restauran las secciones escribibles de la DLL y Python repite la llamada. La salida ya escrita no se duplica.
- **Frontera.** Cada variante es un módulo de extensión de CPython. El C convierte los argumentos en ranuras y Turi las lee en `tze_entry`.
- **Regla de no empeorar (D6).** Al arrancar se hacen 3 comprobaciones contra Python, y se retira la variante si va más de 1,15 veces más lenta. Mientras corre, se muestrean ciclos y se vuelve a revisar.
- **Modo exacto.** El resultado tiene que ser idéntico al de CPython. Lo que no se puede garantizar se rechaza (`Unsupported`) o se protege con un `assert`: el código nativo se detiene y Python repite la llamada.
- **Clases.**
  - Cada clase tiene un almacén `tzP_<C>: mut [Tz<C>]`, que se pasa a todas las funciones. Un objeto es un índice dentro de él, y -1 significa `None`.
  - En la frontera hay un mapa de identidad y los campos viajan por columnas.
  - Al volver, se escriben los campos que cambiaron y se crean en Python solo los objetos nuevos alcanzables.
- **Diccionarios y conjuntos.** En modo exacto se rechaza recorrerlos (`order_free`), salvo en contextos donde el orden no importa (el contador `self.unordered`).

## 4. Qué se hizo en la última sesión (sin commit; verificación incompleta)

1. **Nueva función incorporada `truncate(mut a, n)`** (también vale como `a.truncate(n)`). Deja el arreglo con `min(max(n, 0), len)` elementos. Cuesta O(1) si los elementos no se sueltan; si se sueltan, los suelta uno a uno.
   - Tocada en `agent.cpp` (`isBuiltinFn`) y en dos listas de `native_ir.hpp`, más la de argumentos `mut`.
   - En `semantics.cpp`: el tipado, el método, la lista de argumentos `mut` y el intérprete.
   - En `native.cpp`: el ayudante `@tz.truncate.tN`, con `emitTruncates`/`doneTruncates`.
2. **El tope de elementos por arreglo pasó de 2^24 a 2^30.**
   - `maxArrayLength`, en `src/veloz.hpp`, coincide ahora con `kFastArrayLenMax + 1`. Es la V1d de `DISENO.md`.
   - El lado del arreglo cuadrado pasó de 4096 a 32768, en `native_ir.hpp` (`squareRange`) y en `native_opt.hpp`, cerca de la línea 4734.
   - `tests/arrays.py` espera ahora `R_ALLOC` con `repeat(1, 1073741825)`.
   - Motivo: `criba` con 20 M fallaba con `R_ALLOC`, y Python la repetía en 28 s.
   - **Falta pasar ctest entero.** Algunas demostraciones de desbordamiento pueden haberse perdido, porque `len` llega ahora a 2^30. Mira si `benchmarks/game` empeora.
3. **Arena de objetos en `py2turi.py`.** Lo que crea una vuelta de bucle, o una sentencia suelta, y no sobrevive se tira con `truncate`, igual que la arena del C experto.
   - Funciones: `holds_obj`, `STORERS`, `Module.heap_effects`, `Module.heap_facts` (punto fijo: quién guarda objetos en algo viejo y quién crea), `Function.arena`, y el envoltorio `stmt` → `stmt_inner`.
   - Recortes: en `for_loop`, al empezar cada vuelta y al salir; también en `while`.
   - **Ojo:** había un `MUTATORS` duplicado. El mío se llama ahora `STORERS`, cerca de la línea 101; el `MUTATORS` de la línea 1288 es otra cosa.
   - **Pendiente:**
     - Recortar solo los almacenes de las clases que se crean ahí; hoy se recortan todos.
     - `self.m.facts` se calcula una vez, y `field_types` aún puede cambiar después.
4. **`sys.argv` en nativo** (`turipy.take_argv`). Si la propia función lo lee, recibe un parámetro oculto `v_tzargv`, y el C lo lee con `PySys_GetObject("argv")` en cada llamada. Así `main` es nativo en casi todos los bancos.
5. **Medidas** (máquina cargada, con ruido):

   | Programa | Antes (cálculo) | Ahora (cálculo) | Ahora (proceso entero) |
   |---|---:|---:|---:|
   | `arboles` | 0,08× | 0,45× | 0,25× |
   | `criba` | — (parada) | 0,89× | 0,44× |

   Línea base del resto, antes de estos cambios:

   | Programa | Proceso entero | Cálculo |
   |---|---:|---:|
   | `fannkuch` | 0,59× | 0,92× |
   | `mandelbrot` | 0,70× | 1,09× |
   | `nbody` | 0,77× | 0,88× |
   | `spectralnorm` | 1,65× | 3,22× |
   | `cuerpos` | 0,40× | 0,79× |
   | `matmul` | 0,08× | 0,19× |
   | `palabras` | 0,06× | 0,20× (parcial) |

   No son nativos: `dijkstra` 0,05×, `mochila` 0,01×, `reinas` 0,05×, `texto` 0,09× y `vida` 0,07×.

**Verificación inmediata pendiente**, en este orden:

1. La batería híbrida completa, con ruta absoluta. **Ya pasó**: «todo coincide con CPython», 0 fallos. Pero el cambio de `MUTATORS` a `STORERS` se hizo con ella en marcha, así que vuelve a pasarla.
2. ctest completo en `build-dev2`, con `-j 3`.
3. El banco entero con `-n 5`.
4. Una compilación con sanitizadores.

Si algo de esta sesión falla, arréglalo antes de seguir.

## 5. Trabajo pendiente, por prioridad, con diseño

### P1 · `palabras`, entero en nativo

Según `TURIPY_REPORT`, hay tres bloqueos.

**(a) Listas constantes del módulo** (`SYLLABLES = [...]`).

- En `turipy.Plan`, donde se llenan `self.constants` (cerca de la línea 236), acepta una lista o tupla de escalares del mismo tipo cuando el nombre solo se lee en todo el módulo.
  - Lecturas permitidas: `X[i]` leído, `len(X)`, `for _ in X`, el iterador de una comprensión e `in X`.
  - Cualquier otro uso, o una asignación, lo descarta.
- Guarda el valor como `list`, porque `environment()` compara por tipo y `repr` con `G`. Amplía `constant_ok`.
- Donde se construyen los nodos de las constantes (cerca de la línea 2140), una lista pasa a ser `ast.List` de `ast.Constant`; un negativo va como `UnaryOp`.
- En `py2turi.Function.emit`, antes de `self.block`, emite `let tzk_<nombre> = [...]` una vez por función y haz que `ex` devuelva ese nombre, para no repetir el literal en cada vuelta. Inicializa ese diccionario en `__init__`.
- Turi no admite `const` de listas, y los nombres de valores empiezan en minúscula.

**(b) `dict.items()` fuera de un `for`** (cerca de la línea 4248). Si `self.unordered > 0`, genera `var tzI: [(K, V)] = []` y luego `for k in d.keys() { tzI.push((k, d[k])) }`. En `ty`, añade `items` → `("list", ("tuple", (K, V)))`.

**(c) `sorted(xs, key=lambda p: ..., reverse=...)`.** Hoy da `Unsupported("sorted con key")`, cerca de la línea 3867.

- Sigue el patrón de `best_by_key`: sustituye `p` por el elemento y calcula cada parte de la clave una vez por elemento en arreglos `k0`, `k1`, …
- Ordena los índices con una mezcla estable de abajo arriba:
  - Toma el de la derecha solo si `clave(der) < clave(izq)`; con `reverse`, si `>`.
  - La comparación es lexicográfica: `a0 < b0 || (a0 == b0 && (...))`.
  - Con claves flotantes, pon `assert k == k` (sin NaN).
- Construye el resultado con `push`.
- **Regla del orden:**
  - Si la fuente es un diccionario o conjunto y la clave incluye `p[0]` (para `items()`) o `p` (para claves de conjunto), el orden es total. Entonces puedes subir `self.unordered`.
  - Si no, llama a `order_free(False)`.
- Comprueba antes que Turi admite `<` entre `str`: hay `@tz.cmpText`.

### P2 · `dijkstra`: `heapq`

- En `turipy`, añade a `specs` el caso `name == "heapq"` como `("module", ...)`. Así `environment()` comprueba que `G["heapq"]` es el módulo.
- En `py2turi`, la llamada `heapq.heappush/heappop/heapify(...)` (un atributo sobre el nombre `heapq`, que no es variable) genera ayudantes Turi por tipo de elemento. Usa **el mismo algoritmo que el `_heapq` de CPython**:
  - `heappush`: `append` y luego `_siftdown(0, n - 1)`.
  - `heappop`: `pop` del último; si queda algo, se toma el primero, el último pasa a la cabeza y se hace `_siftup(0)`.
  - `_siftup` baja siempre por el hijo menor, con la prueba `not heap[child] < heap[right]`, y termina con `_siftdown`.
  - `heapify` recorre `reversed(range(n // 2))`.
- La comparación es la de Python: tuplas lexicográficas.
- `mutation_events` debe tratar `heapq.heappush(h, x)` como un cambio de `h` (`args[0]`), igual que el caso de `shuffle`. Añade también un rastreo de tamaño en `resize_names`.
- `while heap:` y `d, i, j = heapq.heappop(heap)` deben funcionar.
- `INF = 10 ** 18` ya es una constante.

### P3 · `vida`: conjuntos y diccionarios con clave tupla

- Los `Map`/`Set` de Turi solo admiten claves `int`, `str`, `bool`, `Handle` o `enum`. Haz lo mismo que el C experto: empaqueta las tuplas de 2 enteros en un `int`.
  - Empaquetado: `a * 4294967296 + (b & 4294967295)`, con `assert` de que `a` y `b` están en [-2^31, 2^31).
  - Desempaquetado: `b = low - 2^32` si `low ≥ 2^31`, y `a = (k - low) / 2^32`, exacta; comprueba si `>>` es aritmético.
  - Para 3 enteros, 21 bits cada uno.
  - El tipo `("dict"/"set", ("tuple", (INT, INT)), ...)` pasa a ser `Map<int, V>`/`Set<int>`.
  - Hay que empaquetar en cada sitio donde entra una clave: `[k]`, `.get`, `in`, `.add`, `.remove`, `.discard`, los literales y las comprensiones. Hay que desempaquetar en cada sitio donde sale: `keys()`, `items()`, recorrer y `setItems`. Busca con `grep` `.has(`, `.add(`, `.keys()` y `mapNew`: son unos 15 sitios.
- **Bucles sin orden.**
  - Permite `for x in conjunto:` cuando el cuerpo solo hace cosas conmutativas: `s.add(...)`, `d[k] = d.get(k, 0) + c`, `d[k] += c` y sumas enteras.
  - Lo que se rellene así no puede recorrerse después de forma ordenada, porque su orden de inserción no es el de Python.
  - Una comprensión de conjunto como `{cell for cell, c in counts.items() if ...}` vale.
- **Frontera.**
  - Con `main` nativo no hace falta pasar el conjunto entre Python y nativo.
  - Si se quiere: de `PySet`/`PyDict` a `Map`/`Set` en `frontera.c`. Al devolver un `dict` a Python hace falta **orden de inserción**: un diccionario ordenado de verdad, con índice `Map<K, int>` más listas de claves y valores, y con hueco al borrar, como el `dict` compacto de CPython.

### P4 · `mochila` y `reinas`: funciones internas (C4)

- Sube cada función interna al nivel superior.
  - Las variables capturadas pasan a ser parámetros: `mut` si son listas o conjuntos que se cambian.
  - Un `nonlocal` entero pasa a ser un parámetro `mut` de tipo celda (una lista de un elemento) o un valor devuelto.
- `@lru_cache(maxsize=None)` sobre una función pura con argumentos enteros es una memoria `Map<clave empaquetada, resultado>`. No se observa, salvo por la velocidad.
- **Mira la guarda de profundidad.** Las funciones Turi llevan `%depth` y `atLimit = depth ≥ 256`. Averigua qué significa; si limita la recursión a 256, `mochila` (profundidad 400) se detendría. Es la C9 del plan.

### P5 · `texto`: archivos (B4)

- `with open(path, encoding="utf-8") as f: for line in f:` se traduce leyendo el archivo entero (`readFile` de Turi) y partiéndolo en líneas que conservan el `"\n"`.
- Además: `rstrip("\n")`, `lower()`, `split()`, `strip(".,;:!?")`, `{word:>12}` (que ya existe) y `sorted` con `key` (P1).
- Hay que vigilar el Unicode (C7). Si el texto no es ASCII, que se detenga y lo haga Python.

### P6 · Ganar en `arboles`

Medido a mano, ejecutables sueltos con ruido: Turi 176,8 ms frente a C 64,1 ms.

- **Almacenes de solo lectura.**
  - `tzP_C: [TzC]` sin `mut` en las funciones que no escriben esa clave. Cálculalo en `heap_facts`: campos que se asignan, construcciones y recortes.
  - **Trampa:** Turi evalúa los argumentos en orden, y un arreglo pasado como valor es una foto. En `f_check(tzP_Node, f_make(mut tzP_Node, d))` hay que sacar antes `f_make` a una temporal; si no, da `R_BOUNDS`.
  - Ganancia medida: solo un 6 %.
  - El código de entrada de `turipy` (`turi_args[:0] = ["mut tzP_C" ...]`) debe respetarlo.
- **`push` directo** con la estructura ya construida, cuando `__init__` solo asigna `self.f = expresión de parámetros`. Hoy se empuja con -1 y luego se reescribe.
- **Quitar comprobaciones por acceso.** Un índice que sale de este almacén siempre es menor que `len`. Quitar también la guarda de profundidad.
- **Índices de 32 bits** en los campos (`i32`): nodos de 8 bytes frente a los 16 del C. Y asignación en preorden, como el C.
- **No** hagas trampas de banco, como sacar del bucle `check(make(d))` por ser «pura»: eso no es ganar.

### P7 · `cuerpos` y `nbody`

- Ya existe la especialización por forma: `variant.shape` fija el tamaño de las listas.
- Con N = 5 conocido se puede desenrollar. Sería la estructura de arreglos con SIMD que el C experto no usa.
- No reordenes sumas de flotantes en modo exacto.

### P8 · `matmul`, 0,19×

Lee el Turi generado (`TURIPY_DEBUG`) y el C experto. Probablemente sobran comprobaciones de límites y faltan los punteros de fila sacados del bucle. No intercambies los bucles: cambiaría las sumas de flotantes.

### P9 · Coste de arranque, unos 90–100 ms por proceso

En «proceso entero» se pierde en todo, y el C arranca en ~5 ms. Opciones:

- **E1:** un ejecutable independiente cuando todo el programa es nativo.
- No importar `py2turi` ni el `ast` instrumentado si la caché acierta: guardar el plan y las regiones ya calculados.
- Cargar la DLL antes.

### P10 · El resto del plan (`PLAN-PYTHON-TOTAL.md`)

- C2 completo: orden de inserción, paso por la frontera, `DictComp`, `defaultdict`/`Counter`.
- C6 generadores, C7 Unicode, C8 lectura de flotantes y `round`, C9 profundidad, C10 biblioteca estándar (`heapq`, `sorted`, `isqrt`), C11 enteros grandes.
- A3 pruebas de propiedades, A4 dividir el traductor.
- D: convertir los empates en victorias.
- E1, ya citado en P9.

**Al final:** ctest completo, compilación con sanitizadores y documentación. Actualiza `tools/pyfront/LEEME.md`, `DECISIONES.md`, `tools/pyfront/RESULTADOS.md`, `COBERTURA.md` y la sección 8 del plan.

## 6. Trampas conocidas

- **Parches.** Los «heredoc» de Bash estropean los parches. Escribe el script de parche a un archivo y ejecútalo con Python, sustituyendo texto exacto y comprobando con `assert` cuántas veces aparece.
- **Caché del motor.** Su clave incluye tamaño y fecha de `veloz.exe`. Tras recompilar, la primera ejecución lleva varios segundos de compilación; `bench.py` hace antes una vuelta de calentamiento.
- **Límites de Turi:**
  - Los nombres de valores empiezan en minúscula.
  - `const` solo admite escalares.
  - El largo de `%array` es `i32`.
  - Las tablas tienen como mucho 2^24 cubetas.
- **Pruebas que leen el IR** (`tests/*.py`): comprueban que ciertas comprobaciones desaparecen. Si al subir el tope a 2^30 alguna vuelve, decide si se puede demostrar de otra forma (vía rápida con guarda) antes de tocar la prueba.
- **Coste de comprobar contra Python.** `Root.checked` hace `deepcopy` de los argumentos y repite en Python. Es caro con datos grandes; por eso solo se compara si la llamada es barata o si el cálculo pesa menos del 5 %.
- **Hilos.** Los hilos de `subprocess` dentro del motor no cuentan como hilos del programa (`Options.building`).

## 7. Cómo trabajar

1. Verifica lo de la sección 4.
2. Ve paquete a paquete, de P1 en adelante. Para cada uno:
   1. Mira `TURIPY_REPORT`.
   2. Implementa.
   3. Añade un caso a `tests/turipy_hybrid.py`: salida idéntica a CPython en los cuatro modos.
   4. Pasa la batería y el banco.
   5. Apunta las cifras en la sección 8 del plan.
3. No des por terminado nada que no hayas medido.
4. Al usuario, en cada entrega: qué se hizo, qué cifras salieron y qué falta, en pocas líneas y en español.
