# Turi frente a C, C++, Rust, Goose y Go

Se ejecutaron 100 algoritmos distintos en Windows sobre Ryzen 7 7700X y los mismos 100 con otras entradas en Linux ARM64 sobre DGX Spark GB10. Una segunda campaña volvió a ejecutar ambas matrices después de modificar el compilador. Todas las muestras aceptadas devolvieron el valor del oráculo.

## Velocidad del ejecutable

Cada factor es tiempo del otro lenguaje dividido entre tiempo de Turi. Un factor mayor que uno favorece a Turi. Cada carga tiene el mismo peso. Las tres cifras por fila son cargas con ventaja confirmada de Turi, resultado indeterminado y desventaja confirmada según el intervalo descriptivo por caso.

| Comparación | Ryzen, factor | Gana / indet. / pierde | Spark, factor | Gana / indet. / pierde |
|---|---:|---:|---:|---:|
| C | 0.886× | 12 / 30 / 58 | 0.740× | 7 / 11 / 82 |
| C++ | 0.889× | 14 / 26 / 60 | 0.904× | 22 / 17 / 61 |
| Rust | 0.932× | 11 / 43 / 46 | 0.782× | 5 / 16 / 79 |
| Goose | 0.836× | 15 / 14 / 71 | 0.811× | 12 / 14 / 74 |
| Go | 1.306× | 69 / 16 / 15 | 1.438× | 86 / 8 / 6 |
| Turi inicial | 1.040× | 25 / 71 / 4 | 1.017× | 18 / 80 / 2 |
| Turi intermedio | 1.015× | 10 / 84 / 6 | 1.016× | 14 / 84 / 2 |

Turi inicial es la versión previa a las dos optimizaciones generales de índices y profundidad. Turi intermedio ya tenía esas optimizaciones. La última versión también dimensiona el primer bloque de la región según los datos y separa el código frío de las trampas. Las tres versiones ejecutaron las mismas entradas en la campaña final.

### Diferencias por familia contra C

| Familia | Ryzen | Spark |
|---|---:|---:|
| aritmetica | 0.948× | 0.951× |
| flujos | 1.009× | 0.948× |
| busqueda orden | 0.759× | 0.742× |
| memoria | 0.938× | 0.700× |
| estadistica | 0.886× | 0.504× |
| matrices | 0.663× | 0.433× |
| dinamica | 0.778× | 0.680× |
| estructuras | 0.897× | 0.822× |
| codificacion | 0.999× | 0.898× |
| aplicaciones | 1.067× | 0.961× |

En una compilación aislada de la multiplicación de matrices para Ryzen, el IR optimizado de Turi conserva 13 sitios de comprobación de desbordamiento. En el de C aparecen 18 operaciones vectoriales enteras y en el de Turi, cero. Son recuentos estáticos de todo el núcleo. Una oportunidad concreta es demostrar rangos para accesos bidimensionales y para acumuladores antes de vectorizarlos, conservando la semántica del desbordamiento. [Recuento reproducible](resultados/final/inspeccion-ir.json).

## Memoria automática

- Ryzen tuvo 64 casos con reservas en C. Turi redujo el pico en 57, lo mantuvo en 2 y lo aumentó en 5 frente a la primera campaña. La mediana del pico entre esos programas pasó de 25,900 a 21,780 bytes. Ningún caso terminó con bytes de arreglos pendientes.
- Spark tuvo 64 casos con reservas en C. Turi redujo el pico en 58, lo mantuvo en 1 y lo aumentó en 5 frente a la primera campaña. La mediana del pico entre esos programas pasó de 25,916 a 21,796 bytes. Ningún caso terminó con bytes de arreglos pendientes.

Al repetir ambos asignadores con exactamente las mismas entradas del Ryzen, 57 de 64 programas con reservas redujeron el pico. Otros 5 lo aumentaron. Fueron scopeChurn, editDistance, commonSubsequence, increasingSubseq, rodCut. El total de reservas en las 100 invocaciones comparables pasó de 120 a 97. [Auditoría con entradas idénticas](resultados/final/memoria-emparejada.json).

AddressSanitizer comprobó 200 combinaciones de programa y semilla en Ryzen, con cuatro llamadas por combinación. Se verificaron también tres tamaños hasta 1.048.576 elementos en tres programas. Los contadores comprueban las reservas del código generado. El consumo total de cada proceso figura por separado en la columna RSS del CSV.

## Cuánto cuesta llegar al código correcto

Qwen2.5 7B local recibió 16 tareas escalares reservadas por condición, un ejemplo público por tarea y hasta tres intentos. Cada acierto superó además los casos privados, que nunca entraron en el contexto. Go y Rust se ejecutaron en WebAssembly con recursos acotados y Turi en el intérprete. Los totales incluyen los fallos, los reintentos y la carga inicial. Tokens efectivos suma los tokens de entrada evaluados de nuevo y los de salida. Tokens lógicos incluye también las entradas servidas desde caché.

| Lenguaje y método | Tarjeta | Primer intento | Hasta 3 | Tokens lógicos | Tokens efectivos | Efectivos por acierto |
|---|---:|---:|---:|---:|---:|---:|
| Go · firma precargada | 51 | 13/16 | 14/16 | 4643 | 2247 | 160.5 |
| Go · tarjeta y función completa | 283 | 11/16 | 13/16 | 11325 | 2907 | 223.6 |
| Rust · firma precargada | 69 | 10/16 | 13/16 | 5611 | 2299 | 176.8 |
| Rust · tarjeta y función completa | 285 | 8/16 | 8/16 | 14821 | 4075 | 509.4 |
| Turi · sin tarjeta | 0 | 0/16 | 0/16 | 10556 | 4290 | — |
| Turi · tarjeta y correcciones verificadas en frío | 279 | 13/16 | 13/16 | 9155 | 9155 | 704.2 |
| Turi · tarjeta y correcciones verificadas | 279 | 13/16 | 13/16 | 8600 | 2206 | 169.7 |
| Turi · tarjeta y función completa | 279 | 10/16 | 10/16 | 13085 | 2888 | 288.8 |
| Turi · firma y gramática en frío | 124 | 12/16 | 12/16 | 6153 | 6153 | 512.8 |
| Turi · firma y gramática | 124 | 12/16 | 12/16 | 7174 | 2721 | 226.8 |

Con Qwen2.5 3B se repitieron las cuatro condiciones de Turi que aíslan la tarjeta, las correcciones y la caché.

| Método | Primer intento | Hasta 3 | Tokens lógicos | Tokens efectivos |
|---|---:|---:|---:|---:|
| Turi · sin tarjeta | 0/16 | 0/16 | 12894 | 5231 |
| Turi · tarjeta y correcciones verificadas en frío | 6/16 | 7/16 | 17304 | 17304 |
| Turi · tarjeta y correcciones verificadas | 5/16 | 6/16 | 18249 | 5058 |
| Turi · tarjeta y función completa | 5/16 | 6/16 | 18345 | 5158 |

En Turi fixed con 7B, la tarjeta sola tiene 279 tokens. Cargar el prefijo completo costó 315 tokens frescos. En 16 tareas, la condición cálida acumuló 6394 tokens de entrada reutilizados. El total efectivo pasó de 9155 en frío a 2206 con caché. Los totales incluyen las salidas y las reparaciones.

Para aislar el efecto de caché, se reprodujeron los mismos 16 primeros mensajes en dos órdenes y con una salida de exactamente un token. El servidor evaluó 11978 tokens de entrada en frío y 1792 con reutilización, una reducción del 85.0 %. Ambas condiciones recibieron exactamente el mismo texto. [Contadores de las 64 peticiones](resultados/ia-final/cache-replay.json).

En el ejemplo de Euclides, permitir parámetros escalares modificables redujo la función de 57 a 47 tokens con el mismo tokenizador y el mismo resultado. [Programa Turi](../../examples/euclides.turi).

bare pide la función completa sin tarjeta. full pide la función completa con la tarjeta. fixed añade las ediciones verificadas del compilador al mismo mensaje. body precarga la firma y usa una tarjeta abreviada. guided precarga la firma, limita las continuaciones con una gramática escalar y permite ediciones verificadas. cold fuerza un prellenado desde cero en cada petición. La gramática se entrega como opción del decodificador. Su tamaño en bytes se conserva por separado. Los cómputos locales no constituyen un precio de una API.

## Alcance y trazabilidad

La Spark ejecutó exclusivamente los binarios ARM en un núcleo, con prioridad ociosa. Los registros conservan los descartes y las ventanas de espera ante uso de la GPU, conexiones, contadores del gateway y otros procesos. La cadencia es de unos 300 ms y los contadores de admisión cubren las solicitudes del gateway. Los resultados miden CPU. Las cargas cubren enteros y arreglos y los resultados caracterizan estos programas, compiladores y máquinas.

El C y C++ de comparación expresan el mismo algoritmo. C, Goose y Turi usan Clang 23 en los dos destinos. C++ ARM usa Zig 0.15.2 y su Clang/libc++. Go y Rust conservan sus propios compiladores. Las filas individuales tienen diez bloques y sus intervalos no corrigen por comparaciones múltiples. No se ejecutó el protocolo de Intel físico del expediente.

El detalle por programa incluye tiempos, intervalos, reservas y pico residente. Las observaciones por bloque están en archivos JSONL comprimidos. [Detalle por programa](resultados/final/comparacion.csv) · [Tokens por condición](resultados/final/tokens.csv) · [Datos Ryzen](resultados/final/ryzen.jsonl.gz) · [Datos Spark](resultados/final/spark.jsonl.gz) · [Fuentes de los seis lenguajes](resultados/final/fuentes.zip) · [Protocolo](PROTOCOLO.md).
