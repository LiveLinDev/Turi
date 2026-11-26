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

La primera vez que se llama a una función se compila (unos segundos); queda guardada en
`%LOCALAPPDATA%\turipy\cache` y las siguientes ejecuciones del mismo programa arrancan directamente.

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
de textos y listas; constantes numéricas o de texto definidas en el módulo.

Se queda en Python, entre otras cosas (el `--informe` da el motivo exacto de cada función):

- lo que no tiene equivalente con tipos fijos: clases, generadores, `try`, `with`, funciones como
  valores, `*args`, decoradores, variables globales que cambian;
- `print` y cualquier otra entrada o salida;
- dos nombres para la misma lista cuando alguno se modifica, o una lista guardada dentro de otra que
  luego cambia: en Python comparten objeto y una copia no se comportaría igual;
- recorrer un diccionario o un conjunto (su orden aquí todavía no es el de inserción de Python);
- convertir un flotante a texto, `round`, `//` y `%` con flotantes;
- textos con caracteres fuera de ASCII y valores que no cruzan la frontera (diccionarios como
  argumento o resultado, `None` como argumento).

## Coma flotante

Por defecto valen las reglas de Turi: puede fundir `a*b+c` en una sola operación y reordenar una suma
acumulada para usar instrucciones vectoriales. El resultado puede diferir del de CPython en los últimos
bits (como le pasa a C entre compiladores). Los errores sí son los de Python: dividir entre cero o sacar
la raíz de un negativo detiene la versión nativa y lo resuelve Python. Con `--exacto` cada operación se
redondea como en CPython y el resultado es idéntico bit a bit; se pierde la vectorización de sumas.

## Velocidad

`python tools/pyfront/bench.py` ejecuta los programas de `examples/` (Python corriente, sin
anotaciones) con las mismas entradas que `benchmarks/run.py` y los compara con el C experto y el Turi
escrito a mano medidos por ese banco. La última medición está en `RESULTADOS.md`. Da dos cifras: el
tiempo de cálculo (las llamadas nativas) y el del proceso entero, que incluye los 70 ms aproximados que
tarda en arrancar CPython con el motor.

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

`tests/turipy_hybrid.py` (en `ctest`): 18 programas que mezclan lo compilable con lo que debe seguir
en Python, ejecutados con CPython y con el motor: en sus dos modos de coma flotante con `--verificar` y
dos veces más en uso normal (primera ejecución y arranque desde lo guardado). Tienen que coincidir
salida, errores, trazas y código de salida.
