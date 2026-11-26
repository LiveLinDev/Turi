# Python entero en código máquina: plan de diseño

25 de noviembre de 2026. Escrito para quien lo implemente (Opus). Es autosuficiente: no hace falta haber visto ninguna conversación anterior.

Lee antes, en este orden: `AGENT_MEMORY.md` (reglas 13 a 15 y la lista de verificación), `tools/pyfront/LEEME.md`, `DECISIONES.md` (Turi 0.9.2 a 0.9.4), `docs/TRES-PILARES.md` y `docs/DISENO.md` (este plan usa sus bloques V y M por su nombre).

## 0. La meta, medida

**Cualquier programa Python da lo mismo que CPython; todo lo que calcula corre en código máquina; y donde existe un C equivalente escrito por un experto, Turi le gana.** Ganar es ganar: un empate es un defecto abierto.

Tres marcadores. Ningún paquete está hecho si empeora alguno.

| Marcador | Cómo se mide | Umbral |
|---|---|---|
| **Exactitud** | Salida, errores, trazas y código de salida frente a CPython (`tests/turipy_hybrid.py`, corpus A1, propiedades A3) | 0 diferencias. Siempre. |
| **Cobertura** | `python tools/pyfront/cobertura.py` (A1): funciones nativas / funciones llamadas, y tiempo en nativo / tiempo total | 90 % → 97 % → 100 % de las funciones, quitando la lista cerrada de abajo; ≥ 99 % del tiempo en los programas de cálculo |
| **Velocidad** | `python tools/pyfront/bench.py -n 9` (alternado con el C experto) | Cada programa ≥ 1,05× en tiempo de cálculo. Entre 0,95 y 1,05 es empate (defecto). Menos es derrota (se publica). |

### Lo que nadie puede prometer (lista cerrada)

Decirlo claro evita perseguir imposibles. En estos casos la promesa es «mismo resultado y nunca más lento que CPython», no «más rápido que C»:

1. **Código que se fabrica mientras corre:** `eval`, `exec`, `compile`, importaciones calculadas, `globals()[...]`, `setattr` sobre módulos o clases, metaclases, `__getattr__`. Lo ejecuta CPython; lo de alrededor sigue nativo.
2. **Módulos de terceros escritos en C** (numpy, pandas…). Ya son C. Se llaman tal cual.
3. **Tiempo que es del sistema:** disco, red, `sleep`, esperar al teclado.
4. **Enteros enormes** (más de 64 bits) en el bucle caliente: el C experto usa GMP, que lleva décadas de ensamblador a mano. Objetivo: no perder con CPython; frente a GMP, lo que se mida (paquete C11).

Si algo no está en esta lista y no corre en nativo, es trabajo pendiente de este plan.

## 1. Punto de partida

Todo esto existe y pasa 49 de 49 pruebas (normal y con sanitizador). **Está sin commit** sobre `0685fa5`; la rama `sintaxis-propia` marca el diseño anterior. Antes de empezar, pide al usuario permiso para hacer commit de este estado.

- **Motor híbrido** (`tools/pyfront/turipy.py`, lanzador `run.py`). CPython ejecuta el programa. Cada función de nivel superior es una `Root`: en su primera llamada se describen los tipos reales (`describe`), se traduce con `py2turi.Module(exact=True, ...)`, `veloz` la compila a una DLL (`build_library`, `VELOZ_EMBED=1`), se carga con `ctypes.PyDLL` y el `__code__` de la función se cambia por un envoltorio rápido (`install`). Hasta 4 variantes por función (`Variant`), una especial para listas de 8 elementos o menos.
- **Transacción.** El nativo trabaja sobre copias (`Codec.encode/decode`). Si se detiene (`@tz.abort` → `turi_embed_abort` → `longjmp`, código 1), se restauran las secciones escribibles de la DLL (`Library.restore`) y la llamada se repite en Python (`Root.trapped`). Tres trampas y una de cada cuatro llamadas: la variante se descarta.
- **Comprobación de arranque.** Las primeras llamadas baratas (menos de 0,5 ms) se repiten en Python y se comparan (`Root.checked`).
- **Arranque en caliente.** Manifiesto en caché (`Runtime.save`), importaciones perezosas (`heavy()`).
- **Traductor** (`tools/pyfront/py2turi.py`, 3.000 líneas): `Module` (firmas, tipos de retorno, `lift`), `Function` (`infer_locals`, `check_exact`, `check_flow`, `check_loop`, `find_nonneg`, `emit`). Lo que no puede garantizar idéntico lanza `Unsupported(motivo)`: hay unos 125 motivos distintos.
- **Cifras.** 60 respuestas de un modelo: 60 iguales a CPython, 58 nativas. Cálculo frente a C experto: `spectralnorm` 2,65×, `fannkuch` 1,19×, `mandelbrot` 1,03× y `nbody` 1,02× (empates). El proceso entero paga unos 70 ms de arranque de CPython.

Qué se queda hoy en Python (de `LEEME.md`): `print` y toda entrada o salida; clases; generadores; `try` y `with`; funciones como valores; funciones internas que usan variables de fuera; decoradores; `*args`; dos nombres para una misma lista que se modifica; recorrer diccionarios o conjuntos; claves de diccionario que no sean enteros o textos; diccionarios y `None` como argumento o resultado; texto no ASCII; flotante a texto, `round`, `//` y `%` de flotantes; el código de nivel superior; y **todo lo que llame a algo de esta lista**.

## 2. La idea central: regiones rápidas y pegamento

Un programa tiene dos clases de código. **Pegamento:** leer la entrada, repartir, imprimir el resumen. Corre una vez y da igual lo que tarde. **Cálculo:** los bucles. Ahí se va el tiempo.

El diseño es: **CPython sigue ejecutando el pegamento; el cálculo corre en «regiones» nativas** (trozos de programa sin efectos que no se puedan deshacer, sobre copias, con trampa y repetición en Python). Eso ya funciona y ya gana a C. Lo que falta es que las regiones lleguen a todas partes:

1. **Regiones más pequeñas.** Hoy la región es la función entera. Tiene que poder ser un bucle (fase B).
2. **Efectos que no rompen la región.** `print`, leer un archivo, `input`, `random` (fase B).
3. **Más Python dentro de la región.** Clases, diccionarios completos, excepciones, generadores, funciones internas, Unicode (fase C).
4. **Cada cosa nueva gana a C** con su propio programa de banco (fase D).
5. **Si el programa entero es una región, sobra CPython:** ejecutable independiente (fase E).

### Decisiones tomadas (no se reabren sin medir)

- **D1 · CPython es el suelo.** Nunca se rechaza un programa. Lo no traducido corre en CPython.
- **D2 · Exacto o nada.** Lo que no se pueda garantizar idéntico a CPython no se traduce «casi igual»: se deja en Python o se protege con una trampa. Ninguna diferencia en silencio.
- **D3 · No se construye una «segunda marcha» que compile todo Python contra la API de C** (al estilo de Cython con objetos). Costaría meses, seguiría necesitando CPython y solo aceleraría el pegamento, que no importa.
- **D4 · No hay registro general de efectos ni funciones gemelas.** Los efectos se tratan por clases (abajo); lo que no entra en una clase parte la región y se queda en el pegamento.
- **D5 · El traductor sigue en Python.** No se reescribe en C++. El tiempo de compilación no importa.
- **D6 · Regla de no empeorar.** Una región que, contando el coste de cruzar la frontera, tarda más que su versión Python, se desactiva sola y se dice en el informe.
- **D7 · Los datos de Python tienen identidad; los de Turi, valor.** Una lista u objeto se representa por valor cuando se demuestra que nadie más lo ve cambiar; si no, por asa (índice en un almacén). Nunca se adivina.
- **D8 · Nada de formas de benchmark** (regla 2 de `DISENO.md`). Cada mejora se justifica con una propiedad general y se prueba con programas que no la inspiraron.
- **D9 · Memoria.** Sin anotaciones. Regiones y dueño único primero; contador de referencias donde haga falta; un recolector de ciclos solo como respaldo para clases que pueden formar ciclos, y nunca dentro de un bucle caliente de tipos sin ciclos.

### Las tres clases de efecto que una región admite

Una región se repite entera en Python si se detiene. Un efecto cabe dentro si esa repetición sigue dando lo mismo:

| Clase | Ejemplos | Cómo se mantiene exacto |
|---|---|---|
| **Contable** | `print`, `sys.stdout.write`, `sys.stderr.write` | Se cuenta lo ya emitido; al repetir en Python se descuenta |
| **Repetible** | leer un archivo abierto dentro de la región; escribir uno abierto dentro con `"w"`; `sys.argv`; el reloj | Repetirlo da lo mismo |
| **Con memoria** | `input`, `sys.stdin`; `random` | Se guarda lo consumido (o el estado) y se repone al repetir |

Todo lo demás (llamar a una función de Python con efectos desconocidos, un archivo abierto fuera, red) **parte la región**: se ejecuta en el pegamento, entre regiones.

## 3. Reglas para quien implemente

1. Un paquete cada vez, en el orden de la sección 5. Tras cada uno: `powershell -File tools/build.ps1` al 100 % sin avisos; si tocaste C++, IR o runtimes, también `-BuildDir build-san -Sanitize` y después borra `build-san`; `python tools/pyfront/bench.py -n 9`; `python tools/pyfront/cobertura.py`.
2. Cada paquete añade sus casos a `tests/turipy_hybrid.py` (cada programa corre 4 veces: dos modos de coma flotante con `--verificar`, primera ejecución y arranque desde caché) y sus propiedades a A3.
3. Procesador: compilar con `--parallel 8`, `ctest -j 3`, y nunca dos trabajos pesados a la vez. El usuario usa la máquina.
4. El modelo de la DGX Spark se comparte con producción: una petición cada vez, por el arnés existente (`tools/aibench`), que ya cede el paso.
5. Commit y push solo cuando el usuario lo pida. Nada de datos del usuario ni del proyecto a servicios externos. Descargar un conjunto público (HumanEval, MBPP) necesita su permiso.
6. Al usuario se le responde corto y llano: de 60 a 200 palabras, sin tablas largas.
7. Tras tocar el traductor, mira el Turi generado (`TURIPY_DEBUG=1`): tiene que parecer escrito por una persona (regla 13). No edites `turipy.py` ni el traductor mientras corre la batería.
8. Los tiempos se comparan alternando (regla 14). Menos de un 5 % de diferencia es empate.
9. En PowerShell y bash los parches con comillas y barras invertidas se estropean: escribe el script de parche en un archivo y ejecútalo.
10. Cada decisión de diseño que tomes se apunta en `DECISIONES.md` con su motivo y su cifra. Si un paquete no llega a su objetivo, se escribe como defecto abierto: no se maquilla.
11. Dentro de la fase C, el orden lo decide el histograma de motivos de `cobertura.py`: se ataca siempre el motivo que más funciones deja fuera.

## 4. Paquetes de trabajo

Tamaño orientativo: P (días), M (una o dos semanas), G (más).

### Fase A · Instrumentos

**A1 · Corpus y marcador de cobertura (P).**
`tools/pyfront/corpus/` con tres carpetas: `modelo/` (las 60 respuestas guardadas en `tools/aibench/results/qwen38-flash-v6-turipy.jsonl`), `reales/` (40 programas pedidos al modelo de la Spark con encargos que fuerzan lo que la gente escribe de verdad: clases, diccionarios, `try`, archivos, generadores, cadenas f, `argparse`, recursión con memoria; cada uno con su entrada fija) y `publico/` (HumanEval y MBPP, solo si el usuario autoriza la descarga).
`tools/pyfront/cobertura.py` ejecuta cada programa con CPython y con `run.py --informe`, compara salidas y escribe `tools/pyfront/COBERTURA.md`: programas idénticos, funciones nativas sobre llamadas, tiempo en nativo (`TURIPY_TIME=1`) y los 20 motivos más frecuentes.
*Hecho cuando:* el informe sale con un comando y `modelo/` entra en `ctest`.

**A2 · Banco Python frente a C experto (M).**
Cada característica nueva necesita un programa que la use en el bucle caliente y su C experto (mismo algoritmo, misma salida, `-O3 -march=native -flto`, versión portable y versión con intrínsecos). Añadir a `tools/pyfront/examples/` y `benchmarks/game/`:

| Programa | Qué ejercita |
|---|---|
| `binarytrees.py` | clases con campos que apuntan a su propia clase |
| `nbody_clases.py` | lista de objetos con atributos, como lo escribe un modelo |
| `kmers.py`, `wordfreq.py` | diccionarios, textos, ordenar por clave |
| `dijkstra.py` | `heapq`, tuplas, lista de adyacencia |
| `life.py` | conjunto de tuplas |
| `knapsack.py` | `lru_cache` o diccionario con clave tupla |
| `nqueens.py` | recursión, función interna con estado de fuera |
| `sieve.py`, `matmul.py` | listas y listas de listas |
| `textproc.py` | leer un archivo, partir, contar, imprimir |

El C se escribe primero y sin mirar a Turi. *Hecho cuando:* `bench.py` los mide todos alternados y publica la tabla, derrotas incluidas.

**A3 · Pruebas por propiedades (P).**
`tests/turipy_props.py`: para cada construcción y cada función de biblioteca traducida, entradas aleatorias con semilla fija (`random.Random(0)`, sin dependencias externas), ejecutadas en CPython y en nativo con `--verificar`; se comparan resultado, argumentos modificados y excepción. Incluye los valores frontera: vacío, un elemento, negativos, ±2⁶³, `nan`, `inf`, `-0.0`, texto con acentos.

**A4 · Partir el traductor (P).**
`py2turi.py` pasa a ser una fachada sobre el paquete `tools/pyfront/traductor/` (tipos, inferencia, comprobaciones exactas, sentencias, expresiones, llamadas, métodos, biblioteca). Movimiento mecánico. *Hecho cuando:* el Turi generado para `examples/` y `corpus/modelo/` es idéntico byte a byte antes y después.

### Fase B · Que nada arrastre a nada

**B1 · `print` y escritura en nativo (M).** Lo más rentable: hoy un `print` deja en Python a la función y a todas las que la llaman.
- El nativo da formato y acumula el texto en un búfer por flujo. Lo vuelca al salir de la región, al pasar de 64 KiB y, si la salida es una terminal, en cada salto de línea. El volcado llama a Python (`sys.stdout.write(texto)` por un `ctypes.PYFUNCTYPE` pasado al cargar la DLL): así se respetan redirecciones, codificación y orden.
- El motor cuenta los caracteres ya volcados por flujo. Si la región se detiene, descarta el búfer y repite en Python con `sys.stdout` y `sys.stderr` cambiados por un objeto que se traga esos primeros caracteres (con `--verificar`, además comprueba que son los mismos) y deja pasar el resto. Se restauran en un `finally`.
- En la comprobación de arranque (`checked`) las dos ejecuciones escriben en búferes, se comparan los textos y se emite una sola vez.
- Formato exacto de `str()`: enteros, booleanos, `None`, textos, flotantes (paquete C8: hazlo aquí si aún no está), y listas, tuplas, diccionarios y conjuntos de ellos (dentro de un contenedor los textos van con `repr`: comillas y escapes de Python). `sep`, `end`, `file`, `flush`.
- Cadenas f, `%` y `str.format`: nativos `{x}`, `:d`, `:5d`, `:05d`, `:.2f`, `:8.3f`, `:>8`, `:<8`, `:^8`, `:,`, `:x`, `:e`, `%d`, `%s`, `%.2f`. Cualquier otra especificación va por el puente puro (B3).
*Archivos:* traductor (`expr_stmt`, `as_text`), `turipy.py` (`SHIM`, `Root.checked`, `Root.trapped`), biblioteca Python del traductor.
*Prueba:* imprimir en bucle; imprimir tres veces y desbordar un entero (la salida aparece una sola vez); `stderr` intercalado; `contextlib.redirect_stdout`; contenedores anidados.

**B2 · Bucles como regiones (M).** Para funciones que no se pueden traducir enteras y para el código de nivel superior.
- `instrument` reescribe cada bucle exterior así, **dejando el bucle original en su sitio**:

```python
_t = __tz_r3.enter(a, b, n)      # 0: no hay nativo, o se detuvo. 1: hecho. 2: hecho, comprobar
if _t != 1:
    for i in range(n):           # el bucle original, intacto
        ...
    if _t == 2: __tz_r3.compare(a, b)
else:
    a, b = __tz_r3.take()
```

  El camino lento es el código original en su marco original: trazas, variables y excepciones son las de CPython por construcción. No se crea ninguna función sintética para el camino lento.
- Entradas: los nombres que el bucle lee y que están asignados con seguridad antes (el análisis de `check_flow`). Salidas: los nombres que el bucle asigna. Un `return` dentro del bucle sale por `take()` con una marca. Una variable que puede quedar sin valor (el índice de un `for` de cero vueltas) lleva su marca.
- No se instrumenta un bucle con `yield`, `await`, `del`, nombres capturados por una función interna, `locals()`, `eval` o `exec` en la función.
- Orden de intentos: la función entera como raíz (lo mejor: nada cruza dentro). Si se descarta (`Root.kill`), su `__code__` pasa a la versión con regiones. Si todas sus regiones se descartan, vuelve el código original, sin coste.
- `Root` se generaliza: una región es una raíz cuyo camino lento es «devuelve 0».
*Prueba:* bucles en `main` junto a `input` y `print`; bucles de nivel superior sobre variables globales; `break`, `continue`, `else`, `return` dentro; error dentro del bucle (traza idéntica).

**B3 · Puente puro (P).** Una región puede llamar a funciones de Python **puras y deterministas** de una lista escrita a mano (`tools/pyfront/puras.py`: nombre, tipos de entrada, tipo de resultado, condiciones). Como son puras, repetir la región las vuelve a calcular: no hace falta registrar nada.
- Mecanismo: tabla de punteros `ctypes.PYFUNCTYPE` pasada a la DLL al cargarla. Si la función de Python lanza un error, la región se detiene y la repetición lo lanza de forma natural.
- Usos: especificaciones de formato raras, `round` y texto↔flotante mientras no sean nativos, métodos de texto con letras fuera de ASCII, `re` con patrón y texto, funciones de `math` aún no nativas.
- Nunca entra en la lista nada que lea estado que cambia (configuración regional, reloj, variables globales).
- Cuesta cerca de 1 µs por llamada: la regla de no empeorar (B5) decide.

**B4 · Efectos repetibles y con memoria (M).**
- **Archivos abiertos dentro de la región:** `open(ruta)` para leer (`read`, `readline`, `readlines`, `for linea in f`, `with`) con la API de archivos de Turi; `open(ruta, "w")` para escribir. Hay que copiar exactamente dos cosas de Python: la codificación por defecto (`locale.getencoding()` en ejecución; solo UTF-8, ASCII, Latin-1 y cp1252; cualquier byte que no sepamos decodificar igual detiene la región) y los saltos de línea universales.
- **Entrada estándar:** `input()`, `sys.stdin.readline/read`, `for linea in sys.stdin`. Se lee por el puente y se guarda lo consumido; al repetir, `sys.stdin` se cambia por un objeto que entrega primero lo guardado.
- **`random`:** MT19937 nativo con las mismas derivaciones que CPython (`random`, `randrange`, `randint`, `choice`, `shuffle`, `uniform`). Al entrar se copia el estado con `random.getstate()`; al salir bien se escribe con `setstate`; si se detiene, no se escribe.
- **Reloj:** `time.time`, `time.perf_counter`.
*Prueba:* `textproc.py`; leer números de `stdin` y desbordar a mitad (la repetición lee lo mismo); una simulación con `random.seed(1)` que da la misma secuencia que CPython.

**B5 · Frontera barata y regla de no empeorar (M).**
- Copiar por la API de C de Python en vez de por `array`: al cargar la DLL se le pasa una tabla con las direcciones de `PyList_GetItem`, `PyLong_AsLongLongAndOverflow`, `PyFloat_AsDouble`, `PyUnicode_AsUTF8AndSize`, `PyList_New`, `PyList_SetItem`, `PyLong_FromLongLong`, `PyFloat_FromDouble`, `PyUnicode_FromStringAndSize`, `PyTuple_*`, `PyDict_Next` (sacadas de `ctypes.pythonapi`). No hace falta `Python.h` ni enlazar con Python. El `Codec` genera C por firma.
- Vistas sin copia para `bytes`, `bytearray`, `array.array` y matrices de numpy (protocolo de búfer) cuando la región solo lee.
- **No empeorar:** cada variante lleva su tiempo nativo con cruce incluido y una estimación del tiempo en Python (de las comprobaciones de arranque). Tras 32 llamadas, si el nativo va más lento, se desactiva con el motivo «más lento que Python por el coste de cruzar» y queda en el manifiesto.

### Fase C · Más Python dentro de la región

El orden real lo marca `cobertura.py`. Este es el previsto.

**C1 · `None`, resultados opcionales y tipos mezclados (M).** `T | None` en variables, parámetros, resultados, elementos y valores → `T?` de Turi, con estrechamiento por flujo (`if x is None: return`). `None` cruza la frontera. Uniones pequeñas (`int | str`) → un `enum` generado; `isinstance` → `match`. Tuplas de cualquier longitud → un `struct` generado por forma (hoy solo de 2 a 4).

**C2 · Diccionarios y conjuntos completos (G; toca el compilador).**
- **Orden de inserción:** el mapa de entradas densas de `DISENO.md` M2 (runtime C, auxiliares en IR y espejo del intérprete). Recorrer `d`, `keys`, `values`, `items`.
- **Claves compuestas:** tuplas y estructuras de enteros, textos, booleanos y flotantes (hoy `E_KEY_TYPE`). Es lo más usado por los modelos: `visitados.add((x, y))`, `memo[(i, j)]`.
- Todos los métodos: `get`, `setdefault`, `pop`, `popitem`, `update`, comprensiones, `defaultdict` (leer una clave que falta la crea), `Counter` con `most_common` (orden estable), `OrderedDict`.
- Cambiar el tamaño mientras se recorre: en Python es un error; aquí, trampa.
- Cruzan la frontera como dos listas en orden; un diccionario modificado se escribe de vuelta en el mismo objeto.
- **Orden de los conjuntos:** en CPython es el de su tabla interna. Donde el orden se vea (`for x in s`, `list(s)`, `print(s)`), un conjunto que imita esa tabla (sondeo, crecimiento y la clave de hash del proceso para textos, `_Py_HashSecret`), comprobado en 3.11 y 3.13. Donde no se vea (`in`, `len`, `sorted(s)`), la tabla rápida de hoy.
- `sorted(..., key=lambda kv: (-kv[1], kv[0]))`: orden estable con clave tupla.
*Banco:* `kmers.py`, `wordfreq.py`, `life.py`, `knapsack.py`.

**C3 · Clases (G).**
- **Qué clases:** las de «mundo cerrado»: atributos asignados en `__init__` o en sus métodos, tipos vistos en la primera llamada. No lo son (se quedan en Python): `__getattr__`, `__setattr__`, metaclases, `__del__`, herencia múltiple, atributos añadidos desde fuera de la clase, decoradores de clase desconocidos.
- **Traducción:** clase → `struct`; método → función con `self`. Herencia simple → `enum` de subclases o `dyn`; `isinstance` → comparar etiqueta; `super().__init__` en línea. `__repr__`, `__str__`, `__eq__`, `__lt__`, `__hash__`, `__len__`, `__getitem__`, `__contains__`, operadores (`__add__`…) por despacho estático. `@dataclass`, `NamedTuple` y `Enum` reconocidos.
- **Representación (decisión D7), de la más rápida a la más general:**
  1. *Valor:* el objeto vive dentro de su lista o variable y nadie más lo nombra mientras cambia. `for p in cuerpos: p.x += p.vx` se traduce a `cuerpos[i].x += cuerpos[i].vx`. Igual que un arreglo de `struct` en C.
  2. *Árbol:* la clase se contiene a sí misma y cada nodo tiene un solo padre → tipo recursivo de Turi (celdas y regiones de `DISENO.md` M1).
  3. *Asa:* hay alias o ciclos (lista doblemente enlazada, puntero al padre, grafo) → todos los objetos de la clase en un almacén por región y cada referencia es un índice de 32 bits. Los alias son exactos por construcción.
- **Memoria en almacenes:** primera versión, se libera todo al terminar la región, con un tope (si se pasa, trampa y a Python). Después: contador de referencias para clases que por sus tipos no pueden formar ciclos; recolector de respaldo para las que sí (D9).
- **Frontera:** el grafo de objetos alcanzable se codifica con una tabla de identidad; al volver, los campos cambiados se escriben en los mismos objetos y los nuevos se crean con `object.__new__`.
*Banco:* `binarytrees.py` (da la cifra con y sin compartir subárboles, regla 15) y `nbody_clases.py`.

**C4 · Funciones internas, lambdas y funciones como valores (M).**
- Función interna que no escapa (el `dfs` anidado que modifica `visitados`): se saca fuera y sus variables capturadas pasan como parámetros `mut`. Amplía `Module.lift`.
- Lambdas y funciones como argumento, en variables, listas y diccionarios (tablas de despacho) → valores `fn` de Turi.
- Closure que escapa: solo si lo capturado es inmutable y no se reasigna después (la captura de Turi es una instantánea; la de Python, no).
- `@lru_cache` y `@cache` → memoria nativa con clave tupla (necesita C2). `functools.partial`, `operator.itemgetter`.
- Argumentos con nombre y valores por defecto, resueltos al traducir. `*args` → una versión por número de argumentos. `**kwargs` se queda en Python.

**C5 · Excepciones (G).**
- Dentro de la región una excepción es flujo de control: la función que puede fallar se compila en una segunda versión que devuelve `Result<T, PyErr>`; quien la llama desde un `try` usa `?` o `match`. Fuera de un `try` se usa la versión de siempre (trampa), así que **lo que no usa `try` no paga nada**.
- `try/except/else/finally`, `raise`, clases de excepción propias (tabla de padres para `except Exception`), `assert` con mensaje, `with` sobre recursos nativos.
- Una excepción que nadie captura dentro de la región es una trampa: Python repite y saca el error y la traza de verdad.
- Si el manejador lee el mensaje (`except ValueError as e: print(e)`), el mensaje tiene que ser el de esta versión de Python: tabla de mensajes que el motor comprueba al arrancar provocando cada error en CPython. Mensaje no comprobado → esa función no es nativa.

**C6 · Generadores e iteradores (M).**
- Generador consumido por un `for`: el cuerpo del `for` se mete en el sitio del `yield` (cero coste).
- Expresiones generadoras dentro de `sum`, `min`, `max`, `any`, `all`, `join`, `sorted`, `list`, `set`, `dict`, `tuple` → bucles.
- Generador no recursivo guardado en una variable → `struct` con estado y función `siguiente`.
- Generador recursivo (`yield from self.recorrer(hijo)`): se convierte en lista solo si se demuestra que es puro y termina; si no, Python.
- `iter`, `next`, `enumerate`, `zip`, `map`, `filter`, `reversed` como valores; `itertools` (C10).

**C7 · Texto: Unicode y formato (M).** Tres anchos como CPython: ASCII (el camino de hoy, sin tocar), y variantes anchas con índice y `len` por carácter. La variante se elige por lo que llega (clave de la `Variant`). Mayúsculas, categorías y espacios fuera de ASCII: tablas generadas de `unicodedata` de este Python, o puente puro. `bytes` y `bytearray` como `[u8]`. `repr` de textos. `encode` y `decode` UTF-8.

**C8 · Flotantes exactos (M).** `repr` (el más corto que vuelve a dar el mismo número, con las reglas de exponente de Python), `round(x, n)`, `//`, `%`, `divmod`, `**`, `float("...")`, comparar entero con flotante por encima de 2⁵³. `math` entero: las funciones que CPython delega en la biblioteca de C se llaman a esa misma biblioteca; las que CPython implementa por su cuenta (`hypot`, `fsum`, `dist`, `prod`, `gamma`, `lgamma`, `erf`, `factorial`, `comb`, `isqrt`) se portan tal cual. Ojo: `sum()` de flotantes es compensada desde Python 3.12 y simple antes; hay que hacer las dos.

**C9 · Recursión profunda y límites (M; toca el compilador).**
- `DISENO.md` M4: guarda de pila en lugar del tope de 256 llamadas.
- Exactitud de `RecursionError`: una función recursiva lleva la cuenta de profundidad y se detiene al llegar a `sys.getrecursionlimit()` menos la profundidad de entrada (Python repite y da su error). Cuando la recursión baja un entero hasta un caso base, la cota se comprueba una sola vez a la entrada y el contador desaparece.
- `DISENO.md` V1d: arreglos de más de 2²⁴ elementos. Subir también el tope de las tablas.

**C10 · Biblioteca estándar (G, por entregas).** Escrita en Turi en `tools/pyfront/pylib/`, se incluye solo lo que se usa, cada función con sus propiedades en A3. Por orden de uso en el corpus: `math`, `collections` (`deque`, `defaultdict`, `Counter`, `namedtuple`), `heapq`, `bisect`, `itertools`, `functools`, `string`, `sys`, `copy`, `enum`, `dataclasses`, `operator`. Por el puente puro: `re`, `json`, `statistics`, `datetime`. `typing` se ignora.

**C11 · Enteros grandes (M, prioridad baja).** Hoy un desbordamiento detiene la región y Python sigue: es exacto, pero ese programa va a velocidad de Python. Cuando un sitio desborda, se recuerda en el manifiesto y esa variable se recompila como «entero ancho»: 64 bits con escape a un entero grande nativo. Objetivo honesto: ganar a CPython; frente a GMP, publicar lo que salga.

### Fase D · Ganar, no empatar

Cada fila de A2 por debajo de 1,05× es un defecto. Las herramientas son los bloques de `DISENO.md`; aquí va solo a qué programa sirve cada una.

| Programa | Qué lo lleva por encima |
|---|---|
| `mandelbrot.py`, `nbody.py` (1,03× y 1,02× hoy) | V5b (entrelazar vueltas independientes), V4 (búsqueda empírica de variantes) |
| `binarytrees.py`, todo lo que crea objetos | M1 puntos 1 a 4 (nodo de 16 y de 8 bytes, celdas sin contador) |
| `kmers.py`, `wordfreq.py`, `knapsack.py`, `life.py` | M2 (entradas densas, prefetch), V6 (sondeo vectorial) |
| `nbody_clases.py` | V7 (estructura de arreglos automática) |
| `dijkstra.py`, `textproc.py` | V6 (ordenar y buscar vectorial), V3 (contrato con el reservador) |
| `nqueens.py`, `knapsack.py` | V1c (llamadas dirigidas), V9 (convención de llamadas propia) |

Regla: medir con `bench.py -n 9`, mirar el ensamblador del bucle caliente (`tools/mcaudit/mcaudit.py`) antes de proponer nada, y no dar por buena una mejora que solo se ve en un programa.

### Fase E · El programa entero en nativo

**E1 · Ejecutable independiente (M).** Cuando el nivel superior y todas las funciones caben en una sola región con efectos de las tres clases, `run.py --exe` produce un ejecutable sin CPython: desaparecen los 70 ms de arranque y un programa corto gana también en tiempo de proceso. Si se detiene y hay CPython instalado, se relanza con él descontando lo ya emitido (el mismo protocolo de B1 y B4, entre procesos); si no lo hay, error claro con la línea de Python. `bench.py` pasa a exigir ≥ 1,05× en **proceso entero**.

## 5. Orden y puertas

| Paso | Paquetes | Puerta para pasar al siguiente |
|---|---|---|
| 1 | A1, A2, A3, A4 | Hay marcadores. Se publica la línea base. |
| 2 | B1, B2, B3 | Ninguna función del corpus fuera por `print` ni por contagio. Cobertura ≥ 90 %. |
| 3 | B4, B5 | `textproc.py` nativo. Ninguna región más lenta que Python. |
| 4 | C1, C2, C4 | Cobertura ≥ 95 %. `kmers.py`, `life.py`, `knapsack.py`, `nqueens.py` ≥ 1,05×. |
| 5 | C3, C5, C6 | Cobertura ≥ 97 %. `binarytrees.py` y `nbody_clases.py` ≥ 1,05×. |
| 6 | C7, C8, C9, C10 | Cobertura 100 % fuera de la lista cerrada. |
| 7 | Fase D completa | Todas las filas de A2 ≥ 1,05×, también contra el C con intrínsecos. |
| 8 | E1, C11 | Proceso entero ≥ 1,05×. |

La fase D no espera al paso 7: cada paquete de la fase C cierra con su programa de banco medido, y si empata se abre ya su defecto.

## 6. Trampas ya pisadas

- **Una marca global escrita dentro de un bucle mata la vectorización.** Los avisos de error van en una variable local (`tzZ`) que se mira al salir.
- **Tras un `longjmp` no te fíes de las variables locales** (el registro MXCSR se restauraba desde una local pisada: fallo 0xc000008f). Usa estáticas.
- **Cómo salen los resultados cambia el bucle.** Leer seguidos 30 valores al final hizo que LLVM reorganizara todo `nbody` en vectores lentos (700 ms). Las listas cortas modificadas se devuelven copiándolas a una lista reservada con un tamaño que el compilador no conoce.
- **Rodeos del traductor cuestan de 4 a 6 veces:** la variable del `for` declarada arriba, índices por función auxiliar, `//` y `%` con saltos. Escribe lo que escribiría una persona.
- **Renombrar con expresiones regulares** tocó también el código generado. Renombra en el árbol, no en el texto.
- **Tipo de retorno de funciones recursivas:** inferencia optimista y comprobación en cada `return`.
- **Después de `while True`** hay que añadir siempre una salida para que Turi no dé `E_RETURN`.
- **Medir con otro tamaño de entrada** que el de `bench.json` da comparaciones falsas. Medir un lenguaje detrás de otro, también.
- **`PYTHONDONTWRITEBYTECODE=1`** puede estar puesto: el lanzador guarda él mismo el motor compilado.
- **Hilos:** con hilos las llamadas nativas pasan de una en una (`watch_threads`). Mientras corre el nativo no se atiende Ctrl+C.
- **No quites operaciones de propiedad del IR** para ahorrar código (regla 12), y **lee por campos lo que se escribió por campos** (regla 11).

## 7. Fuera de este plan

Instalador (`pip install`), Linux y macOS, documentación en inglés, más modelos y conjuntos públicos de IA, y el compilador escrito en Turi. Están en `DISENO.md` (bloques I5 y P). Nada de este plan depende de ellos, salvo que cada paquete debe evitar suposiciones solo válidas en Windows cuando no cueste nada.

## 8. Estado de la implementación (se actualiza al terminar cada paquete)

25 de noviembre de 2026, noche. Todo sin commit sobre `0685fa5`.

| Paquete | Estado | Dónde |
|---|---|---|
| A1 | Hecho. 60 respuestas del modelo y 40 programas «reales» pedidos a la Spark (`corpus/pedir.py`). Marcador: `cobertura.py` → `COBERTURA.md` | `tools/pyfront/corpus/`, `cobertura.py` |
| A2 | Hecho (10 programas con su C experto). Línea base: los 10 a velocidad de CPython | `benchmarks/python/`, `bench.py` |
| A3, A4 | Pendientes | |
| B1 | Hecho: `print`, `sys.stdout/stderr.write`, cadenas f con especificación, `%`, `str.format`, `repr`, flotantes como Python (2,3 millones de casos sin diferencia). Búfer en el shim, repetición sin duplicar | `turipy.SHIM`, `py2turi.print_call/formatted` |
| B2 | Hecho: cada bucle es una región nativa (también los del nivel superior) | `regiones.py`, `turipy.Region` |
| B4 | En parte: `random` exacto (Mersenne Twister y fórmulas de random.py). Faltan archivos y entrada estándar | `azar.c` |
| B5 | Hecho: cada biblioteca es un módulo de extensión; la frontera está en C (0,18 µs por llamada sencilla, antes ~3 µs). Regla de no empeorar al arrancar (3 comprobaciones) y mientras corre (muestreo de ciclos) | `frontera.c` |
| C1 | Hecho: resultados valor-o-None, centinelas, argumentos None | `py2turi` (`is_opt`, `none_params`) |
| C5 | Hecho como paradas: el cuerpo del `try` corre en nativo; un error detiene y Python repite (con su manejador). `raise` = parada | `py2turi.flatten_try` |
| C3 | Hecho v1: clases de mundo cerrado (sin herencia), dataclass, métodos, constructores, `is None`, campos escalares/texto/objeto/listas. Almacén por clase con índices (-1 = None); en la frontera, mapa de identidad y columnas; al volver se escriben los campos que cambian y se crean solo los objetos alcanzables. Banco: `cuerpos` 0,73× y `arboles` 0,09× frente a C (el almacén no libera temporales) | `clases.py`, `objetos.c` |
| C2, C4, C6–C11, D, E1 | Pendientes. Siguiente: liberar temporales del almacén (arboles), diccionarios con orden y claves tupla, generadores, closures/lru_cache, archivos y stdin | |

Sesión siguiente (sin commit, verificación incompleta): `truncate` en el compilador, tope de arreglos 2^30, arena de objetos, `sys.argv` nativo; `arboles` 0,45× y `criba` 0,89× en cálculo. Estado detallado, diseño de lo que falta y reglas en `docs/RELEVO-GPT.md`.

Fallos encontrados y arreglados por el camino: `__builtin_longjmp` devolvía mal XMM6–15 en Windows (ahora `setjmp` de la CRT); los hilos de `subprocess` ponían todo el motor en modo «con hilos»; `a[f()] += 1` evaluaba el índice dos veces; una función retirada invalidaba para siempre a las que dependían de ella.
