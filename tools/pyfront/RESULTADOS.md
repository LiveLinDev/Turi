# Python sobre Turi frente a C experto

## Línea base del 25 de noviembre de 2026

`python tools/pyfront/bench.py --veloz build-dev2/veloz.exe -n 5` alternó el motor con C y tomó el mínimo de cinco rondas después del calentamiento. Las salidas coincidieron en los 14 programas. El cociente es C dividido por Turi y valores superiores a 1 indican ventaja de Turi.

| Programa | Entrada | Proceso Turi (ms) | Cálculo nativo (ms) | C experto (ms) | Proceso frente a C | Cálculo frente a C |
|---|---:|---:|---:|---:|---:|---:|
| fannkuch | 10 | 220 | 147 | 168,4 | 0,76× | 1,15× |
| mandelbrot | 4000 | 279 | 200 | 198,8 | 0,71× | 1,00× |
| nbody | 20000000 | 694 | 601 | 568,2 | 0,82× | 0,95× |
| spectralnorm | 3000 | 167 | 91 | 278,5 | 1,67× | 3,05× |
| arboles | 16 | 178 | 107 | 45,2 | 0,25× | 0,42× |
| criba | 20000000 | 154 | 68 | 65,2 | 0,42× | 0,96× |
| cuerpos | 2000000 | 178 | 94 | 62,8 | 0,35× | 0,67× |
| dijkstra | 500 | 405 | — | 15,0 | 0,04× | — |
| matmul | 400 | 147 | 62 | 13,7 | 0,09× | 0,22× |
| mochila | 400 | 1550 | <1 (parcial) | 17,1 | 0,01× | — |
| palabras | 2000000 | 522 | 125 (parcial) | 34,3 | 0,07× | — |
| reinas | 11 | 272 | — | 7,6 | 0,03× | — |
| texto | texto.txt | 1060 | — | 102,4 | 0,10× | — |
| vida | 300 | 412 | — | 20,2 | 0,05× | — |

En `mochila` y `palabras` el tiempo nativo corresponde a un fragmento del cálculo. El cociente de proceso entero sí incluye el trabajo que quedó en CPython en ambos casos.

## P1 · `palabras` entero en nativo

Con la misma entrada de 2.000.000 de palabras y cinco rondas alternadas, el proceso pasó de 522 ms a 126 ms. El cálculo completo en nativo tomó 60 ms y el C experto 34,8 ms. Turi queda en 0,58× en cálculo y 0,28× en proceso entero. El informe del motor confirmó una llamada nativa a `main` y la salida coincidió con CPython y con C.

## P2 · `dijkstra` entero en nativo

En cinco rondas alternadas con la cuadrícula de 500, el proceso pasó de 405 ms a 89 ms. El cálculo nativo tomó 25 ms y el C experto 13,1 ms. Turi queda en 0,52× en cálculo y 0,15× en proceso entero. Las cuadrículas de 1, 10 y 500 dieron la salida de CPython sin volver a él durante el cálculo de `main`. El banco también coincidió con la salida de C.

## P3 · `vida` entero en nativo

En cinco rondas alternadas a 300 generaciones, el proceso pasó de 412 a 146 ms. El cálculo nativo tomó 80 ms y el C experto 19,4 ms. Turi obtuvo 0,24× en cálculo y 0,13× en proceso entero. La salida coincidió con CPython a 0, 2 y 300 generaciones y con C en el banco.

## P4 · `reinas` y `mochila` enteros en nativo

En cinco rondas alternadas con 11 reinas, el proceso pasó de 272 a 82 ms. El cálculo nativo tomó 21 ms y el C experto 5,3 ms. Turi obtuvo 0,25× en cálculo y 0,06× en proceso entero. La salida coincidió con CPython a 0, 4 y 11 reinas y con C en el banco.

En cinco rondas alternadas a 400 elementos, `mochila` pasó de 1550 a 424 ms de proceso. El cálculo nativo tomó 260 ms y el C experto 14,2 ms. Turi obtuvo 0,05× en cálculo y 0,03× en proceso entero. La salida coincidió con CPython a 0, 10 y 400 elementos y con C en el banco.

## P5 · `texto` entero en nativo

Con el mismo archivo de 400.000 líneas y nueve vueltas alternadas por versión en modo exacto, el mínimo de proceso bajó de 664 a 255 ms. El cálculo nativo tomó 175 ms y el C experto 76,8 ms de proceso. Turi obtuvo 0,44× en cálculo y 0,30× en proceso entero. La salida coincidió con CPython y con C.

Con la caché caliente, el pico del conjunto residente en Windows fue de 19,7 MiB para Turi por bloques, 19,1 MiB para el motor anterior y 48,2 MiB para C. La variante experimental que leía todo el archivo llegó a 63,2 MiB. Las cifras son las medianas de tres procesos por variante.

La repetición después de P6 dio 219 ms de proceso y 160 ms de cálculo frente a 70,7 ms de C. La salida volvió a coincidir.

La batería en modo exacto coincidió con el C del banco en 13 de 14 salidas. `mandelbrot` a tamaño 4000 difiere en un píxel y su checksum. La diferencia se reproduce con el motor anterior. Ambas versiones exactas coinciden con el mismo C compilado con `-ffp-contract=off`. El modo rápido coincide con el C habitual.

### Banco completo después de P5

`bench.py --exacto -n 9` tomó el mínimo de cada medida en nueve rondas alternadas. Los cocientes son el tiempo de C dividido por el de Turi.

| Programa | Proceso Turi (ms) | Cálculo Turi (ms) | C experto (ms) | Proceso frente a C | Cálculo frente a C |
|---|---:|---:|---:|---:|---:|
| `fannkuch` | 209 | 140 | 162,2 | 0,77× | 1,16× |
| `mandelbrot` | 274 | 202 | 182,2 | 0,66× | 0,90× |
| `nbody` | 1307 | 1226 | 526,8 | 0,40× | 0,43× |
| `spectralnorm` | 295 | 227 | 223,7 | 0,76× | 0,99× |
| `arboles` | 150 | 88 | 38,4 | 0,26× | 0,44× |
| `criba` | 119 | 60 | 46,9 | 0,39× | 0,79× |
| `cuerpos` | 170 | 93 | 56,3 | 0,33× | 0,60× |
| `dijkstra` | 101 | 25 | 13,7 | 0,14× | 0,54× |
| `matmul` | 121 | 48 | 11,1 | 0,09× | 0,23× |
| `mochila` | 399 | 269 | 12,2 | 0,03× | 0,05× |
| `palabras` | 116 | 50 | 31,0 | 0,27× | 0,62× |
| `reinas` | 85 | 20 | 5,1 | 0,06× | 0,25× |
| `texto` | 255 | 175 | 76,8 | 0,30× | 0,44× |
| `vida` | 139 | 69 | 17,2 | 0,12× | 0,25× |

## P6 · Inicialización directa y referencias compactas en `arboles`

Se alternaron las variantes con C a profundidad 16 durante nueve rondas por ensayo. Cada variante usó su propia caché para medir el proceso con el plan ya cargado. Se comparan los mínimos.

- La inicialización directa bajó el proceso de 149,9 a 133,1 ms y el cálculo de 97,5 a 66,8 ms.
- Al reducir los índices de referencia a 32 bits, el proceso bajó de 135,2 a 116,0 ms y el cálculo de 61,9 a 57,8 ms. El C experto tomó 37,4 ms en ese ensayo. Turi obtuvo 0,65× en cálculo y 0,32× en proceso entero.

Todas las salidas coincidieron con CPython. Los dos campos de referencia de cada nodo ocupan en total 8 bytes, frente a 16 bytes en la representación anterior del motor.

La repetición tras integrar ambos cambios dio 121 ms de proceso y 55 ms de cálculo frente a 36,5 ms de C a profundidad 16. La salida volvió a coincidir.

## Medición anterior de los ejemplos

| Programa (Python) | Entrada | Proceso entero (ms) | Cálculo (ms) | C experto (ms) | Turi a mano (ms) | Cálculo frente a C |
|---|---:|---:|---:|---:|---:|---:|
| fannkuch | 10 | 197 | 140 | 167.5 | 152.7 | 1.19x |
| mandelbrot | 4000 | 249 | 180 | 185.8 | 184.2 | 1.03x |
| nbody | 20000000 | 579 | 510 | 520.1 | 487.6 | 1.02x |
| spectralnorm | 3000 | 145 | 83 | 221.1 | 91.4 | 2.65x |

## Primera campaña frente a C extremo · 27 de noviembre de 2026

En el Ryzen 7 7700X con Windows, CPython 3.11 y Clang 23.1.4, `bench.py --c-extremo -n 9` tomó los mínimos de nueve rondas alternadas. El cálculo se mide dentro de cada proceso. Para cada métrica se elige la variante válida de C más rápida entre LTO y LTO con PGO. Se conservaron el orden de las operaciones de coma flotante y la salida de CPython. Los dos binarios C de cada programa coincidieron también en 74 entradas de validación, en total, ajenas al entrenamiento. En `spectralnorm` la multiplicación de 194 matrices por vectores se comprobó también bit a bit.

| Programa | Entrada | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 10 | 215 | 140 | 90,0 | 85,5 | 0,42× | 0,61× |
| `mandelbrot` | 4000 | 283 | 204 | 92,6 | 86,8 | 0,33× | 0,42× |
| `spectralnorm` | 3000 | 296 | 229 | 87,8 | 82,6 | 0,30× | 0,36× |
| `arboles` | 16 | 131 | 58 | 17,8 | 14,1 | 0,14× | 0,24× |
| `criba` | 20000000 | 156 | 81 | 10,5 | 4,7 | 0,07× | 0,06× |
| `matmul` | 400 | 125 | 49 | 8,7 | 3,1 | 0,07× | 0,06× |
| `palabras` | 2000000 | 120 | 50 | 21,4 | 16,6 | 0,18× | 0,33× |
| `reinas` | 11 | 88 | 21 | 5,0 | 0,4 | 0,06× | 0,02× |

Los ocho quedan por debajo de 1,05× en ambas métricas. Quedan por validar referencias extremas para `nbody`, `cuerpos`, `dijkstra`, `mochila`, `texto` y `vida`.

### Ensayo de división entera por potencias de dos

En Ubuntu se comparó el traductor actual con un descenso de `//` y `%` por literales positivos a desplazamientos y máscaras. Con `spectralnorm 3000`, el cálculo pasó de 231,8 a 372,1 ms en nueve rondas alternadas. La ruta con desplazamiento dejó dos comprobaciones de desbordamiento dentro del bucle y perdió la vectorización que aparece en la variante base. Se descartó ese cambio.

### Agrupación de filas e intercambio de bucles

Se compararon dos copias del motor con los programas Python del banco y cachés independientes. En nueve rondas alternadas, el mínimo de cálculo de `spectralnorm 3000` bajó de 227,5 a 85,2 ms. El de `matmul 400` bajó de 50,5 a 23,0 ms. Los mínimos de proceso pasaron de 297,2 a 149,8 ms y de 123,9 a 100,0 ms, respectivamente.

El pico residente de `spectralnorm` fue de 13,79 MiB en ambas copias. En `matmul` fue de 18,39 y 18,30 MiB. Son las medianas de cinco rondas alternadas con la caché caliente. Los cambios del traductor se aplican en modo exacto y conservan el programa Python original.

Se repitió la comparación con C extremo después de integrar ambos cambios. Los ocho programas con referencia verificada siguen por debajo de 1,05×.

| Programa | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `spectralnorm 3000` | 161 | 88 | 89,5 | 84,3 | 0,55× | 0,96× |
| `matmul 400` | 102 | 24 | 8,4 | 3,3 | 0,08× | 0,14× |

PGO en el motor híbrido se ensayó con tres tamaños distintos de 3000. En nueve rondas alternadas, el cálculo mínimo fue de 85,89 ms con la compilación habitual y 86,05 ms con PGO. La variante con perfil quedó descartada.

También se probaron las preferencias de ancho SIMD de Clang en `spectralnorm 3000`. En nueve rondas alternadas, los mínimos de cálculo fueron 84,03 ms con el ajuste habitual y 85,25, 86,40 y 84,83 ms con 128, 256 y 512 bits. Las cuatro medianas quedaron entre 87,06 y 87,26 ms. Se conservó el ajuste habitual.

### Resto del banco frente a C extremo

Se completaron las 14 comparaciones con el mismo protocolo. Las salidas de los ejecutables de C con LTO y con PGO coincidieron con CPython en 134 parejas de programa y entrada ajenas al entrenamiento. La simulación vectorizada de cuerpos se cotejó además con la escalar bit a bit para diez duraciones. El banco de `texto` es ASCII y su entrenamiento usa archivos de tamaños y semillas distintos.

| Programa | Entrada | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|---:|
| `dijkstra` | 500 | 89 | 24 | 6,0 | 1,3 | 0,07× | 0,05× |
| `mochila` | 400 | 398 | 245 | 4,5 | 0,1 | 0,01× | <0,01× |
| `vida` | 300 | 132 | 66 | 5,2 | 0,7 | 0,04× | 0,01× |
| `nbody` | 20000000 | 1321 | 1243 | 585,0 | 580,2 | 0,44× | 0,47× |
| `cuerpos` | 2000000 | 163 | 94 | 63,3 | 58,2 | 0,39× | 0,62× |
| `texto` | 400000 líneas | 241 | 182 | 70,3 | 62,8 | 0,29× | 0,34× |

La ventaja exigida de 1,05× en cálculo y proceso entero queda pendiente en los 14 programas. El cálculo más cercano es `spectralnorm`, con 0,96×.

### Ensayo de ejecutable independiente

Un prototipo compiló el Python original de `spectralnorm` en modo exacto. Conservó la salida de CPython sin argumentos y en siete tamaños adicionales. La tabla de importaciones del ejecutable de Windows confirmó que puede arrancar sin CPython.

En nueve rondas alternadas a tamaño 3000, Turi tomó 89,01 ms de proceso y 84,80 ms de cálculo. Los mejores tiempos de C extremo en la misma tanda fueron 87,69 y 82,50 ms. Para E1 quedan la integración en el lanzador y la recuperación de errores.

### Formato nativo y referencia reforzada de `spectralnorm`

Tras corregir el tipo de `"%.9f" % x`, se repitieron los bancos que usan ese formato. La referencia de `spectralnorm` pasó a compartir una división por cada par de vectores de denominadores, con corrección del redondeo por FMA. La variante de pares coincidió bit a bit con la división directa en 8.388.608 valores. La referencia integrada también se cotejó con 194 productos y cuatro bloques cerca del límite de su guarda, y con CPython en 11 tamaños ajenos al entrenamiento de PGO.

| Programa | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `spectralnorm 3000` | 148 | 85 | 64,7 | 59,5 | 0,44× | 0,70× |
| `nbody 20000000` | 1309 | 1228 | 579,8 | 575,9 | 0,44× | 0,47× |
| `cuerpos 2000000` | 156 | 92 | 62,2 | 56,9 | 0,40× | 0,62× |

Un prototipo SIMD en Turi con el mismo método tomó 66,47 ms de proceso y 61,34 ms de cálculo. En esa misma tanda, C extremo tomó 66,49 y 61,25 ms. Se comprobó la salida en diez tamaños, incluidos los límites de los bloques. Queda generar esta variante a partir del Python de entrada. Se mantuvo el cálculo por pares tras ensayar cuatro recíprocos por división y `rcp14` con corrección, con mínimos de cálculo de 64,30 y 74,48 ms en sus respectivas tandas.

### Campaña de los ejecutables independientes

El traductor ya genera los recíprocos por pares para un denominador entero de grado dos cuando demuestra la cota del redondeo. En una comparación con los dos motores híbridos y cachés independientes, `spectralnorm 3000` bajó de 86,02 a 59,39 ms de cálculo. Las medianas del pico residente fueron 13,81 y 13,87 MiB. Las dos versiones coincidieron con CPython en ocho tamaños de validación.

Con `bench.py --c-extremo --exe -n 9` se ejecutaron los 14 bancos enteros en nativo con los tamaños oficiales. Se comprobó la salida contra CPython en cada ronda. Los tiempos son mínimos, con el reloj interno para cálculo y con el arranque incluido para proceso.

| Programa | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 147 | 139 | 89,1 | 83,8 | 0,61× | 0,60× |
| `mandelbrot` | 210 | 204 | 90,3 | 86,0 | 0,43× | 0,42× |
| `nbody` | 1363 | 1415 | 602,2 | 619,9 | 0,44× | 0,44× |
| `spectralnorm` | 76 | 69 | 82,4 | 68,8 | 1,08× | 1,00× |
| `arboles` | 72 | 59 | 19,6 | 14,3 | 0,27× | 0,24× |
| `criba` | 68 | 47 | 8,9 | 4,6 | 0,13× | 0,10× |
| `cuerpos` | 102 | 94 | 63,6 | 57,3 | 0,62× | 0,61× |
| `dijkstra` | 36 | 30 | 6,9 | 1,3 | 0,19× | 0,04× |
| `matmul` | 26 | 21 | 7,9 | 3,0 | 0,30× | 0,14× |
| `mochila` | 247 | 243 | 4,1 | 0,1 | 0,02× | <0,01× |
| `palabras` | 65 | 58 | 22,3 | 16,8 | 0,34× | 0,29× |
| `reinas` | 28 | 23 | 5,3 | 0,4 | 0,19× | 0,02× |
| `texto` | 186 | 192 | 69,7 | 63,2 | 0,37× | 0,33× |
| `vida` | 86 | 79 | 4,9 | 0,7 | 0,06× | 0,01× |

`spectralnorm` superó el umbral de 1,05× en proceso y empató en cálculo en esta tanda. Las 14 filas quedan pendientes del umbral en las dos métricas.

Una vez ajustados los saltos de línea de Windows, se comparó la salida byte por byte en las entradas de prueba de los 14 ejecutables. Con `spectralnorm 3000`, la mediana del pico residente bajó de 13,80 MiB en el híbrido a 3,15 MiB en el ejecutable. En cinco rondas alternadas los mínimos de proceso fueron 152,11 y 69,31 ms, respectivamente. El ensayo de emparejar recíprocos de columnas contiguas conservó la salida y quedó por detrás de agrupar filas en Turi y en C.

### Mochila 0/1 con cota fraccionaria

El traductor reconoce la recurrencia entera de selección 0/1. Usa una solución factible y una cota fraccionaria para fijar qué objetos entran o quedan fuera. Una fila SIMD resuelve la capacidad restante. Las entradas fuera de la prueba conservan la versión memorizada. El banco construye ambas referencias extremas en C, fila completa y cota fraccionaria, con LTO y PGO. Cada métrica usa la más rápida.

En la primera tanda con el traductor integrado, `mochila 400` obtuvo 1,06× en proceso y 1,07× en cálculo. Las dos repeticiones de 17 rondas con binarios recompilados dieron los siguientes mínimos.

| Tanda | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| Segunda | 5,76 | 0,0241 | 5,46 | 0,0289 | 0,95× | 1,20× |
| Tercera | 5,72 | 0,0273 | 5,55 | 0,0256 | 0,97× | 0,94× |

El cálculo del motor pasó de unos 243 ms a menos de 0,03 ms en las repeticiones. La ventaja en las dos métricas sigue pendiente de reproducirse.

### Selección ponderada y búsqueda con máscaras

En 19 rondas alternadas, los mínimos de cálculo en `mochila 400` fueron 0,0217 ms con ordenación y 0,0155 ms con selección de la densidad crítica. La misma selección en C extremo dio 0,0098 ms. Los ocho ejecutables coincidieron byte a byte con CPython en doce tamaños. En otro ensayo, el enlace estático fue más rápido que el de la DLL en Windows para Turi y C. El PGO no dio una mejora reproducida para el ejecutable de Turi.

En `reinas` se sustituyen los tres conjuntos locales por máscaras y se comprueban directamente los dos órdenes posibles de las últimas filas. El ensayo comparó las tres variantes C y dos de Turi en 14 tamaños. Con ambos cambios integrados y los tamaños oficiales, la tanda siguiente alternó 17 rondas.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `mochila 400` | 4,37 | 0,0167 | 4,47 | 0,0103 | 1,02× | 0,62× |
| `reinas 11` | 5,70 | 0,3312 | 5,32 | 0,3169 | 0,93× | 0,96× |

El pico residente se midió en cinco rondas alternadas en Windows. En `reinas 11` las medianas fueron 9,06 MiB con CPython, 3,12 MiB con Turi y 3,11 MiB con C extremo con cierre y PGO. En `mochila 400` fueron 205,46, 3,13 y 3,12 MiB con la referencia C de selección y PGO.

### Salida byte a byte en los 14 bancos

Se repitió `bench.py --c-extremo --exe -n 9` en el Ryzen con Windows. Los 14 ejecutables completaron su tamaño oficial en nativo y coincidieron byte a byte con CPython. Se eligió la referencia C válida más rápida por métrica entre las opciones manuales con LTO y PGO. Ninguna fila alcanzó 1,05× en las dos métricas en esta campaña.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 151,9 | 145,0 | 91,2 | 85,1 | 0,60× | 0,59× |
| `mandelbrot` | 212,0 | 207,7 | 92,2 | 87,6 | 0,43× | 0,42× |
| `nbody` | 1488,4 | 1442,4 | 627,4 | 622,6 | 0,42× | 0,43× |
| `spectralnorm` | 66,9 | 60,8 | 65,3 | 61,0 | 0,98× | 1,00× |
| `arboles` | 61,9 | 59,5 | 19,0 | 13,0 | 0,31× | 0,22× |
| `criba` | 65,7 | 69,5 | 9,85 | 5,20 | 0,15× | 0,07× |
| `cuerpos` | 98,1 | 93,1 | 64,5 | 57,7 | 0,66× | 0,62× |
| `dijkstra` | 31,3 | 27,9 | 6,11 | 1,23 | 0,20× | 0,04× |
| `matmul` | 32,3 | 23,9 | 8,26 | 3,46 | 0,26× | 0,15× |
| `mochila` | 4,50 | 0,0212 | 4,55 | 0,0108 | 1,01× | 0,51× |
| `palabras` | 64,7 | 53,4 | 22,8 | 16,5 | 0,35× | 0,31× |
| `reinas` | 5,28 | 0,3758 | 4,63 | 0,3168 | 0,88× | 0,84× |
| `texto` | 195,0 | 193,5 | 69,9 | 68,3 | 0,36× | 0,35× |
| `vida` | 87,6 | 84,0 | 5,43 | 0,7540 | 0,06× | 0,01× |

### Reutilización en norma espectral y producto de matrices

Se probó reutilizar los recíprocos de un bloque de 16 o 32 filas al aplicar una matriz y su transpuesta. Las nueve variantes coincidieron con CPython en 13 tamaños. En C se verificaron también cinco vectores por tamaño bit a bit. El cálculo de `spectralnorm 3000` pasó de 65,37 a 118,78 ms con la variante Turi de fusión más rápida. En C pasó de 65,36 a 69,02 ms. Se conservó el recorrido por pares en ambos.

En `matmul 400`, detectar filas y columnas repetidas redujo el cálculo del prototipo Turi de 28,94 a 4,31 ms, con la salida exacta en los 11 tamaños de ensayo. C con bloques tomó 4,07 ms y con períodos, 0,42 ms. Se integraron la detección dinámica en Turi y la variante periódica en la referencia C.

Con la integración, `bench.py --c-extremo --exe -n 19 matmul` dio 9,15 ms de proceso y 3,26 ms de cálculo con Turi, frente a 5,71 y 0,3950 ms con el C periódico con PGO. Las medianas de pico residente en cinco rondas fueron 27,96 MiB en CPython, 7,31 MiB con el producto general de Turi, 7,41 MiB con períodos en Turi y 3,11 MiB con el C periódico.

### Catálogo de palabras y criba por cocientes

El prototipo de `palabras 2000000` bajó de 57,95 a 14,12 ms de cálculo y de 35,69 a 3,12 MiB de pico residente. Su primera tanda superó 1,05× en las dos métricas. Se añadió a C la selección variable de carril. Con esa referencia, las dos repeticiones dieron 1,02× y 1,03× en proceso, y 1,01× y 1,02× en cálculo. Los binarios coincidieron con CPython en el tamaño oficial y en diez tamaños adicionales. El ejecutable ahora genera el catálogo cuando puede demostrar la equivalencia.

En `criba 20000000` se pasó de 69,27 a 0,31 ms de cálculo y de 21,73 a 3,12 MiB de pico residente en el prototipo. C con cocientes llegó a 0,2694 ms. Los resultados coincidieron con CPython en 18 tamaños. La identidad de conteo y suma se comprobó también con una enumeración directa en 1.200 límites. El patrón de consumo de la lista de primos se verifica antes de generar la variante.

Tras integrar ambos patrones, se hicieron dos tandas independientes de 19 rondas alternadas. Los ejecutables conservaron la salida byte a byte.

| Banco y tanda | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `palabras` primera | 20,0 | 14,5 | 20,9 | 14,8 | 1,04× | 1,03× |
| `palabras` segunda | 19,7 | 14,3 | 20,0 | 14,5 | 1,02× | 1,02× |
| `criba` primera | 5,62 | 0,2869 | 5,46 | 0,2894 | 0,97× | 1,01× |
| `criba` segunda | 5,58 | 0,3083 | 5,25 | 0,2828 | 0,94× | 0,92× |

En otro ensayo se anticipó la selección del siguiente estado, se estrecharon los contadores a 32 bits y se probó seleccionar también el índice en SIMD. El mínimo de la ruta integrada fue 13,95 ms. El siguiente fue 13,99 ms con dos selecciones en SIMD. La referencia C más rápida dio 14,51 ms. Se conservó la ruta integrada.

### Campaña con los conteos integrados

Se repitió `bench.py --c-extremo --exe -n 9` con los tamaños oficiales y las referencias nuevas. Los 14 ejecutables completaron el cálculo en nativo y coincidieron byte a byte con CPython. Las referencias C de `criba`, `matmul`, `mochila`, `palabras` y `reinas` compiten en varias variantes de código con LTO y PGO.

| Banco | Entrada | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 10 | 153,6 | 146,7 | 91,8 | 86,3 | 0,60× | 0,59× |
| `mandelbrot` | 4000 | 227,3 | 217,2 | 96,1 | 94,6 | 0,42× | 0,44× |
| `nbody` | 20000000 | 1362,1 | 1310,1 | 623,8 | 607,6 | 0,46× | 0,46× |
| `spectralnorm` | 3000 | 71,3 | 64,3 | 69,0 | 63,1 | 0,97× | 0,98× |
| `arboles` | 16 | 79,4 | 72,6 | 22,4 | 15,6 | 0,28× | 0,21× |
| `criba` | 20000000 | 6,82 | 0,3724 | 6,67 | 0,3363 | 0,98× | 0,90× |
| `cuerpos` | 2000000 | 104,3 | 99,9 | 66,0 | 59,4 | 0,63× | 0,59× |
| `dijkstra` | 500 | 35,8 | 29,6 | 7,15 | 1,27 | 0,20× | 0,04× |
| `matmul` | 400 | 10,7 | 4,03 | 6,82 | 0,4749 | 0,64× | 0,12× |
| `mochila` | 400 | 5,24 | 0,0171 | 4,84 | 0,0100 | 0,92× | 0,58× |
| `palabras` | 2000000 | 20,0 | 14,6 | 20,5 | 15,1 | 1,03× | 1,03× |
| `reinas` | 11 | 5,94 | 0,4003 | 4,95 | 0,3146 | 0,83× | 0,79× |
| `texto` | texto.txt | 232,9 | 207,2 | 78,1 | 68,5 | 0,34× | 0,33× |
| `vida` | 300 | 90,8 | 85,2 | 6,24 | 0,7341 | 0,07× | 0,01× |

La ventaja reproducible de al menos 1,05× en proceso y cálculo sigue pendiente para los 14 bancos.

### Parejas de permutaciones, diagonales y bloques de celdas

La variante de `fannkuch` procesa cuatro permutaciones empaquetadas. Pasó de 143,71 a 31,49 ms de cálculo en la prueba alternada. El C con el mismo agrupamiento registró 28,67 ms. `criba` agrupa los intervalos con el mismo cociente y pasó de 0,291 a 0,249 ms frente a 0,203 ms del C con recíprocos.

En `palabras`, reconstruir únicamente las claves del prefijo ordenado ahorra construir el diccionario de cadenas. Dos tandas de 23 rondas con las razones sin redondear dieron 1,02071× y 1,08567× en proceso, y 1,04265× y 1,04761× en cálculo. El margen de 5 % sigue pendiente.

| Banco | Entrada | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|---:|
| `dijkstra` | 500 | 6,43 | 0,0017 | 5,71 | 0,0051 | 0,89× | 3,00× |
| `vida` | 300 | 5,28 | 0,1702 | 4,99 | 0,1550 | 0,94× | 0,91× |

Cada fila corresponde a 23 rondas alternadas con la salida byte a byte. Las referencias incluyen la suma por diagonales y las triadas por filas con bloques activos, respectivamente, en LTO y PGO. Las pruebas dirigidas de Windows comparan además el programa original y una variante con pesos o reglas distintos para comprobar la ruta general.

### Campaña con escape, cotas de salida y muestras SIMD

`bench.py --c-extremo --exe -n 11` verificó byte a byte las 14 salidas en los tamaños oficiales. Todos los ejecutables emitieron el tiempo de cálculo nativo. Para cada métrica se tomó el mínimo del C extremo válido, con LTO y PGO y entradas de entrenamiento distintas de las medidas.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 37,80 | 31,47 | 35,25 | 29,21 | 0,93× | 0,93× |
| `mandelbrot` | 44,06 | 39,37 | 45,62 | 39,81 | 1,04× | 1,01× |
| `nbody` | 1244,27 | 1257,95 | 590,96 | 586,86 | 0,47× | 0,47× |
| `spectralnorm` | 29,41 | 24,20 | 29,39 | 23,40 | 1,00× | 0,97× |
| `arboles` | 59,85 | 56,63 | 17,04 | 12,47 | 0,28× | 0,22× |
| `criba` | 4,87 | 0,2518 | 4,25 | 0,1875 | 0,87× | 0,74× |
| `cuerpos` | 101,48 | 95,31 | 62,66 | 57,74 | 0,62× | 0,61× |
| `dijkstra` | 4,88 | 0,0015 | 4,96 | 0,0043 | 1,02× | 2,87× |
| `matmul` | 8,19 | 3,11 | 5,02 | 0,4242 | 0,61× | 0,14× |
| `mochila` | 5,30 | 0,0185 | 4,90 | 0,0116 | 0,93× | 0,63× |
| `palabras` | 10,27 | 4,52 | 8,90 | 4,17 | 0,87× | 0,92× |
| `reinas` | 5,72 | 0,3379 | 4,97 | 0,3165 | 0,87× | 0,94× |
| `texto` | 197,65 | 185,32 | 71,97 | 61,91 | 0,36× | 0,33× |
| `vida` | 5,33 | 0,1765 | 5,29 | 0,1551 | 0,99× | 0,88× |

Ningún banco superó el margen de 1,05× en ambas métricas con los tiempos sin redondear. La tabla precede al cambio de las rondas de cinco cuerpos.

### Cinco cuerpos, productos de generadores periódicos y texto por bloques

| Banco | Entrada | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|---:|
| `nbody` | 20000000 | 534,33 | 525,93 | 515,45 | 510,17 |
| `cuerpos` | 2000000 | 57,93 | 51,92 | 56,69 | 50,63 |
| `matmul` primera | 400 | 4,57 | 0,2462 | 4,59 | 0,2126 |
| `matmul` segunda | 400 | 4,72 | 0,2454 | 4,71 | 0,2216 |
| `texto` | texto.txt | 43,58 | 36,35 | 34,71 | 30,95 |

Las referencias incluyeron las cuatro rondas por cuerpo, las teselas periódicas y el recuento con claves de ocho bytes. La salida fue idéntica byte a byte en los cinco ensayos. Las posiciones y velocidades completas de las dos representaciones de cuerpos se compararon también con CPython en diez duraciones. El ensayo que agrupó las comprobaciones de división por cero recuperó correctamente el error para cuerpos coincidentes. Aumentó el tiempo de cálculo de 51,98 a 54,03 ms en el mejor grupo probado, por lo que se conservó la comprobación inmediata.

El prototipo de seis permutaciones por grupo en `fannkuch` dio 28,30 ms de cálculo frente a 30,54 ms de la variante integrada y 26,97 ms del C con seis permutaciones. La tabla compacta de nodos y el recorrido de dos niveles de `arboles` bajaron de 54,40 a 20,38 ms en el prototipo, frente a 9,83 ms del C con índices de 32 bits. Ambos prototipos coincidieron con CPython en diez entradas.

### Memoria residente

Mediana de cinco ejecuciones independientes por programa en Windows.

| Banco | CPython (MiB) | Turi (MiB) | C extremo (MiB) |
|---|---:|---:|---:|
| `matmul` | 27,93 | 3,15 | 3,18 |
| `palabras` | 118,26 | 3,14 | 3,12 |
| `texto` | 9,17 | 3,26 | 24,56 |
| `vida` | 9,98 | 3,15 | 3,11 |

El lector por bloques conserva un búfer de 64 KiB. La referencia C más rápida de `texto` conserva el archivo entero para facilitar la tokenización SIMD. Los generadores periódicos de `matmul` materializan 97 filas de cada matriz en la entrada de 400. La tabla de `vida` usa dos búferes automáticos y limita la ruta densa a 1024 generaciones.

### Campaña con árboles compactos, rueda de 210 y seis permutaciones

Se repitió `bench.py --c-extremo --exe -n 13` en Windows con los 14 tamaños oficiales. Se verificaron las salidas byte a byte frente a CPython y los marcadores de cálculo nativo. Cada columna de C toma el mínimo de las variantes válidas para esa métrica, con LTO y PGO entrenado en otras entradas.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 34,27 | 29,00 | 32,22 | 27,12 |
| `mandelbrot` | 45,83 | 39,93 | 46,20 | 39,51 |
| `nbody` | 619,66 | 575,11 | 569,56 | 534,55 |
| `spectralnorm` | 34,08 | 24,54 | 31,73 | 25,04 |
| `arboles` | 6,34 | 1,4247 | 6,40 | 1,0369 |
| `criba` | 5,46 | 0,2450 | 5,20 | 0,1675 |
| `cuerpos` | 60,31 | 54,88 | 58,00 | 53,03 |
| `dijkstra` | 5,01 | 0,0014 | 4,73 | 0,0044 |
| `matmul` | 5,47 | 0,2408 | 4,98 | 0,2201 |
| `mochila` | 4,87 | 0,0118 | 4,82 | 0,0100 |
| `palabras` | 9,25 | 4,0713 | 8,67 | 3,4936 |
| `reinas` | 5,64 | 0,2912 | 5,25 | 0,2774 |
| `texto` | 49,05 | 43,15 | 40,82 | 37,23 |
| `vida` | 5,18 | 0,1720 | 5,02 | 0,1554 |

El cálculo de `dijkstra` alcanza 3,14× frente a la referencia de suma cerrada. El criterio de 1,05× en ambas métricas sigue pendiente en los 14 programas. La verificación de gramática con `fmt --strict` pasó en Windows y en una réplica local de Ubuntu, con 39 entradas válidas, 33 inválidas y 4 secuencias UTF-8 inválidas.

### Series por bloques, permutación inversa y regiones interiores

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `spectralnorm` | 5,5570 | 0,6920 | 5,5682 | 0,6637 |
| `fannkuch` | 31,7557 | 25,2489 | 32,0943 | 25,8626 |
| `mandelbrot` | 41,2460 | 35,5455 | 40,3319 | 35,0296 |
| `dijkstra` | 4,8583 | 0,0010 | 4,7727 | 0,0043 |

Se tomaron los mínimos de 19 rondas con las entradas oficiales y la salida byte a byte. Las referencias incluyen cada optimización de la tabla. Pasar el argumento entero directamente al punto de entrada reduce el trabajo de `dijkstra`. Esta ruta evita construir la lista de argumentos cuando la función solo usa `int(sys.argv[1])` con un valor predeterminado. `fannkuch` alcanza 1,0107× en proceso y 1,0243× en cálculo. El margen de 1,05× sigue pendiente en las dos medidas simultáneas.

Las parejas de palabras bajaron el cálculo de 4,0713 a 3,4787 ms en las dos tandas oficiales. En la segunda, el C extremo con saltos de dos palabras y PGO marcó 2,9579 ms de cálculo y 8,2233 ms de proceso frente a 8,4792 ms de Turi, con las mismas salidas.

El ensayo del residuo de Rayleigh redujo de ocho a cuatro las aplicaciones de `AᵀA` en la entrada de 3000. En la misma tanda, Turi pasó de 0,6952 a 0,3724 ms de cálculo. El C extremo más rápido con el residuo dio 0,3524 ms. El cálculo coincidió byte a byte en 20 tamaños frente a CPython y en diez tamaños mayores frente a la versión con intervalos previamente verificada.

El árbol con series, parejas de palabras y recíprocos SIMD pasó las 47 pruebas de la réplica Ubuntu 24.04, con CPython 3.12 y Clang 23. También pasó la prueba de gramática con sus dependencias fijadas. La batería anterior de Windows pasó 49 de 49 pruebas y se repitieron allí las comprobaciones dirigidas de los ejecutables tras integrar las series y las regiones interiores.

### Margen de Fannkuch con la referencia de grupos por primer valor

Dos tandas independientes de 23 rondas con `fannkuch 10`. Cada una cotejó la salida de todos los candidatos con CPython. Tanto Turi como la referencia C más rápida agrupan las 24 permutaciones por primer valor y reducen los acumuladores al final.

| Tanda | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 24,3855 | 18,7300 | 26,1786 | 20,8014 | 1,07353× | 1,11059× |
| 2 | 23,6171 | 19,0546 | 25,3257 | 20,3098 | 1,07235× | 1,06587× |

`fannkuch` superó el margen de 1,05× en las dos métricas frente a la referencia de estas tandas. La campaña posterior añade una referencia C que entrelaza dos grupos.

En la segunda tanda, `reinas` quedó en 0,1977 ms de cálculo y 5,0200 ms de proceso en Turi, frente a 0,1702 y 4,2952 ms en C extremo con cierre de cuatro filas y BMI2. El ejecutable independiente con nombre Unicode pasó la prueba de Windows, que comprueba también la recuperación de la excepción y salidas por ambos flujos.

### Campaña completa con el nuevo orden de permutaciones y la siembra espectral

En 13 rondas alternadas en Windows con los tamaños oficiales, los 14 ejecutables emitieron el marcador nativo y coincidieron byte a byte con CPython. Cada métrica usa el mínimo de las referencias C válidas, con LTO y PGO entrenado en otras entradas.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 23,2667 | 18,5638 | 26,0513 | 20,4498 |
| `mandelbrot` | 40,8565 | 36,0254 | 40,9387 | 35,4456 |
| `nbody` | 550,0066 | 544,1283 | 528,6062 | 522,1877 |
| `spectralnorm` | 5,2001 | 0,2874 | 4,7773 | 0,2679 |
| `arboles` | 6,4443 | 1,3993 | 5,6429 | 1,0391 |
| `criba` | 5,0128 | 0,2448 | 4,7665 | 0,1580 |
| `cuerpos` | 58,9960 | 52,0602 | 57,1247 | 50,9011 |
| `dijkstra` | 4,6207 | 0,0009 | 4,4543 | 0,0042 |
| `matmul` | 5,0537 | 0,2431 | 4,7361 | 0,2169 |
| `mochila` | 4,7344 | 0,0138 | 4,2591 | 0,0102 |
| `palabras` | 8,6870 | 3,5703 | 7,9542 | 2,9480 |
| `reinas` | 4,6918 | 0,1946 | 4,6282 | 0,1702 |
| `texto` | 46,4873 | 40,8835 | 36,7131 | 32,5231 |
| `vida` | 4,8910 | 0,1424 | 4,9056 | 0,1243 |

Esta tanda reproduce el margen de `fannkuch` frente al C de grupos por primer valor, con 1,11968× en proceso y 1,10160× en cálculo. `dijkstra` llega a 4,67× en cálculo y 0,96× en proceso. La siembra con la primera fila reduce `spectralnorm` a 0,2874 ms y su variante C equivalente marca 0,2679 ms. El cierre de cuatro filas y las rutas de ejecutable, texto y contadores pasaron la comprobación dirigida en Windows.

### Divisores coprimos, órbitas y ocho filas por vector

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `criba` | 4,6071 | 0,1194 | 4,4002 | 0,0924 |
| `cuerpos` | 52,3317 | 46,1193 | 51,0597 | 45,9566 |
| `vida` | 4,5630 | 0,0739 | 4,6095 | 0,0696 |

Mínimos de 23 rondas en Windows. Las referencias C incluyen las mejoras correspondientes. Se compararon las 30 magnitudes del estado de `cuerpos` y `nbody` frente a CPython en diez duraciones. El prototipo de `criba` coincidió con CPython en doce tamaños y con la referencia de cocientes en el límite de 2³⁰.

La comprobación dirigida del ejecutable, los contadores, el texto y las máscaras de reinas pasó también en Ubuntu 24.04. Los seis bancos medidos allí terminaron en nativo con las salidas exactas. `fannkuch` marcó 20,5186 ms de proceso y 19,1169 ms de cálculo, frente a 20,5364 y 19,3440 ms de C extremo con grupos por primer valor.

### Campaña con franjas certificadas y referencias actualizadas

En Windows se midieron los tamaños oficiales en 13 rondas alternadas. Las 14 salidas coincidieron byte a byte con CPython y los ejecutables emitieron su tiempo nativo. Se relanzó `arboles` tras corregir una definición duplicada en su referencia C. Cada tiempo de C es el mínimo entre las referencias válidas, compiladas con LTO y también con PGO entrenado en otras entradas.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 24,0629 | 18,8242 | 24,1217 | 18,4825 | 1,0024 | 0,9818 |
| `mandelbrot` | 25,2316 | 19,4776 | 25,4980 | 19,3078 | 1,0106 | 0,9913 |
| `nbody` | 485,2443 | 468,2081 | 485,6721 | 472,2312 | 1,0009 | 1,0086 |
| `spectralnorm` | 5,1273 | 0,1223 | 4,8533 | 0,1022 | 0,9466 | 0,8357 |
| `arboles` | 6,3691 | 0,6154 | 6,1532 | 0,5787 | 0,9661 | 0,9404 |
| `criba` | 5,3868 | 0,1184 | 5,1751 | 0,0872 | 0,9607 | 0,7365 |
| `cuerpos` | 52,2214 | 46,2167 | 53,4779 | 46,9150 | 1,0241 | 1,0151 |
| `dijkstra` | 4,8994 | 0,0010 | 5,0622 | 0,0045 | 1,0332 | 4,5000 |
| `matmul` | 5,9761 | 0,2858 | 5,2504 | 0,2249 | 0,8786 | 0,7869 |
| `mochila` | 5,8261 | 0,0114 | 4,8057 | 0,0098 | 0,8249 | 0,8596 |
| `palabras` | 9,4893 | 3,5317 | 9,1113 | 3,2595 | 0,9602 | 0,9229 |
| `reinas` | 5,7267 | 0,1392 | 5,2353 | 0,1048 | 0,9142 | 0,7529 |
| `texto` | 46,1452 | 39,5997 | 37,2450 | 32,8815 | 0,8071 | 0,8303 |
| `vida` | 5,3476 | 0,0924 | 5,1794 | 0,0736 | 0,9685 | 0,7965 |

Ninguno de los 14 bancos alcanzó 1,05× en ambas métricas. El C con dos grupos entrelazados dejó atrás la referencia usada al confirmar el primer margen de `fannkuch`. La tabla de franjas y el cierre de cinco filas de `reinas` se evaluaron también en las referencias C equivalentes. En Windows pasaron las pruebas dirigidas del ejecutable, contadores, texto y máscaras de reinas con los cambios de esta campaña.

En una tanda posterior de 23 rondas, la ruta que certifica `spectralnorm` con un solo producto confirmó la salida de CPython para 3000 filas. Turi midió 4,9774 ms de proceso y 0,0696 ms de cálculo. C extremo midió 4,8711 y 0,0532 ms, con el nuevo certificado habilitado en la variante más rápida de cálculo.

### Franjas de 512, teselas por ciclos y validación del texto por bloques

La batería de Windows pasó las 49 pruebas con el certificado espectral. Se añadió la comparación espectral de 513 filas y pasó la prueba dirigida. Tras integrar las tres mejoras de esta sección volvieron a pasar el ejecutable independiente, los contadores, el texto y las máscaras de reinas. La comprobación de gramática pasó con Lark 1.3.1 y llguidance 1.9.1, con 39 entradas válidas, 33 inválidas y 4 secuencias UTF-8 inválidas.

Se tomaron los mínimos de 19 rondas alternadas con el tamaño oficial y la salida byte a byte de CPython.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `mandelbrot` | 18,0644 | 11,6865 | 17,2416 | 11,6042 |
| `matmul` | 4,9988 | 0,2208 | 4,4641 | 0,2146 |
| `texto` | 35,3315 | 29,8063 | 31,7335 | 27,9229 |

La referencia de `mandelbrot` incluyó franjas de 512. La de `matmul` incluyó teselas por ciclos. Para `texto`, el menor tiempo de proceso fue el de la variante C que une el recuento de filas y la detección de bytes nulos.

### Árboles de un bit y preparación de la rueda de primos

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `arboles` | 5,3844 | 0,2189 | 5,2462 | 0,1640 |
| `criba` | 4,8047 | 0,0935 | 4,3689 | 0,0809 |

También se usaron 19 rondas con la salida exacta de CPython y los tamaños oficiales. El código nativo de `arboles` escribe y cuenta cada árbol en el bucle, incluidos los árboles pequeños guardados en las ranuras de una palabra. El ensayo de la rueda de `criba` incluyó el límite de 2³⁰ elementos y coincidió con la referencia anterior.

Con estas mejoras pasaron en Ubuntu 24.04 el ejecutable independiente, los contadores, el texto y las máscaras de reinas. También pasó la verificación de gramática con las dependencias fijadas por CI. La réplica usó Clang 23 y Python 3.12.

### Memoria con los ejecutables actualizados

Mediana de cinco picos de memoria residente en Windows con los tamaños oficiales.

| Banco | CPython (MiB) | Turi (MiB) | C extremo (MiB) |
|---|---:|---:|---:|
| `matmul` | 28,38 | 3,64 | 3,66 |
| `palabras` | 117,90 | 3,63 | 3,62 |
| `texto` | 9,66 | 3,74 | 25,05 |
| `vida` | 10,46 | 3,61 | 3,57 |
| `arboles` | 33,64 | 3,64 | 3,62 |
| `criba` | 162,09 | 3,61 | 3,61 |

El C de `arboles` usa un bit por nodo con LTO. El lector de `texto` en Turi conserva su bloque de 64 KiB, mientras que C carga el archivo de entrada.

En un ensayo separado de arranque de `vida` se usó un anfitrión mínimo en ambos ejecutables. Se comparó la salida con CPython en nueve entradas. C extremo obtuvo 3,7771 ms de proceso y 0,0702 ms de cálculo con LTO. Turi obtuvo 4,1523 y 0,0705 ms. El ensayo del anfitrión mínimo queda pendiente de la recuperación de errores del ejecutable general.

### Campaña completa con árboles de un bit y franjas de 512

Se midieron los 14 tamaños oficiales en 13 rondas alternadas en Windows. Todas las salidas coincidieron byte a byte con CPython y se comprobó el marcador de cálculo nativo. Cada tiempo de C es el menor entre sus variantes válidas con LTO y PGO entrenado en entradas distintas. Las razones superiores a 1 indican menor tiempo de Turi.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) | Proceso C/Turi | Cálculo C/Turi |
|---|---:|---:|---:|---:|---:|---:|
| `fannkuch` | 24,1068 | 19,0766 | 23,3592 | 18,6265 | 0,9690× | 0,9764× |
| `mandelbrot` | 16,9211 | 11,4047 | 16,6935 | 11,2539 | 0,9865× | 0,9868× |
| `nbody` | 457,4552 | 452,6964 | 470,6658 | 463,4554 | 1,0289× | 1,0238× |
| `spectralnorm` | 6,1210 | 0,0786 | 5,7960 | 0,0590 | 0,9469× | 0,7506× |
| `arboles` | 7,9288 | 0,2945 | 6,0661 | 0,2153 | 0,7651× | 0,7311× |
| `criba` | 5,7288 | 0,1241 | 5,4551 | 0,0904 | 0,9522× | 0,7284× |
| `cuerpos` | 52,7295 | 46,3849 | 52,4239 | 47,0845 | 0,9942× | 1,0151× |
| `dijkstra` | 5,5219 | 0,0011 | 5,4385 | 0,0045 | 0,9849× | 4,0909× |
| `matmul` | 7,5749 | 0,2756 | 6,5081 | 0,2401 | 0,8592× | 0,8712× |
| `mochila` | 6,6435 | 0,0149 | 5,4686 | 0,0121 | 0,8232× | 0,8121× |
| `palabras` | 10,1684 | 3,8168 | 9,6632 | 3,3841 | 0,9503× | 0,8866× |
| `reinas` | 5,3146 | 0,1546 | 5,1881 | 0,1023 | 0,9762× | 0,6617× |
| `texto` | 41,8924 | 34,3103 | 35,0205 | 28,0157 | 0,8360× | 0,8165× |
| `vida` | 5,9303 | 0,0708 | 4,8970 | 0,0798 | 0,8258× | 1,1271× |

`nbody` registró el menor tiempo en ambas métricas. Su margen fue de 1,0289× en proceso y 1,0238× en cálculo. Ningún banco alcanzó 1,05× en ambas métricas. Los tiempos de esta tabla corresponden a la misma campaña.

### Lotes de árboles, franjas adaptativas y pares de palabras

Pasaron las 49 pruebas de Windows con las nuevas rutas de `arboles`, `fannkuch`, `mandelbrot`, `nbody`, `cuerpos` y `texto`. La prueba de gramática volvió a verificar 39 entradas válidas, 33 inválidas y 4 secuencias UTF-8 inválidas con Lark 1.3.1 y llguidance 1.9.1. En `nbody` y `cuerpos` coincidieron las 30 posiciones y velocidades con CPython en diez duraciones cada uno.

Los prototipos de árboles agrupados por posición redujeron el cálculo de 0,2123 a 0,0417 ms. El ensamblado de Turi confirmó que cada vuelta escribe el lote y lee sus bits. En Mandelbrot, los tramos adaptativos redujeron el cálculo de 11,3630 a 8,4417 ms y usaron 28.946 bytes de certificados. En Fannkuch, codificar el desplazamiento de inversión en cada ficha redujo el cálculo de 19,2636 a 17,0274 ms. Las referencias C también se midieron con estas tres técnicas.

En la comparación de `texto` por parejas, Turi obtuvo 27,9585 ms de proceso y 22,0671 ms de cálculo. C extremo con parejas obtuvo 28,6442 ms de proceso y 23,2704 ms de cálculo. Se cotejaron los archivos de prueba con CPython, incluida la entrada oficial.

#### Ejecutables integrados en 19 rondas

Las seis salidas oficiales coincidieron byte a byte con CPython y se verificó el tiempo nativo. Se mantuvieron las variantes C anteriores y se añadieron las correspondientes a los nuevos algoritmos.

| Banco | Proceso Turi (ms) | Cálculo Turi (ms) | Proceso C extremo (ms) | Cálculo C extremo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 28,3998 | 17,1432 | 24,7416 | 16,0002 |
| `mandelbrot` | 15,7565 | 8,8635 | 16,6913 | 8,6682 |
| `nbody` | 483,1576 | 484,3775 | 486,1727 | 483,7131 |
| `arboles` | 8,9597 | 0,0555 | 6,4798 | 0,0496 |
| `cuerpos` | 55,2160 | 45,4405 | 53,8714 | 47,6529 |
| `texto` | 36,4654 | 29,1098 | 34,5893 | 27,8220 |

`mandelbrot` alcanzó 1,0593× en proceso y 0,9780× en cálculo. `cuerpos` alcanzó 1,0487× en cálculo y 0,9756× en proceso. La ventaja de `texto` del ensayo previo no se repitió en esta tanda.

La réplica Ubuntu 24.04 con Clang 23 y Python 3.12 pasó el ejecutable independiente, los contadores, el lector de texto, las máscaras de reinas y la gramática con las dependencias de CI.

Mediana de cinco picos de memoria residente. `texto` usó 3,74 MiB en Turi, 9,66 MiB en CPython y 25,05 MiB en C extremo con pares. `arboles` usó 3,66 MiB en Turi, 33,63 MiB en CPython y 3,63 MiB en C extremo con lotes de 2048.

### Lector fusionado y tres vectores completos

En 23 rondas alternadas, `texto` obtuvo 23,7 ms de proceso y 19,3 ms de cálculo. Los mínimos de C extremo fueron 26,7 y 23,2 ms. Se repitió con la variante C de claves provisionales entrenada con archivos distintos al oficial. Turi obtuvo 25,0395 y 21,0311 ms frente a 26,4901 y 22,6445 ms. La mejora fue de 1,05793× en proceso y 1,07671× en cálculo, con la misma salida que CPython. La mediana de cinco picos de memoria residente fue 3,74 MiB en Turi, 9,67 MiB en CPython y 25,05 MiB en C extremo.

`fannkuch` bajó a 20,4 ms de proceso y 14,6 ms de cálculo con tres vectores de ocho carriles. La referencia C de tres vectores por raíz obtuvo 19,5 y 14,3 ms. La salida oficial coincidió con CPython en las 23 rondas.

En Windows se reprodujo el fallo de la traza con un directorio temporal 8.3. La prueba pasó tras conservar la grafía de la ruta del programa. Con CPython 3.14.5 y un directorio 8.3 pasaron el ejecutable independiente, los contadores y el lector de texto.

### Grupos estrechos de cuerpos, regiones a 2048 y ramas compactas

Se midieron los 14 tamaños oficiales en 13 rondas alternadas en Windows. Cada ejecutable emitió su tiempo nativo y reprodujo la salida de CPython byte a byte. Cada columna de C toma el mínimo de las variantes manuales válidas para esa métrica, con LTO y con PGO entrenado en otras entradas.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 20,3552 | 15,2152 | 20,0304 | 14,4835 |
| `mandelbrot` | 12,3366 | 7,0981 | 12,2515 | 6,9166 |
| `nbody` | 419,8558 | 426,0726 | 404,5204 | 400,8510 |
| `spectralnorm` | 5,3589 | 0,0046 | 4,8312 | 0,0015 |
| `arboles` | 4,9906 | 0,0241 | 4,7307 | 0,0236 |
| `criba` | 5,5171 | 0,1280 | 5,0042 | 0,0870 |
| `cuerpos` | 47,0841 | 41,0533 | 44,2881 | 38,4014 |
| `dijkstra` | 4,5592 | 0,0010 | 4,4365 | 0,0043 |
| `matmul` | 4,7734 | 0,2199 | 4,7049 | 0,2101 |
| `mochila` | 5,3864 | 0,0113 | 4,8066 | 0,0100 |
| `palabras` | 8,5503 | 3,4753 | 8,1535 | 3,1248 |
| `reinas` | 5,2045 | 0,1351 | 4,9466 | 0,1060 |
| `texto` | 31,8269 | 23,5487 | 30,0448 | 24,1019 |
| `vida` | 5,6287 | 0,0860 | 4,8116 | 0,0809 |

Los mínimos de proceso y cálculo vienen de rondas independientes. `dijkstra` ganó en cálculo por 4,30×. En esta tanda, `texto` ganó en cálculo por 1,0235× y quedó en 0,9440× en proceso. Ningún banco alcanzó 1,05× en ambas medidas.

La batería completa del motor pasó en Windows con CPython 3.15.2. Los casos de cierres, recursión, mochila y reinas pasaron en los cuatro modos, incluida la recuperación con un límite de recursión bajo. Pasaron también el ejecutable independiente, los contadores, el lector de texto y el formato de salida.

### Signos finales SIMD y comprobación de órbitas al terminar

Cada tanda usó los tamaños oficiales, salidas byte a byte de CPython y los mínimos por métrica de las referencias C con LTO y PGO.

| Banco | Rondas | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|---:|
| `texto` con saltos agrupados | 23 | 23,9927 | 18,9172 | 24,3292 | 19,4113 |
| `texto` con signos finales SIMD | 29 | 21,3001 | 15,5902 | 20,9629 | 17,3616 |
| `texto` con signos finales SIMD | 23 | 22,1424 | 15,3994 | 21,7652 | 16,0185 |
| `texto` con signos finales SIMD y un núcleo | 37 | 28,9090 | 18,7499 | 28,8633 | 18,0708 |
| `nbody` con comprobación final | 23 | 405,0654 | 391,5347 | 395,9064 | 387,4279 |
| `cuerpos` con comprobación final | 23 | 44,6029 | 39,0111 | 43,3721 | 38,2616 |

Con la máscara de signos, el cálculo de `texto` ganó por 1,1136× y 1,0402× en las dos primeras tandas. Los mínimos de proceso quedaron en 0,9842× y 0,9830×. La tercera fijó el mismo procesador lógico para ambos y dio 0,9984× en proceso y 0,9638× en cálculo. En cinco mediciones por ejecutable, el pico residente mediano fue de 3,74 MiB en Turi, 9,67 MiB en CPython y 25,05 MiB en C extremo con signos SIMD y PGO.

Las pruebas de Windows con CPython 3.15.2 compararon el lector con AVX-512, AVX2 y escalar. Los dos simuladores conservaron las seis magnitudes de cada cuerpo en diez duraciones. La colisión exacta y el subdesbordamiento reprodujeron la excepción, la traza y la salida completa de CPython.

En Ubuntu 24.04 con Clang 23 pasaron el ejecutable independiente, los contadores, el lector de texto y las máscaras de reinas. El parser, el formateador y las gramáticas aprobaron los 39 casos válidos, 33 inválidos y cuatro entradas con UTF-8 incorrecto.

### Segunda casilla de palabras y divisores congelados

La lectura por pares prueba también la segunda casilla de la tabla compacta cuando hay una colisión. La criba conserva el cociente anterior al menor factor de cada divisor grande. La batería de Windows pasó los 49 casos con CPython 3.13. Con CPython 3.15.2 pasaron también los ejecutables de texto y contadores, los cierres, la recursión, la mochila y las reinas. En la réplica Ubuntu 24.04 con Clang 23.1.6 pasaron el ejecutable independiente, los contadores, el lector y las máscaras de reinas. La prueba de gramática verificó 39 casos válidos, 33 inválidos y cuatro entradas con UTF-8 incorrecto.

Se midieron los 14 bancos en 13 rondas alternadas en Windows. Las salidas coincidieron byte a byte con CPython. Cada columna de C toma el mínimo de las variantes manuales válidas con LTO y con PGO entrenado en otras entradas.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 19,6101 | 14,5385 | 18,7178 | 14,1355 |
| `mandelbrot` | 12,0645 | 7,0696 | 11,7365 | 6,9294 |
| `nbody` | 401,6009 | 395,4595 | 393,8737 | 386,7854 |
| `spectralnorm` | 4,4173 | 0,0041 | 4,0629 | 0,0014 |
| `arboles` | 3,9320 | 0,0210 | 3,9185 | 0,0246 |
| `criba` | 4,8804 | 0,0851 | 3,7479 | 0,0812 |
| `cuerpos` | 44,4036 | 39,2382 | 43,1929 | 38,3659 |
| `dijkstra` | 4,2299 | 0,0010 | 4,0652 | 0,0043 |
| `matmul` | 4,6465 | 0,2290 | 3,9877 | 0,2162 |
| `mochila` | 4,4832 | 0,0113 | 4,0555 | 0,0099 |
| `palabras` | 7,7796 | 3,2995 | 7,4599 | 2,9769 |
| `reinas` | 4,5688 | 0,1146 | 4,2728 | 0,1012 |
| `texto` | 19,6239 | 13,9823 | 20,0525 | 15,0370 |
| `vida` | 5,9171 | 0,0864 | 5,0773 | 0,0937 |

`dijkstra`, `arboles`, `vida` y `texto` ganaron en cálculo. `texto` ganó también en proceso por 1,0218×. Ningún banco alcanzó 1,05× en ambas medidas. El ensayo de recíprocos enteros en `criba` conservó los resultados hasta 2³⁰, con 0,1139 ms frente a 0,1146 ms de la base Turi en la misma tanda.

### Lectura directa y certificado lineal

En 31 rondas alternadas con el archivo oficial, `texto` obtuvo 17,9602 ms de proceso y 13,0743 ms de cálculo frente a 20,0832 y 14,8808 ms de C extremo. Las referencias de C más rápidas usaron lectura directa, con LTO para proceso y con LTO y PGO para cálculo. Turi alcanzó 1,1182× y 1,1382× respectivamente. La salida coincidió byte a byte con CPython. Pasaron las pruebas de archivos en Windows con CPython 3.13 y 3.15.2, incluidos dos casos en el límite de 64 KiB.

Una segunda tanda de 31 rondas dio 17,9567 y 13,3115 ms para Turi frente a 19,9042 y 14,8843 ms para C extremo. Las ventajas fueron de 1,1085× en proceso y 1,1182× en cálculo. La mediana de cinco picos residentes fue de 3,74 MiB en Turi, 9,68 MiB en CPython y 25,04 MiB en C extremo con lectura directa, LTO y PGO.

El certificado de Mandelbrot con derivada abarcó el 93,9310 % del área en 25.321 tramos a resolución 2048. En la misma tanda, Turi pasó de 11,4799 a 10,3994 ms de proceso y de 6,9583 a 5,7824 ms de cálculo. C extremo con los mismos tramos obtuvo 9,9650 y 5,5755 ms. Las quince mallas de control y la oficial coincidieron byte a byte con CPython.

### Prefijo de rueda constante y extremos de tramos reutilizados

La criba comparte la tabla de prefijos entre el acceso por rango y la inicialización vectorial. Se comprobó la salida frente a CPython en doce tamaños y frente al C de divisores filtrados en cuatro tamaños adicionales hasta 2³⁰. Mandelbrot calcula una vez por malla los extremos horizontales y los reutiliza entre filas. Coincidieron las quince mallas de control y la oficial.

Con las dos mejoras integradas, los 14 bancos coincidieron byte a byte con CPython en 23 rondas alternadas en Windows. Se tomó el mínimo de cada variante y se eligió la más rápida por métrica entre las referencias C manuales con LTO y PGO entrenado en entradas independientes.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 19,5263 | 14,5461 | 19,0166 | 14,0321 |
| `mandelbrot` | 9,9421 | 5,5882 | 10,0879 | 5,6146 |
| `nbody` | 390,1776 | 388,2313 | 386,6094 | 384,5592 |
| `spectralnorm` | 3,7475 | 0,0041 | 3,6900 | 0,0013 |
| `arboles` | 3,8078 | 0,0201 | 3,6286 | 0,0233 |
| `criba` | 4,2147 | 0,0690 | 3,8810 | 0,0737 |
| `cuerpos` | 43,4442 | 38,4351 | 42,6107 | 37,5841 |
| `dijkstra` | 4,0510 | 0,0010 | 4,0365 | 0,0042 |
| `matmul` | 4,4908 | 0,2348 | 4,0981 | 0,2172 |
| `mochila` | 4,0657 | 0,0138 | 3,6781 | 0,0095 |
| `palabras` | 7,6392 | 3,2681 | 7,3289 | 2,9157 |
| `reinas` | 4,2895 | 0,1148 | 3,9722 | 0,1006 |
| `texto` | 17,7029 | 13,3882 | 20,5541 | 14,9972 |
| `vida` | 4,4256 | 0,0756 | 4,0514 | 0,0737 |

`texto` confirmó el margen del 5 % en las dos métricas, con 1,1611× en proceso y 1,1202× en cálculo. Mandelbrot registró 1,0147× y 1,0047×. `arboles`, `criba` y `dijkstra` superaron el margen en cálculo. Quedan 13 bancos pendientes del margen en ambas mediciones.

El prototipo Windows con CRT diferida pasó de 3,8355 a 3,1369 ms de proceso en `dijkstra`. El C manual con entrada mínima, LTO y PGO registró 3,0435 ms. El cálculo de `arboles` y `vida` subió de 0,0191 y 0,0677 ms en la base a 0,0297 y 0,0806 ms en el prototipo. En los tres programas coincidieron las salidas y los errores de CPython en argumentos con espacios, guiones bajos, texto inválido y dígitos Unicode.

### Filas agrupadas en Mandelbrot

La prueba con dos vectores por fila redujo el cálculo de Turi de 5,6330 a 5,0022 ms en la misma tanda. Las quince mallas de control y la oficial coincidieron byte a byte con CPython, también tras integrar el cambio. En 31 rondas oficiales con la nueva referencia C, Turi obtuvo 9,5164 ms de proceso y 4,9615 ms de cálculo. C extremo obtuvo 9,0595 y 4,8319 ms.

En la misma campaña, `texto` obtuvo 17,6054 y 12,7660 ms en Turi frente a 19,8879 y 15,2924 ms en C extremo. Los cocientes fueron 1,1296× y 1,1979×.

### Certificado cuadrático y catálogo de mochila

El término cuadrático elevó el área certificada de Mandelbrot de 93,9310 % a 94,5390 % y redujo la tabla compartida de 105.382 a 86.794 bytes. A resolución 4096 se certificó el 96,0391 % con una tabla de 249.870 bytes. En el mismo ensayo, Turi calculó la malla oficial en 5,0709 ms con la base, 4,7406 ms a 2048 y 4,4948 ms a 4096. El C más rápido de la tanda calculó en 4,3507 ms a 4096. Se integró la tabla de 2048 en Turi y la de 4096 en las variantes C. Coincidieron las dieciséis mallas tras integrar ambas tablas.

El catálogo de `mochila` bajó el cálculo del prototipo Turi de 0,0137 a 0,0065 ms en la misma tanda, frente a 0,0084 ms del nuevo C con catálogo y PGO. El menor proceso completo fue 3,8062 ms para ese Turi y 3,6225 ms para C. El ejecutable integrado se cotejó con CPython en diecisiete tamaños, incluidos 439, 440 y 441 objetos.

Las asignaciones parciales de `fannkuch` reutilizan la última rama. En el ensayo, el cálculo Turi pasó de 12,3457 a 11,0017 ms. El mejor C extremo calculó en 11,5633 ms y obtuvo el menor proceso completo con 15,9678 ms frente a 16,5329 ms de Turi. La versión integrada coincidió byte a byte con CPython para los tamaños del uno al diez.

La siguiente tanda oficial de 19 rondas verificó la salida byte a byte de los 14 bancos. `texto` mantuvo el margen del 5 % con 1,1242× en proceso y 1,1957× en cálculo. `arboles`, `criba`, `dijkstra` y `mochila` alcanzaron el margen en cálculo, con 1,1959×, 1,0551×, 4,2000× y 1,4262× respectivamente. El `fannkuch` integrado registró 15,6779 ms de proceso y 11,2333 ms de cálculo frente a 15,8643 y 11,1439 ms de C extremo. Con su tabla de 4096, C extremo bajó Mandelbrot a 8,6751 ms de proceso y 4,3347 ms de cálculo frente a 9,1336 y 4,7662 ms de Turi.

En cinco ejecuciones por variante con las entradas oficiales, la mediana del pico residente en Turi, C extremo con LTO y PGO, y CPython respectivamente fue de 3,72, 25,04 y 9,68 MiB en `texto`, 3,62, 3,61 y 162,11 MiB en `criba`, y 3,62, 3,60 y 205,37 MiB en `mochila`.

Pasaron las 49 pruebas de Windows y la validación dirigida del motor híbrido con CPython 3.15.2. En Ubuntu 24.04 con Clang 23 se verificaron las 47 pruebas en dos tandas. Las cuatro que dependen de la ubicación del binario o de escrituras en el árbol de fuentes pasaron al repetirlas con el binario bajo `build/` y la copia del proyecto con permisos de escritura. La gramática comprobó 39 entradas válidas, 33 inválidas y 4 secuencias UTF-8 inválidas con Lark 1.3.1 y llguidance 1.9.1.

### Desplazamientos codificados, huecos cortos y sumas periódicas

En `fannkuch`, las fichas de las asignaciones parciales guardan el desplazamiento de su inversión. Mandelbrot procesa los huecos de dieciséis píxeles con dos vectores. `matmul` puede iniciar las teselas en la suma exacta de cuatro ciclos cuando los generadores afines son aptos. La entrada de 400 usa 12.480 bytes de datos estáticos para ese prefijo. La suma final procesa varias filas a la vez y reutiliza la memoria del primer factor.

La campaña de 23 rondas ejecutó estas variantes integradas y las referencias C más rápidas de cada métrica con LTO y PGO entrenado en otras entradas.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 14,8251 | 10,2765 | 14,2598 | 9,9444 |
| `mandelbrot` | 9,3015 | 4,5821 | 8,7703 | 4,1115 |
| `matmul` | 4,1218 | 0,1080 | 3,8753 | 0,0785 |

Coincidieron byte a byte con CPython diez tamaños de `fannkuch`, dieciséis de Mandelbrot y diecisiete de `matmul`. Con CPython 3.11 y 3.13 también se verificaron en `matmul` tres tamaños con otro módulo y otras semillas, y dos sin prefijo. Antes de estos tres cambios, la mediana de cinco picos residentes fue 3,74 MiB en Mandelbrot en Turi frente a 3,92 MiB en C extremo, y 3,66 MiB en `fannkuch` en Turi frente a 3,60 MiB en C extremo.

En 31 rondas comparables de arranque, el C manual con entrada mínima alcanzó 2,8814 ms de proceso y 0,0189 ms de cálculo en `arboles`, y 2,8952 y 0,0034 ms en `mochila`. Las variantes Turi con CRT diferida redujeron el proceso frente a su propia base y quedaron por detrás de esos mínimos C. Estos ensayos mantuvieron las salidas de CPython.

En el ensayo de `dijkstra`, el C de entrada mínima con PGO registró 2,8525 ms de proceso y el Turi con CRT diferida registró 2,8534 ms. El cálculo de la referencia mínima quedó cerca de la resolución del contador.

En `texto`, la nueva referencia C con lectura de 256 KiB redujo el proceso de 19,6747 a 17,2599 ms y el cálculo de 15,7660 a 12,4025 ms. Turi obtuvo 18,1319 y 13,1536 ms en la misma tanda. Coincidieron el archivo oficial y los controles de fin de línea, bytes NUL y líneas que cruzan bloques. La victoria de `texto` frente a la referencia anterior queda pendiente frente a esta variante.

En `matmul`, preparar los factores de los términos restantes redujo el cálculo a 0,0753 ms, frente a 0,0610 ms de C extremo en 23 rondas. La variante integrada que acumula una franja de cuatro filas antes de pasar a la siguiente llegó a 0,0433 ms. La variante C con franjas y transposición AVX obtuvo 0,0299 ms. Los procesos completos registraron 4,2507 y 3,7269 ms para Turi y C extremo respectivamente. Se volvieron a verificar los diecisiete tamaños de `matmul` y los cinco adicionales en CPython 3.11 y 3.13.

Los lectores C de 64, 256 y 512 KiB se comprobaron con el archivo oficial y nueve controles, entre ellos líneas largas, CR aislados, CRLF junto a un límite y claves con NUL. En cinco mediciones por ejecutable del ensayo con el archivo oficial, la mediana de los picos fue 3,62 MiB en el C de 256 KiB y 3,74 MiB en Turi. Los ensayos de bloque mayor y conversión in situ de Turi mantuvieron las salidas y quedaron por detrás de C en cálculo.

La campaña posterior de 19 rondas cubrió los 14 bancos con las referencias que incluyen las franjas de `matmul` y los lectores por bloques de `texto`. Las salidas fueron idénticas a CPython y el margen de 1,05× en proceso y cálculo quedó pendiente en los 14. `vida` registró 3,7297 ms de proceso y 0,0687 ms de cálculo en Turi frente a 3,7780 y 0,0711 ms de C extremo. En la misma revisión, la mediana de cinco picos residentes en Turi, C extremo y CPython fue de 3,69, 3,64 y 28,45 MiB en `matmul`, y 3,74, 3,62 y 9,68 MiB en `texto`. Las 49 pruebas de Windows pasaron.

En `texto`, las claves del primer bloque permiten elegir un multiplicador que evita sus colisiones. Las claves posteriores conservan el sondeo. En 25 rondas alternadas después de cuatro de calentamiento, los mínimos de Turi pasaron de 17,4880 a 16,6515 ms de proceso y de 13,3378 a 12,2220 ms de cálculo. Las referencias C con la misma reorganización midieron 15,6025 ms de proceso y 11,0052 ms de cálculo en sus mejores variantes. Todas reprodujeron los bytes de CPython en el archivo oficial y ocho controles.

La integración de `texto` conservó la salida en 11 archivos en 15 ejecutables. La mediana de cinco picos residentes fue 3,75 MiB con Turi, 3,62 MiB con la referencia C de 256 KiB y 9,67 MiB con CPython. Las franjas de `matmul` se comprobaron también en 22 tamaños con CPython 3.15.2.

En Mandelbrot se emplearon franjas a resolución 4096 y se omitieron los tramos de menos de 14 celdas. La tabla ocupa 160.966 bytes. El recorrido SIMD calcula los píxeles de los tramos omitidos. En 23 rondas medidas, la variante Turi pasó de 9,2625 a 8,5788 ms de proceso y de 4,5650 a 4,1621 ms de cálculo. C extremo con el mismo recorte registró 8,5785 y 4,0499 ms. Todas las salidas coincidieron con CPython en quince mallas y la oficial. El pico residente mediano de Turi fue de 3,89 MiB, frente a 4,09 MiB con la tabla completa y 3,74 MiB con la anterior de 2048.

Guardar la ficha conocida y la posición pendiente en campos del mismo tamaño redujo `fannkuch` a 13,9428 ms de proceso y 8,9977 ms de cálculo en las variantes C con signo fijado. Pasaron diez tamaños frente a CPython. En la misma tanda, el cálculo mínimo de Turi fue de 10,0557 ms con la variante integrada y de 10,2126 ms con los campos compartidos.

La réplica local de Ubuntu pasó la batería híbrida con estas integraciones. Pasó también la comprobación de gramáticas con las dependencias de CI, con 39 entradas válidas, 33 inválidas y 4 casos de UTF-8 inválido.

El recorrido inverso de `fannkuch` agrupa las permutaciones que aún comparten fichas sin fijar. En 25 rondas medidas pasó de 15,1283 a 11,9158 ms de proceso y de 10,3543 a 7,2959 ms de cálculo en Turi. C extremo con el mismo recorrido obtuvo 11,7572 y 7,0805 ms. Los diez tamaños coincidieron byte a byte con CPython.

Resolver la última ficha en su única posición libre y continuar con la permutación completa redujo el cálculo de Turi a 6,4510 ms y el proceso a 11,4104 ms en otra tanda de 25 rondas. C extremo con esa optimización registró 6,4000 y 11,2045 ms. Las diez salidas volvieron a coincidir con CPython.

La suma de sufijos en ULP de `matmul` coincidió con CPython en 24 casos, incluidos los de respaldo. A 400, el cálculo mínimo de Turi fue de 0,0740 ms frente a 0,0382 ms de las franjas. Se mantuvieron las franjas. Las ruedas de `criba` con cinco y seis primos base conservaron el resultado hasta 2³⁰ y no redujeron el mínimo de cálculo frente a la rueda de 210.

### Comparación de los 14 bancos con el recorrido inverso integrado

En 19 rondas alternadas, cada ejecutable reprodujo la salida de CPython byte a byte. Los tiempos C son los mínimos entre las variantes manuales válidas con LTO y PGO entrenado en entradas independientes. El umbral de 1,05× en las dos medidas queda pendiente en los 14 bancos. Los ensayos separados con entrada mínima de Windows también obtuvieron tiempos de proceso inferiores a los de esta campaña en `arboles`, `dijkstra` y `mochila`.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 11,0043 | 6,6109 | 11,5342 | 6,3941 |
| `mandelbrot` | 8,9724 | 4,1319 | 8,4665 | 4,0890 |
| `nbody` | 393,4370 | 389,2163 | 389,9764 | 384,3096 |
| `spectralnorm` | 4,5162 | 0,0042 | 4,1006 | 0,0013 |
| `arboles` | 3,8919 | 0,0192 | 3,9664 | 0,0229 |
| `criba` | 4,0862 | 0,0768 | 3,8409 | 0,0752 |
| `cuerpos` | 43,5249 | 39,1303 | 42,7257 | 37,9106 |
| `dijkstra` | 3,9603 | 0,0010 | 3,6819 | 0,0040 |
| `matmul` | 4,5252 | 0,0450 | 4,1360 | 0,0300 |
| `mochila` | 3,7124 | 0,0063 | 3,7557 | 0,0088 |
| `palabras` | 7,8033 | 3,2601 | 7,3930 | 2,8814 |
| `reinas` | 4,3925 | 0,1144 | 4,0612 | 0,1015 |
| `texto` | 19,0019 | 12,1144 | 17,7608 | 11,0275 |
| `vida` | 3,9977 | 0,0736 | 3,9099 | 0,0734 |

La mediana de cinco picos residentes de Turi y C extremo fue de 3,68 y 3,60 MiB en `fannkuch`, y de 3,90 y 3,83 MiB en Mandelbrot.

La batería completa de Windows pasó las 49 pruebas tras integrar el recorrido inverso con la resolución de la última ficha.

### Campos de cinco bits y primer cuerpo aislado

Completar las dos últimas fichas redujo el cálculo mínimo de `fannkuch` de 6,3666 a 6,1692 ms en una repetición con 25 rondas medidas. Con campos de cinco bits y el máximo en las hojas, el siguiente ensayo redujo la mediana del cálculo de 7,5062 a 6,8269 ms. La variante C que devuelve el recuento y el máximo en una palabra alcanzó 5,8268 ms en el ensayo de campos estrechos. Se cotejaron diez tamaños byte a byte tras integrar los campos de cinco bits.

La nueva distribución de los cinco cuerpos conservó las tres coordenadas y las tres velocidades de cada cuerpo en diez duraciones de cada programa. En la repetición, el cálculo mínimo de Turi pasó de 389,0311 a 379,6651 ms en `nbody` y de 37,5324 a 36,9192 ms en `cuerpos`. Las mejores referencias C de la tanda quedaron en 382,9968 y 37,1674 ms respectivamente. Pasaron también siete entradas por programa en todas las variantes de esa tanda.

En Mandelbrot, usar el certificado en los bordes de cada byte conservó las salidas en dieciocho mallas. El cálculo de Turi pasó de 4,1744 a 4,3948 ms. En `texto`, guardar parte del contador dentro de la clave elevó el cálculo mínimo de 11,9540 a 14,0262 ms tras cotejar ocho archivos de control y el oficial.

### Campaña con las dos optimizaciones integradas

Los 14 bancos reprodujeron la salida de CPython byte a byte en 19 rondas alternadas. Las referencias son los mínimos de cada métrica con variantes manuales, LTO y PGO entrenado en entradas independientes. `cuerpos` obtuvo 1,0200× en proceso y 1,0135× en cálculo. El margen de 1,05× en ambas medidas quedó pendiente en los 14 bancos.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 10,6111 | 5,9548 | 10,3714 | 5,8596 |
| `mandelbrot` | 9,0625 | 4,1187 | 8,7475 | 4,0841 |
| `nbody` | 389,6273 | 379,4106 | 387,4263 | 380,5610 |
| `spectralnorm` | 4,0263 | 0,0042 | 4,1065 | 0,0013 |
| `arboles` | 4,1476 | 0,0195 | 3,9096 | 0,0231 |
| `criba` | 4,0891 | 0,0763 | 3,7519 | 0,0731 |
| `cuerpos` | 41,7420 | 37,1257 | 42,5749 | 37,6279 |
| `dijkstra` | 3,9283 | 0,0009 | 3,8169 | 0,0041 |
| `matmul` | 4,0057 | 0,0419 | 3,7924 | 0,0292 |
| `mochila` | 4,2618 | 0,0063 | 4,0988 | 0,0088 |
| `palabras` | 8,0033 | 3,3470 | 7,4409 | 2,9611 |
| `reinas` | 3,7749 | 0,1138 | 3,6776 | 0,1004 |
| `texto` | 19,0260 | 12,0020 | 17,8933 | 11,0544 |
| `vida` | 3,8456 | 0,0680 | 3,8101 | 0,0699 |

La batería completa de Windows pasó las 49 pruebas con los campos de cinco bits y el primer cuerpo aislado.

La mediana de cinco picos residentes en Turi y C extremo fue de 3,67 y 3,60 MiB en `fannkuch`, de 3,62 y 3,61 MiB en `nbody`, y de 3,63 y 3,62 MiB en `cuerpos`.

### Tabla de signos y recíprocos de la rueda

El recorrido inverso de `fannkuch` comparte una vista de 165 palabras que representan el signo de cada orden de tres fichas completas. El ensayo redujo el cálculo mínimo de 5,8058 a 5,5108 ms con la vista. La referencia C con su tabla estática obtuvo 5,3663 ms en la misma tanda. La tabla cubre los 720 órdenes válidos y las diez entradas coincidieron con CPython después de integrarla.

`criba` calcula los recíprocos de los residuos que guarda en su tabla de divisores. En dos ensayos, los mínimos de cálculo pasaron de 0,0697 a 0,0628 ms y de 0,0700 a 0,0631 ms. Coincidieron trece entradas con CPython y cuatro tamaños adicionales con las referencias C hasta 2³⁰ tras integrar el cambio.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 11,3981 | 5,4750 | 11,1125 | 5,3321 |
| `criba` | 5,7183 | 0,0629 | 5,0100 | 0,0628 |

Se midieron 23 rondas alternadas con las nuevas referencias C y todas las salidas coincidieron con CPython.

La batería completa de Windows pasó las 49 pruebas tras integrar la vista de signos y los recíprocos por residuo. La comprobación con Lark 1.3.1 y llguidance 1.9.1 pasó 39 entradas válidas, 33 inválidas y 4 secuencias UTF-8 inválidas.

Tras esos cambios, los picos residentes medianos de Turi y C extremo fueron 3,66 y 3,60 MiB en `fannkuch` y 3,62 y 3,58 MiB en `criba`, en cinco mediciones por ejecutable.

### Orden de carriles y preparación de posiciones

La disposición de carriles de los cuerpos se eligió midiendo siete distribuciones además de la integrada. En una repetición, `nbody` pasó de 382,1680 a 370,9680 ms de cálculo y `cuerpos` de 36,6765 a 36,0747 ms. C extremo alcanzó 377,4840 y 36,8043 ms, respectivamente. La integración conservó las 30 coordenadas y velocidades en diez duraciones de cada programa.

En `fannkuch`, los campos con cero quedan fuera de las coincidencias y la primera posición se excluye mediante el valor 15 de la identidad. Se puede omitir la máscara de posiciones durante el recorrido de las permutaciones completas. El piloto redujo el cálculo mínimo de 5,4352 a 5,2204 ms en Turi. C extremo registró 5,1492 ms. Coincidieron las diez entradas con CPython.

`criba` prepara también las posiciones de la rueda en grupos SIMD. En el piloto, el mínimo de cálculo pasó de 0,0625 a 0,0606 ms y la mediana de 0,0849 a 0,0759 ms. Coincidieron trece entradas con CPython y cuatro tamaños adicionales con las referencias C hasta 2³⁰.

El prototipo de entrada ligera de Windows usó un ejecutable auxiliar para recuperar los argumentos que requieren la ruta general. Con arena inicial, `arboles` obtuvo 2,9022 ms de proceso y 0,0245 ms de cálculo. C extremo con entrada mínima obtuvo 3,0324 y 0,0201 ms. En `mochila`, Turi obtuvo 2,8832 y 0,0055 ms frente a 2,8451 y 0,0033 ms de C. También coincidieron los argumentos de control con espacios, guiones bajos, dígitos Unicode y texto inválido.


### Campaña integrada de los 14 bancos

En 19 rondas alternadas, los 14 bancos coincidieron byte a byte con CPython. Los tiempos de C extremo corresponden a los mejores resultados de sus variantes manuales con LTO y con PGO entrenado en entradas independientes. Los ensayos de inicio mínimo de Windows se midieron por separado. `nbody` y `cuerpos` quedaron por delante en las dos medidas, con ventajas de entre 0,5 % y 2,7 %. El margen de 1,05× en ambas medidas sigue pendiente en los 14 bancos.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 11,0788 | 5,2555 | 10,5462 | 5,1441 |
| `mandelbrot` | 8,8164 | 4,1044 | 8,4944 | 4,0384 |
| `nbody` | 382,0339 | 376,5556 | 388,7819 | 378,4822 |
| `spectralnorm` | 4,3343 | 0,0045 | 4,1871 | 0,0014 |
| `arboles` | 4,3882 | 0,0193 | 4,0662 | 0,0233 |
| `criba` | 5,3181 | 0,0618 | 5,1588 | 0,0627 |
| `cuerpos` | 40,9688 | 36,5731 | 42,0556 | 37,1415 |
| `dijkstra` | 4,4967 | 0,0010 | 4,0937 | 0,0043 |
| `matmul` | 4,5145 | 0,0425 | 4,3339 | 0,0300 |
| `mochila` | 4,4911 | 0,0063 | 3,9455 | 0,0089 |
| `palabras` | 8,1722 | 3,4095 | 7,6798 | 3,0230 |
| `reinas` | 4,8019 | 0,1168 | 4,4078 | 0,1028 |
| `texto` | 19,3385 | 12,2774 | 17,5153 | 11,0346 |
| `vida` | 4,1089 | 0,0678 | 3,8150 | 0,0696 |

Las 49 pruebas de Windows pasaron con esta revisión.

### Cociente por rango, reserva única y catálogo de 16 bits

El cociente de los divisores de `criba` se comprobó en los 32.768 valores de su rango. En el ensayo, el cálculo mínimo de Turi pasó de 0,0610 a 0,0591 ms. En 23 rondas con la revisión integrada, Turi obtuvo 5,6684 ms de proceso y 0,0611 ms de cálculo. C extremo obtuvo 5,3612 y 0,0629 ms. La salida coincidió con CPython en trece tamaños y con C extremo en cuatro adicionales hasta 2³⁰.

Al usar una reserva para ambos planos de `arboles`, Turi pasó de 0,0188 a 0,0179 ms de cálculo en el ensayo. La revisión integrada obtuvo 4,4010 y 0,0186 ms frente a 4,0764 y 0,0235 ms de las referencias en la campaña oficial. Una referencia adicional con arranque mínimo alcanzó 3,0120 ms de proceso y 0,0177 ms de cálculo en el ensayo separado. Las doce profundidades verificadas frente a CPython y cuatro adicionales frente a C extremo coincidieron. El pico residente mediano fue de 3,61 MiB en ambos ejecutables.

Con dieciséis carriles estrechos en el catálogo de `palabras`, el mínimo de cálculo pasó de 3,2591 a 2,9527 ms y el de proceso de 7,4010 a 7,0152 ms en el ensayo. Las referencias C actualizadas obtuvieron 6,8600 ms de proceso y 2,7512 ms de cálculo. La revisión integrada coincidió con CPython en catorce tamaños y en 63 controles con generadores, alfabetos y consumidores alternativos. Su pico residente mediano fue de 3,63 MiB.

La campaña completa posterior verificó los 14 bancos y usó también las dos variantes C con carriles estrechos. En 19 rondas, `palabras` obtuvo 9,0117 ms de proceso y 3,0217 ms de cálculo frente a 8,5060 y 2,7729 ms. `nbody` y `cuerpos` volvieron a quedar por delante en ambas medidas. Los 14 bancos coincidieron byte a byte con CPython y el margen del 5 % en ambas medidas quedó pendiente en todos.

En Ubuntu 24.04 con Clang 23 se recompiló esa revisión. Pasaron los controles dirigidos del ejecutable y los 76 casos de gramática con Lark 1.3.1 y llguidance 1.9.1. El formateador estricto rechazó también `fn main() { let a = 2; }`.

### Reutilización de la reserva, filas más estrechas y adelanto de palabras

`arboles` reutiliza la mitad de la reserva del árbol inicial para construir el árbol persistente. La reserva oficial bajó de 24 a 16 KiB. La revisión integrada reprodujo doce profundidades frente a CPython y cuatro adicionales frente a C extremo, con 3,61 MiB de pico residente mediano. En 23 rondas dio 3,9575 ms de proceso y 0,0173 ms de cálculo frente a 3,9305 y 0,0228 ms de las variantes oficiales de C. Una referencia con inicio mínimo midió 0,0171 ms de cálculo en un ensayo aparte.

En `vida`, las palabras de 32 bits permiten procesar dieciséis filas por vector. La reserva de los dos tableros bajó de 143.104 a 129.536 bytes. Coincidieron 44 controles frente a CPython con seis dimensiones, semillas e intervalos. El pico residente mediano fue de 3,62 MiB. En 23 rondas obtuvo 3,95 ms de proceso y 0,0696 ms de cálculo frente a 4,06 y 0,0654 ms de C extremo. La referencia de cálculo más rápida usó también dieciséis filas y PGO.

En `palabras`, el adelanto de ocho transiciones y el avance de dos pares por iteración bajaron el cálculo del prototipo de 3,0288 a 2,9105 ms. La revisión integrada pasó sus 77 controles con CPython, con 3,63 MiB de pico residente mediano. En 23 rondas obtuvo 8,32 ms de proceso y 2,95 ms de cálculo frente a 8,34 y 2,77 ms de C extremo.

En la campaña completa de 19 rondas, cada ejecutable reprodujo la salida oficial de CPython. Se tomaron los mínimos por métrica entre las variantes C manuales con LTO y con PGO entrenado en entradas independientes. Los tres bancos con ventaja en las dos medidas en esta campaña fueron `nbody`, `cuerpos` y `mochila`. El margen de 1,05× en ambas medidas queda pendiente en los 14 bancos. Las referencias con inicio mínimo de Windows se midieron aparte.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `fannkuch` | 11,2 | 5,36 | 10,7 | 5,13 |
| `mandelbrot` | 8,83 | 4,11 | 8,41 | 4,04 |
| `nbody` | 376,3 | 371,6 | 381,0 | 378,1 |
| `spectralnorm` | 4,07 | 0,0043 | 3,83 | 0,0013 |
| `arboles` | 4,13 | 0,0171 | 3,82 | 0,0229 |
| `criba` | 5,47 | 0,0627 | 4,83 | 0,0637 |
| `cuerpos` | 41,3 | 36,1 | 41,7 | 36,8 |
| `dijkstra` | 4,13 | 0,0010 | 4,08 | 0,0042 |
| `matmul` | 4,50 | 0,0431 | 3,81 | 0,0305 |
| `mochila` | 3,71 | 0,0065 | 3,85 | 0,0089 |
| `palabras` | 8,71 | 2,92 | 7,96 | 2,73 |
| `reinas` | 4,04 | 0,1147 | 4,05 | 0,1013 |
| `texto` | 19,0 | 12,0 | 17,7 | 11,1 |
| `vida` | 3,97 | 0,0687 | 3,75 | 0,0701 |

La ventana de capacidades de `mochila` coincidió con CPython en quince tamaños, pero el mínimo de cálculo subió de 0,0058 a 0,0060 ms y el de proceso de 3,7820 a 4,1119 ms. Se mantuvo el recorrido anterior.

Pasaron las 49 pruebas de Windows con esta revisión. La prueba de gramática con las dependencias fijadas pasó 39 entradas válidas, 33 inválidas y cuatro secuencias UTF-8 inválidas.

### Vistas de una reserva y transiciones de treinta y dos carriles

En `vida`, la reserva única bajó el mínimo de cálculo del piloto de 0,0732 a 0,0664 ms. En la repetición de 35 rondas alternadas pasó de 0,0670 a 0,0660 ms y la mediana de 0,0908 a 0,0805 ms. Los mínimos de proceso fueron 3,9149 y 3,9423 ms en esa repetición. Con la revisión integrada, los 44 controles fueron exactos y el pico residente mediano fue de 3,61 MiB.

Se añadieron las referencias C que escriben ambos enteros en una operación. En 23 rondas alternadas, las tres salidas coincidieron con CPython y los mínimos por métrica quedaron así.

| Banco | Turi proceso (ms) | Turi cálculo (ms) | C extremo proceso (ms) | C extremo cálculo (ms) |
|---|---:|---:|---:|---:|
| `criba` | 5,2534 | 0,0602 | 4,9827 | 0,0587 |
| `dijkstra` | 4,3058 | 0,0009 | 3,9773 | 0,0018 |
| `vida` | 4,1255 | 0,0664 | 3,9202 | 0,0617 |

En `palabras`, preparar treinta y dos transiciones juntas y sumar el salto empaquetado bajó el mínimo de cálculo del prototipo de 2,9562 a 2,8421 ms. En la repetición pasó de 3,0024 a 2,8456 ms. Los 77 controles con la revisión integrada coincidieron con CPython y el pico residente mediano fue de 3,63 MiB. En 23 rondas el banco oficial dio 8,5388 y 2,8684 ms para Turi frente a 8,1065 y 2,6129 ms para C extremo. En esa tanda, el C de cálculo más rápido también preparó treinta y dos transiciones.

### Proyección de las cinco últimas filas de `reinas`

Los quince tamaños de control coincidieron con CPython en la ruta habitual y con BMI2 desactivado. Dos tamaños adicionales coincidieron con la búsqueda independiente anterior en C. Los picos residentes medianos fueron 3,63 MiB en Turi y 3,61 MiB en C. El mínimo de cálculo del prototipo pasó de 0,1146 a 0,0676 ms. En 27 rondas del banco oficial, Turi obtuvo 4,8942 ms de proceso y 0,0675 ms de cálculo frente a 4,4631 y 0,0603 ms de las referencias C más rápidas de cada métrica.

Con esta revisión pasaron las 49 pruebas de Windows. La gramática verificó 39 entradas válidas, 33 inválidas y cuatro secuencias UTF-8 inválidas. En la campaña completa de 13 rondas alternadas, los 14 bancos coincidieron con CPython. `nbody` obtuvo 379,39 ms de proceso y 375,63 ms de cálculo frente a 381,03 y 377,80 ms de C extremo. `cuerpos` obtuvo 41,69 y 36,48 ms frente a 42,19 y 37,35 ms. El margen de 1,05× en ambas medidas sigue pendiente en los 14 bancos.

### Tablas constantes y franja corta de `matmul`

El compilador dejó en el ejecutable tablas de 1965 elementos en `reinas`, 210 en `criba`, 1560 en el prefijo general de `matmul` y 165 en `fannkuch`. La prueba con ASan comprobó la lectura recursiva, la copia que se modifica, la escritura por medio de una vista y la vista devuelta por una función. Los picos residentes medianos de los cuatro bancos se mantuvieron entre 3,60 y 3,67 MiB en esta revisión.

La franja corta de `matmul` coincidió en 27 tamaños con CPython 3.11 y en los mismos 27 con CPython 3.13. Los tamaños cubren tres períodos, los dos extremos de la franja y un generador cuya fase no es invertible. Los dos recorridos C, con LTO y con LTO más PGO, coincidieron en ocho tamaños con las dos versiones de CPython. El pico residente mediano del ejecutable de Turi fue de 3,69 MiB frente a 3,66 MiB de C extremo.

Pasaron 80.260 comparaciones de formato fijo con CPython y comprobación de comportamiento indefinido. Tras ampliar el formato fijo, 31 rondas alternadas de `matmul` dieron 4,2803 ms de proceso y 0,0123 ms de cálculo para Turi frente a 4,2358 y 0,0177 ms de los C más rápidos de cada métrica. El cálculo ganó 1,44× en esta campaña. El proceso completo quedó dentro del margen de empate.

Pasaron las 49 pruebas de Windows. Con Lark 1.3.1 y llguidance 1.9.1 se verificaron 39 entradas válidas, 33 inválidas y cuatro secuencias UTF-8 inválidas. La gramática rechazó `fn main() { let a = 2; }`.

La comparación ampliada de `matmul` validó doce tamaños frente a CPython y entrenó el perfil de C con entradas independientes. En 19 rondas medidas, Turi obtuvo 0,0130 ms de cálculo y el C con formato fijo exacto obtuvo 0,0107 ms. En proceso completo, la entrada con tubería de Turi obtuvo 4,1468 ms y la entrada mínima de C obtuvo 3,5365 ms. Se incorporaron las dos variantes C de formato fijo a la comparación oficial.

### Profundidad de expresiones en Windows

Con la reserva de pila anterior, la comprobación de 32 llamadas SIMD anidadas terminaba con `0xc00000fd`. Los ejecutables del compilador reservan ahora 32 MiB de espacio virtual y declaran 4 KiB de compromiso inicial en la cabecera PE. Pasaron las pruebas de núcleo, semántica, arreglos y gramática. El caso de 32 llamadas se ejecutó correctamente y el de 100 devolvió `E_DEPTH`.

En una copia verificada de Ubuntu 24.04 con Clang 23.1.6 y CPython 3.12.3 pasaron las pruebas dirigidas de ejecutables, contadores, texto, mochila, reinas y productos rectangulares. También pasó la gramática con las dependencias fijadas.

La campaña completa de esta revisión reprodujo las 14 salidas de CPython en 19 rondas alternadas. `nbody` obtuvo 379,8 ms de proceso y 370,4 ms de cálculo frente a 382,9 y 372,9 ms de C extremo. `cuerpos` obtuvo 42,0 y 37,2 ms frente a 42,5 y 37,6 ms. La referencia C de `matmul` con salida fija obtuvo 0,0103 ms de cálculo frente a 0,0133 ms de Turi. El margen del 5 % en ambas medidas sigue pendiente en los 14 bancos.

### Prototipos de intervalos en `matmul` y `spectralnorm`

La factorización de la franja de `matmul` conservó la salida en los 16 tamaños del período oficial y en otros 32 tamaños de control, con CPython 3.11 y 3.13. En cada intérprete, la cota cerró 45 de esos 48 tamaños. Las tres cotas abiertas conservan la evaluación ordenada original. En 19 rondas, el prototipo compacto con entrada por tubería pasó de 0,0139 a 0,0073 ms de cálculo frente a la variante con tubería y sin cota. El mejor C con cota compacta obtuvo 0,0053 ms de cálculo. Los procesos más rápidos de Turi y C obtuvieron 4,4597 y 4,6185 ms. Los ejecutables coincidieron con CPython en 21 tamaños.

El polinomio con un recíproco de `spectralnorm` mantuvo los límites de referencia para todas las combinaciones de dimensión y rondas auditadas. En el ejecutable se comprobaron ocho dimensiones frente a CPython y dos más frente al C de potencia completa. Turi pasó de 0,0042 a 0,0022 ms de cálculo. La nueva variante C alcanzó 0,0006 ms. El reloj nativo de C en este banco se detiene antes de formatear, mientras el de Turi incluye ese paso.

### Intervalo de `matmul` integrado

Coincidieron 33 controles con CPython 3.11 y con CPython 3.13, incluidos tres con doce decimales. En la ruta general, LLVM reagrupaba una suma estricta y alteraba el total en hasta tres unidades del último bit de los casos medidos. Al retirar la indicación de vectorización forzada de ese bucle, los totales de ambos casos coincidieron también bit a bit.

En 31 rondas alternadas con la entrada 400, Turi obtuvo 4,0097 ms de proceso y 0,0069 ms de cálculo. Los mínimos de C extremo con entrada estándar fueron 3,9787 y 0,0049 ms. Con entrada mínima, Turi obtuvo 3,3759 y 0,0075 ms frente a 3,1634 y 0,0052 ms de C. Los ejecutables coincidieron con CPython en veinte tamaños. El pico residente mediano fue de 3,66 MiB para Turi estándar y 3,00 MiB con entrada mínima, frente a 3,60 y 2,96 MiB de C con cota de cuatro grupos.

Pasaron las 49 pruebas de Windows. En Ubuntu 24.04 con Clang 23.1.6 y CPython 3.12.3 pasaron las pruebas dirigidas de ejecutables, contadores, texto, mochila, reinas y productos rectangulares. En ambos sistemas la gramática verificó 39 programas válidos, 33 inválidos y cuatro secuencias UTF-8 inválidas con Lark 1.3.1 y llguidance 1.9.1.

### Ensayos con la cola afín y la consulta por prefijo

La cola de `matmul` se puede evaluar con una expresión afín por fila y una corrección por cada vuelta modular. Las tres variantes en Turi y las dos referencias de C con este método coincidieron con CPython en veinte dimensiones. En 31 rondas, el mejor cálculo de Turi bajó de 7,6 a 6,6 µs. El mejor C con la cola afín tomó 5,9 µs y el mejor C con la cota anterior tomó 4,6 µs.

La consulta de `texto` a partir de los tres primeros bytes conservó la salida de CPython en doce archivos. Su variante más rápida en Turi tomó 13,6640 ms de cálculo, frente a 13,1097 ms en la base y 11,4870 ms en el C extremo más rápido.

### Campaña completa con el intervalo integrado

Los 14 ejecutables coincidieron byte a byte con CPython 3.11 durante 19 rondas alternadas. Cinco quedaron por delante de sus referencias más rápidas en ambas medidas. En `cuerpos`, los mínimos fueron 43,3602 ms de proceso y 38,6079 ms de cálculo en Turi, frente a 47,0706 y 40,1603 ms de C extremo. Los cocientes fueron 1,0856× y 1,0402×. El margen de 1,05× en ambas medidas sigue pendiente en los 14 bancos.

La prueba dirigida reservó una lista de 16.777.217 valores `u8` en el ejecutable, escribió el último elemento y comprobó ambos extremos y la longitud.

Con AddressSanitizer y UBSan pasaron las cinco pruebas dirigidas de arreglos, primera escritura, pila, semántica y límites. Pasaron también los ejecutables instrumentados de contadores y el ejecutable independiente, incluida la recuperación de excepciones y el producto periódico con doce decimales.

### Ejes empaquetados y redondeo de la salida

El modelo Zen 4 estimó una presión alta en la unidad que ejecuta las permutas, raíces y divisiones de `cuerpos`. Las tres disposiciones con dos ejes en un vector conservaron las 30 coordenadas y velocidades en diez duraciones de `nbody` y de `cuerpos`. En 23 rondas, el cálculo de `cuerpos` tomó 42,0115 ms en la base Turi y 48,8297 ms con la disposición nueva más rápida. El C extremo más rápido en cálculo obtuvo 42,5961 ms.

El prototipo de formato redondea el valor escalado en f64 cuando está por debajo de 2⁵² y su parte fraccionaria difiere de 0,5. Los empates y los tamaños mayores se resuelven con la mantisa entera. Pasaron 80.260 formatos frente a CPython con UBSan. En `matmul` coincidieron veinte dimensiones. El cálculo mínimo de Turi pasó de 9,5 a 8,4 µs, frente a 6,6 µs del C extremo más rápido. Los mínimos de proceso pasaron de 6,3727 a 6,6461 ms en Turi.

Se ensayaron 33 disposiciones de carriles en `cuerpos`, incluidas 18 con un ciclo de parejas orientado. La repetición enfrentó cuatro disposiciones y la base a 14 referencias de C extremo. La base Turi dio 45,9703 ms de proceso y 37,8271 ms de cálculo. La disposición más rápida en proceso tomó 45,0684 ms de proceso y 40,0580 ms de cálculo. Los mínimos de C extremo fueron 45,6019 y 39,4279 ms. Los finalistas conservaron las 30 coordenadas y velocidades en diez duraciones de cada programa. El finalista orientado reprodujo cinco casos con ceros de ambos signos.

Seis configuraciones del planificador LLVM conservaron el mismo núcleo de `cuerpos`. En siete duraciones se comprobó la salida byte a byte frente a CPython. En 23 rondas finales, Turi base obtuvo 44,4922 ms de proceso y 38,1977 ms de cálculo. Planificar después de asignar los registros bajó el cálculo a 38,1436 ms y subió el proceso a 46,1581 ms. Los mínimos de C extremo con las mismas opciones disponibles fueron 46,3752 y 39,8663 ms.

### Catálogo y cota integral de `mochila`

El catálogo de solo lectura de 440 elementos se compartió en la imagen del ejecutable. La búsqueda por prefijos usó 4912 sumas parciales adicionales. En 23 rondas alternadas, Turi base y el catálogo compartido obtuvieron 6,3 µs de cálculo. La búsqueda por prefijos tomó 11,0 µs y subió el pico residente mediano de 3,61 a 3,64 MiB. Los trece ejecutables en Turi y C coincidieron con CPython en dieciocho tamaños.

La cota por residuos módulo cinco usa la menor pérdida posible para cada cambio de peso alrededor del pivote fraccionario. Pasó 96.562 combinaciones de catálogo, prefijo y capacidad frente a programación dinámica. Los ejecutables coincidieron con CPython en dieciocho tamaños y las dos variantes con cota reprodujeron la fila completa en los 441 prefijos del catálogo. En 31 rondas, Turi bajó de 8,0 a 4,2 µs de cálculo y mantuvo 3,61 MiB de pico residente mediano. C extremo con el mismo certificado obtuvo 8,0 µs con arranque estándar y 1,7 µs con entrada mínima. Los tiempos de proceso de esta tanda presentaron variación alta entre rondas.

La cota se integró en el catálogo generado por Turi y en la referencia C con LTO y PGO. Pasaron las pruebas de Windows del ejecutable independiente, los contadores con generadores originales y alterados, incluida la entrada 400 de mochila, y la mochila general. Se ensayó buscar el pivote con lotes SIMD de 8, 16 y 32 objetos. En la repetición, Turi escalar obtuvo 3,2 µs de cálculo y el ancho 16 obtuvo 3,6 µs. C extremo con ancho 16 y entrada mínima tomó 0,9 µs de cálculo.

La batería completa de Ubuntu 24.04 pasó las 47 pruebas con Clang 23.1.6 y CPython 3.12.3. El trabajo de gramática con Lark 1.3.1 y llguidance 1.9.1 verificó 39 programas válidos, 33 inválidos y cuatro secuencias UTF-8 inválidas.

Para buscar el pivote por sumas acumuladas, la tabla completa de 14.112 palabras se redujo a 1792 al guardar un prefijo de cada ocho. Una tabla inversa de 440 palabras permite descontar los objetos fuera del prefijo pedido. Este prototipo reprodujo dieciocho salidas de CPython y los 441 tamaños frente a la base Turi. En 27 rondas, el mínimo de cálculo bajó de 3,2 a 2,5 µs. El pico residente mediano pasó de 3,61 a 3,62 MiB. El mínimo de proceso fue 4,9146 ms en la base y 5,1212 ms en el prototipo. C extremo con entrada mínima alcanzó 3,5849 ms de proceso con la tabla compacta y 1,4 µs de cálculo con la búsqueda escalar.

### Conversión decimal del argumento del ejecutable

Las dos rutas de conversión decimal en una pasada coincidieron con CPython en 28 controles de argumentos, errores y codificaciones. En 27 rondas por banco, la variante que lee directamente los bytes en el anfitrión redujo el cálculo de `mochila` de 3,6 a 3,3 µs y el de `dijkstra` de 1,0 a 0,9 µs. `spectralnorm` obtuvo 4,6 µs con la base y 4,8 µs con esa variante. El mínimo de proceso bajó en `mochila` y subió en `dijkstra` y `spectralnorm`.
