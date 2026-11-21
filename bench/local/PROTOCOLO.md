# Ensayo exploratorio, Ryzen 7 7700X y Windows

Protocolo fijado antes de ejecutar los binarios. Las comparaciones de esta carpeta sirven para investigar en la máquina de desarrollo. La comparación competitiva del expediente requiere otro entorno y más programas.

Se fijan tres cargas monohilo y las semillas de entrada estándar 17, 97 y 719. El formato es un entero decimal y un salto de línea. Se comparan procesos completos, incluida su inicialización. Cada ejecutable imprime únicamente el checksum decimal. Una salida distinta o un timeout de 10 s constituye un fallo y se conserva. El compilador lee la semilla en ejecución. Todos los cálculos y elementos de arreglo usan 64 bits.

1. **Escalar.** Partir de la semilla y repetir ocho millones de veces `x = (x * 48271 + 1) % 2147483647`. Imprimir el estado final.
2. **Arreglo.** Crear 262144 elementos con la semilla. Escribir `(i + seed) % 1009` en cada posición. Ejecutar 32 pasadas que añaden `p + 1` y reducen módulo 1009 cada elemento. Imprimir la suma final.
3. **Temporales.** Para cada `r` de 0 a 19999, crear un arreglo de `32 + (r + seed) % 2017` elementos con valor `r % 251`. Reemplazar cada elemento por `(a[i] + i + seed) % 251`. Acumular el valor en `(r + seed) % n`. Imprimir el acumulado.

El oráculo independiente de Python usa exponenciación de la recurrencia afín para el escalar y fórmulas equivalentes para el resultado de los arreglos. Los programas que se cronometran sí realizan el algoritmo completo. Se permiten las optimizaciones del compilador. Ningún fuente incluye un resultado precalculado.

Por cada carga y ejecutable se hacen tres calentamientos, uno por semilla, y 30 ejecuciones medidas, diez por semilla. Se alternan los órdenes en bloques aleatorios de semilla fija. En Windows se fija al proceso del medidor y sus hijos una sola CPU lógica permitida. Se conserva cada tiempo y cada checksum. Se informa la mediana por caso y el cociente pareado de tiempos C/Turi con intervalo bootstrap del 95 % por bloques de semilla.

La fila de C usa Clang con `-O3 -march=x86-64-v3 -flto`. Una segunda fila C comprueba desbordamiento y límites. La versión Turi usa los mismos flags sobre LLVM IR e incluye todas sus comprobaciones. Se intenta compilar Goose con su traductor y la misma configuración de Clang. Rust y Go usan sus configuraciones de release locales. Las versiones, opciones exactas y cualquier diferencia se registran junto a los resultados.

Se usa una compilación por variante. Turbo, SMT, servicios Windows y el plan de energía conservan sus ajustes. Las unidades de traducción de este ensayo no tienen PGO ni BOLT. No se usa la media de estas tres cargas como estimación general del rendimiento de los lenguajes.
