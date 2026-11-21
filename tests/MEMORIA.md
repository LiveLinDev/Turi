# Auditoría de memoria de arrays

El 20 de noviembre de 2026 se ejecutó el programa estático `PROGRAM` de `tests/arrays.py` en Windows x86-64 con Clang 23.1.4 y AddressSanitizer. Durante 600 iteraciones el array temporal aumenta de longitud. El programa modifica el último elemento, vuelve antes de completar el bucle, construye arrays por retorno de funciones, copia y reasigna valores y comprueba que las modificaciones de las copias conservan el valor original.

| Optimización del ejecutable auditado | Reservas de Turi | Máximo de reservas vivas simultáneas | Reservas vivas al salir |
|---|---:|---:|---:|
| `-O0` | 97 | 4 | 0 |
| `-O2` | 97 | 4 | 0 |

La región usa un bloque inicial de 4096 bytes en el heap. Al salir de un ámbito recupera el marcador de la región y libera los bloques adicionales creados dentro. Una iteración que cabe en el primer bloque puede reutilizarlo. En este programa, las iteraciones a partir de 513 enteros de 8 bytes necesitan espacio adicional. Las cifras de la tabla también incluyen las reservas de dueño único y sus copias.

Desde la raíz del repositorio, con una compilación actual de `veloz`, se puede repetir la auditoría así.

```powershell
python tests/allocation_audit.py build/veloz.exe "C:/Program Files/LLVM/bin/clang.exe"
```

El script toma el IR LLVM que genera `veloz emit-ir`, redirige sus llamadas a `malloc` y `free` a un contador y usa el asignador del sistema para reservar y liberar. En cada nivel de optimización exige que el IR instrumentado tenga comprobaciones de AddressSanitizer, ejecuta el programa, compara su salida completa y verifica cero reservas vivas al terminar. El contador abarca las llamadas del módulo de Turi. Los resultados corresponden a este programa y sus entradas. Los tiempos y el RSS se miden por separado en ejecutables de producción.

## Comprobaciones relacionadas

- `python tests/arrays.py build/veloz.exe` compara el intérprete y los binarios nativos con valores esperados. Cubre región, dueño único, retorno, índice negativo y fuera de límite, tamaño negativo o excesivo y errores de tipo.
- `python tests/arrays.py build/veloz.exe --clang-asan "C:/Program Files/LLVM/bin/clang.exe"` comprueba también accesos a memoria en el ejecutable instrumentado. En Windows debe estar disponible el DLL de AddressSanitizer de Clang al ejecutar este comando.
- `python tests/native_edges.py build/veloz.exe` verifica los límites de 256 llamadas, el orden de evaluación con efectos observables y los tramos de las trampas de aritmética y aserciones.

## Pico residente por número de iteraciones

El 20/11/2026 se ejecutaron dos series en Windows x64 con Clang 23.1.4. Cada proceso reserva, llena y lee un arreglo de aproximadamente 1 MiB por iteración. Su longitud varía entre 131.072 y 131.102 enteros. `veloz costes` asignó el punto `repeat` al nivel 2. El IR optimizado de la mayor carga conserva la reserva del bloque adicional, las escrituras vectorizadas, una lectura de sus datos y la liberación del bloque.

| Iteraciones | Suma de control | Pico en KiB, serie 1 (mín./mediana/máx.) | Pico en KiB, serie 2 (mín./mediana/máx.) |
|---:|---:|---:|---:|
| 4 | 26 | 4644 / 4648 / 4656 | 4644 / 4644 / 4656 |
| 32 | 656 | 4644 / 4648 / 4660 | 4644 / 4648 / 4660 |
| 256 | 33920 | 4640 / 4648 / 4660 | 4644 / 4644 / 4656 |
| 2048 | 2106368 | 4644 / 4648 / 4672 | 4644 / 4644 / 4668 |

Cada serie contiene siete procesos nuevos por tamaño, mezclados con semilla 7921. La CLI compiló cada tamaño una vez con `veloz build`. El instrumento consulta `PeakWorkingSetSize` con `K32GetProcessMemoryInfo` después de esperar al proceso, conservando abierto su handle. El dato incluye las páginas residentes del proceso completo. Cada ejecución verificó su salida con `n*(n-1)/2 + 5*n`. El pico se mantuvo entre 4.640 y 4.672 KiB al multiplicar por 512 la cantidad de iteraciones. Esta observación cubre una región de arreglo temporal por iteración en este equipo.

```powershell
python tests/rss_windows.py build/veloz.exe
```

La revisión independiente incluyó una devolución temprana desde un bucle a través de una identidad genérica, nueve retornos recursivos de copias, autoasignación, modificación de una copia y tres ámbitos de arreglos anidados con cortocircuito. El intérprete y el ejecutable compilado con `-O1 -fsanitize=address` devolvieron 2464 y pasaron las aserciones. También se comprobó en el IR instrumentado que Clang inserta comprobaciones de lecturas y escrituras.

## Transferencia de un dueño al devolver un arreglo local

El IR transfiere la reserva de un arreglo local al resultado cuando su ranura es dueña del heap y el retorno constituye su último uso en ese camino. Primero carga el valor y vacía la ranura, después ejecuta la limpieza normal de todos los ámbitos activos. Un parámetro prestado se copia al devolverlo. La prueba `RETURNS` de `tests/arrays.py` ejercita un parámetro prestado, dos devoluciones tempranas dentro de un bucle con región, una variable interior que oculta a otra y la devolución tras reasignar el dueño exterior. También modifica cada resultado y comprueba la independencia del argumento original.

La auditoría con ASan y el contador anterior dio las mismas cifras en `-O0` y `-O2`.

| Programa | Reservas antes | Reservas después | Pico simultáneo antes / después | Vivas al terminar antes / después |
|---|---:|---:|---:|---:|
| Programa de 600 iteraciones, `PROGRAM` | 97 | 96 | 4 / 4 | 0 / 0 |
| Cuatro rutas de devolución, `RETURNS` | 13 | 10 | 5 / 4 | 0 / 0 |

El comando `allocation_audit.py` de arriba reproduce la primera fila. Para contar las reservas del caso de cuatro retornos con el mismo instrumento usa el siguiente comando desde la raíz.

```powershell
python -c "import sys; sys.path.insert(0, 'tests'); import arrays, allocation_audit as a; a.PROGRAM, a.EXPECTED = arrays.RETURNS, arrays.RETURN_OUTPUT; a.main()" build/veloz.exe "C:/Program Files/LLVM/bin/clang.exe"
```
