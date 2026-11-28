# Python sobre Turi

Dos herramientas, las dos sobre el mismo compilador:

| Herramienta | Qué hace | Para qué |
|---|---|---|
| `run.py` / `turipy.py` | Ejecuta **cualquier** programa Python. Lo que Turi puede compilar con el mismo resultado corre en nativo; el resto lo ejecuta CPython. | Usar Python tal cual, sin renunciar a nada. |
| `py2turi.py` | Traduce a Turi un subconjunto de Python con anotaciones de tipo. | Medir con `tools/aibench` (adaptador `turipy`) y `replay.py`. |

## Uso

```
python tools/pyfront/run.py [opciones] programa.py [argumentos del programa]
```

| Opción | Efecto |
|---|---|
| `--informe` | Al terminar dice qué funciones fueron nativas, cuáles no y por qué. |
| `--verificar` | Comprueba contra Python **todas** las llamadas nativas (lento; para pruebas). |
| `--exacto` | Coma flotante idéntica bit a bit a CPython (sin fundir `a*b+c` ni reordenar sumas). |
| `--sin-nativo` | Ejecuta solo con Python. |
| `--veloz RUTA` | Compilador de Turi a usar (por defecto, el de `build/`). |
| `--exe RUTA` | Compila la unidad Python completa a un ejecutable en modo exacto. |

La primera vez que se llama a una función se compila (unos segundos); queda guardada en
`%LOCALAPPDATA%\turipy\cache` y las siguientes ejecuciones del mismo programa arrancan directamente.

Para un módulo cerrado con una llamada final a su función de entrada, `python tools/pyfront/run.py --exe programa.exe programa.py` crea un binario que recibe los argumentos del programa. El binario ejecuta el cálculo y la salida en nativo. Si el código nativo se detiene, descarta la salida pendiente y usa el CPython de compilación con una copia del fuente incluida en el ejecutable. Admite la lectura de archivos regulares UTF-8 por la misma vía del motor híbrido.

## Cómo funciona

1. CPython ejecuta el programa entero. Por eso funciona todo: clases, generadores, excepciones,
   decoradores, `eval`, enteros sin límite, cualquier módulo.
2. Cada función de nivel superior se observa la primera vez que se llama: con los tipos reales de esa
   llamada se traduce a Turi junto con las funciones que usa, se compila y se sustituye por la versión
   nativa. No hacen falta anotaciones de tipo.
3. La versión nativa trabaja sobre copias de los datos. Si se detiene (un entero que no cabe en 64
   bits, un índice fuera de rango, una clave que no está, una división entre cero, un `assert`, una
   variable sin valor), no ha cambiado nada: la llamada se repite en Python, que da el resultado o el
   error de siempre, con su traza de siempre.
4. Las primeras llamadas baratas de cada función se comprueban contra Python. Si algo no coincide, esa
   función vuelve a Python y se avisa por la salida de errores.
5. Con listas cortas (hasta 8 elementos) la versión nativa se compila sabiendo sus tamaños reales: el
   compilador desenrolla los bucles y quita comprobaciones. Si luego llegan otros tamaños se añade la
   versión general.

## Qué pasa a nativo y qué no

Pasa a nativo una función que usa: enteros, flotantes, booleanos, textos ASCII; listas de esos tipos,
listas de listas de números y listas de tuplas de números; tuplas de 2 a 4 elementos; diccionarios y
conjuntos como variables locales; `if`, `while`, `for` sobre `range`, listas, textos, `enumerate` y
`zip`; comprensiones de lista; cortes; llamadas a otras funciones del programa; recursión; `len`,
`range`, `int`, `float`, `str`, `bool`, `abs`, `min`, `max`, `sum`, `sorted`, `reversed`, `ord`, `chr`,
`pow`, `divmod`, `any`, `all`, `math.sqrt/floor/ceil/sin/cos/gcd/fabs/pi/e/inf`; los métodos habituales
de textos y listas y constantes numéricas o de texto definidas en el módulo. También se admiten listas
y tuplas literales de escalares homogéneos del módulo que solo se indexan, recorren o consultan con
`len` y `in`. Si una lista cambia de contenido, la variante vuelve a CPython.

`sorted` admite una lambda de un parámetro que devuelve una clave escalar o tupla, con
`reverse=True` o `reverse=False`. Conserva el orden de los empates. En `dict.items()`, puede prescindir
del orden de inserción si la clave de ordenación es pura e incluye la clave original del diccionario.

Se traducen `heapq.heappush`, `heapq.heappop` y `heapq.heapify` sobre listas de enteros, textos,
booleanos o tuplas de esos tipos. El motor comprueba que el módulo y las funciones siguen siendo
los de CPython antes de ejecutar la variante.

Los diccionarios y conjuntos locales admiten claves de dos enteros con signo de 32 bits cada uno
o tres enteros con signo de 21 bits cada uno. Una clave fuera de ese rango hace que CPython
repita la llamada. Los recorridos de conjuntos y diccionarios se traducen cuando solo producen
efectos independientes del orden, como inserciones en un conjunto o sumas enteras.

Las funciones internas con capturas se traducen si se usan por llamada directa y tienen una firma
sencilla. Las capturas se pasan como parámetros. Un `nonlocal` entero inicializado en la función
exterior se puede actualizar en nativo. El motor comprueba el margen de recursión de CPython.

Una función interna pura con dos argumentos enteros, usada en una llamada directa final, admite
`@lru_cache(maxsize=None)`. La recursión se resuelve con una pila explícita y una tabla por estado.
Si cambia el decorador de CPython o se supera el presupuesto de recursión, la llamada se ejecuta en Python.

`with open(ruta, encoding="utf-8") as archivo` admite un `for linea in archivo` sobre un
archivo regular de texto ASCII. El motor lee por bloques de 64 KiB, normaliza los saltos de
línea y cierra el archivo al salir. Las líneas que no caben en el bloque, los casos no
admitidos y los errores de lectura pasan a CPython. También se usa CPython cuando hay
auditoría activa, se ha sustituido `open` o la versión carece de la comprobación de auditoría.
El bucle directo sobre `texto.split()` recorre palabras sin materializar una lista.

Se queda en Python, entre otras cosas (el `--informe` da el motivo exacto de cada función):

- construcciones como las clases con herencia, los generadores, los manejadores de excepciones,
  otros usos de `with`, las funciones como valores, `*args` y los decoradores arbitrarios;
- la entrada estándar y las operaciones de archivo distintas de la iteración indicada;
- dos nombres para la misma lista cuando alguno se modifica, o una lista guardada dentro de otra que
  luego cambia: en Python comparten objeto y una copia no se comportaría igual;
- recorrer un diccionario o un conjunto cuando el resultado depende de su orden en Python;
- convertir un flotante a texto, `round`, `//` y `%` con flotantes;
- textos con caracteres fuera de ASCII y valores que no cruzan la frontera (diccionarios como
  argumento o resultado, `None` como argumento).

## Coma flotante

Por defecto valen las reglas de Turi: puede fundir `a*b+c` en una sola operación y reordenar una suma
acumulada para usar instrucciones vectoriales. El resultado puede diferir del de CPython en los últimos
bits (como le pasa a C entre compiladores). Los errores sí son los de Python: dividir entre cero o sacar
la raíz de un negativo detiene la versión nativa y lo resuelve Python. Con `--exacto` cada operación se
redondea como en CPython y el resultado es idéntico bit a bit. Las acumulaciones de una misma suma
conservan su orden. El traductor puede agrupar sumas independientes para vectorizarlas.

## Velocidad

`python tools/pyfront/bench.py` ejecuta los programas de `examples/` (Python corriente, sin
anotaciones) con las mismas entradas que `benchmarks/run.py` y los compara con el C experto y el Turi
escrito a mano medidos por ese banco. La última medición está en `RESULTADOS.md`. Da dos cifras: el
tiempo de cálculo (las llamadas nativas) y el del proceso entero, que incluye los 70 ms aproximados que
tarda en arrancar CPython con el motor.

Con `--c-extremo`, el banco usa el modo exacto y las fuentes declaradas en `c_extremo` en
`bench.json`. Mide LTO y LTO con PGO cuando hay entradas de entrenamiento registradas.
Cada columna usa la variante de C válida más rápida para esa métrica. El cálculo se cronometra
dentro de ambos procesos y requiere que la función de Turi haya corrido entera en nativo.
También se comprueba que la ejecución instrumentada coincide con CPython.
Los casos sin una referencia extrema aparecen como pendientes.

`bench.py --c-extremo --exe -n 9` usa esta entrada y mide el tiempo entero del binario generado. El tiempo de cálculo se recoge en el propio ejecutable cuando toda la ejecución fue nativa.

## Límites conocidos

- Una función nativa no puede llamar a una que se quedó en Python: si una función usa algo no
  admitido, las que la llaman tampoco se compilan.
- El código de nivel superior del módulo (fuera de funciones) siempre lo ejecuta Python.
- Pasar una lista grande a una función nativa cuesta una copia de ida (y otra de vuelta si la función la
  modifica).
- Mientras corre una función nativa no se atiende Ctrl+C ni corren otros hilos de Python.
- Si el programa usa hilos, las llamadas nativas pasan de una en una y por un camino algo más lento.
- Solo se compilan las funciones del programa principal, no las de los módulos que importa.
- Probado en Windows x64 con CPython 3.11 y 3.13.

## Pruebas

`tests/turipy_hybrid.py` (en `ctest`) ejecuta programas que mezclan lo compilable con lo que debe seguir
en Python, ejecutados con CPython y con el motor: en sus dos modos de coma flotante con `--verificar` y
dos veces más en uso normal (primera ejecución y arranque desde lo guardado). Tienen que coincidir
salida, errores, trazas y código de salida. También comprueba la caché del lanzador al alternar entre
dos copias del motor.
