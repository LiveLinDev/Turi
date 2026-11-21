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
