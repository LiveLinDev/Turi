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
