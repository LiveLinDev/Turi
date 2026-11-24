# Verificación del compilador en Windows x64

## Revisión con módulos y rangos

La revisión actual pasó **13/13 grupos CTest** en release y con ASan/UBSan. Incluye proyectos, contexto A/B, criterio de victorias, límites afines, rangos y auditoría de copias, además de los grupos históricos.

Durante la revisión, `fix` con `fn f(a: [int]) { a[0]=5 }` y una reparación en un proyecto produjeron un fallo de excepciones bajo ASan/UBSan. Controles mínimos mostraron que `check` directo funcionaba y que la captura seguida de reelevación dentro de la ruta con `Program` reproducía el fallo. La corrección captura `Diagnostic` como valor mediante `captureDiagnostic`, lo relanza fuera del `catch` y permite que `fix` reporte directamente sin reelevar. No se desactivó ASan ni UBSan en ninguna función.

La puerta sanitizada usa el mismo script que release:

```powershell
powershell -File tools/build.ps1 -BuildDir build-sanitize-review -Sanitize
```

El parámetro localiza LLVM, CMake y Ninja, configura `RelWithDebInfo` con `-fsanitize=address,undefined`, `-fno-sanitize-recover=all`, instala las opciones `halt_on_error` y ejecuta los 13 grupos. La entrada de `fix` permanece en este informe como reproducción del fallo corregido.

`tests/ranges.py` añade 31 casos de cotas y respaldos, incluidos dos iteradores homónimos anidados que antes podían ocultar un overflow. Una revisión detectó que `len(texto)` había recibido incorrectamente el tope de arrays. La regresión construye 16.777.217 caracteres ASCII y comprueba que `len(texto) * 549755813887` produce `R_OVERFLOW` en intérprete y nativo; la cota `[0,2²⁴]` ahora solo aplica a `[int]`. Otros casos cubren `repeat(x*x)`, reducciones, mutaciones, retornos, shadowing, recursión y los extremos de `int64`.

## Evidencia histórica

La versión 0.2 amplió CTest a siete grupos, añadiendo arreglos y eliminación comprobada del primer llenado. El compilador instrumentado y la compilación release pasaron los siete grupos. La instrumentación de los propios programas Turi se detalla en `MEMORIA.md`.

El 20/11/2026 se compiló el compilador C++23 y el ejecutor de pruebas con Clang 23.1.4, AddressSanitizer y UndefinedBehaviorSanitizer, usando CMake y Ninja. CTest pasó los cinco grupos registrados, con un tiempo conjunto de 8,69 s en esta máquina.

| Grupo | Comprobación |
|---|---|
| `semantics` | 3.405 programas independientes. El oráculo de Python comprueba las cinco operaciones enteras para 669 parejas de operandos. Otros 60 casos cubren ámbitos, cortocircuito, llamadas, tipos, agregados, genéricos y presupuestos de recursos. Semilla 61593. |
| `core` | Ejemplos, tipos, aritmética comprobada, pruebas del usuario y diagnósticos. |
| `agent` | Herramientas de edición y consulta de la CLI. |
| `differential` | Coincidencia entre el intérprete y los ejecutables producidos, con los ejemplos, ocho lotes de doce funciones y cinco trampas. Semilla 20261120. |
| `native_edges` | Ocho casos de equivalencia entre el intérprete y el backend. |

ASan y UBSan instrumentan el compilador y el ejecutor de pruebas. En la prueba diferencial, `veloz build` enlaza el IR generado con las opciones normales del producto. El comando de Clang para repetir la instrumentación se puede consultar en `build-asan/compile_commands.json` si se activa `CMAKE_EXPORT_COMPILE_COMMANDS`.

## Reproducir con PowerShell

Desde la raíz del repositorio, con LLVM x64, CMake, Ninja, Python 3.10 o posterior y las herramientas de enlace de Visual Studio instaladas.

```powershell
$compiler = 'C:/Program Files/LLVM/bin/clang++.exe'
$resourceDir = & $compiler -print-resource-dir
$env:PATH = "$resourceDir/lib/windows;C:/Program Files/LLVM/bin;$env:PATH"
$env:ASAN_OPTIONS = 'halt_on_error=1'
$env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
cmake -S . -B build-asan -G Ninja "-DCMAKE_CXX_COMPILER=$compiler" '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' '-DCMAKE_BUILD_TYPE=RelWithDebInfo'
cmake --build build-asan --parallel 4
ctest --test-dir build-asan --output-on-failure -j 3
```

## Mutación guiada por cobertura

LibFuzzer completó 20.000 ejecuciones del frontend y del intérprete con ASan y UBSan. Partió de los cuatro programas de `examples/`, con semilla 20261120 y longitud máxima de 2.048 bytes. El informe final registró `cov: 7006`, 20.691 características y 314 entradas en el corpus. El límite de 2.000 instrucciones y 32 llamadas por ejecución permite probar también bucles y recursión. Se capturan los diagnósticos esperados del lenguaje. Las excepciones imprevistas y fallos de los sanitizadores terminan el proceso.

Tras la preparación del entorno PowerShell anterior, usar una carpeta de corpus nueva para partir de los mismos ejemplos.

```powershell
& $compiler -std=c++23 -O0 -g '-fsanitize=fuzzer,address,undefined' -fno-sanitize-recover=all -fno-omit-frame-pointer -D_DISABLE_STRING_ANNOTATION -D_DISABLE_VECTOR_ANNOTATION -Isrc tests/semantics_fuzz.cpp src/parser.cpp src/semantics.cpp src/generics.cpp -o build-asan/semantics-fuzz.exe
New-Item -ItemType Directory -Force build-asan/fuzz-run | Out-Null
& ./build-asan/semantics-fuzz.exe -runs=20000 -seed=20261120 -max_len=2048 -timeout=5 -artifact_prefix=build-asan/ build-asan/fuzz-run examples
```

En este paquete de LLVM para Windows, el primer enlace del fuzzer falló por incompatibilidad en `annotate_string` con la biblioteca precompilada de libFuzzer. Las dos macros ajustan las anotaciones de contenedores de la STL para esa compilación. Los cinco grupos de CTest usan las anotaciones ordinarias.

La compilación de cobertura a `-O1` terminó con la excepción de Windows `0x80000003` al ejecutar la entrada vacía. Un control independiente en C++ reprodujo el fallo lanzando y capturando una excepción con varios campos `std::string` construidos por valor. Un control mínimo con `std::runtime_error` pasó. El cambio aislado de `-fno-sanitize-coverage=trace-cmp` corrigió la ejecución del control y del compilador, manteniendo la cobertura de aristas, ASan y UBSan. Repitiendo las órdenes anteriores con `-O1 -fno-sanitize-coverage=trace-cmp` y un corpus nuevo, el compilador pasó otras 20.000 ejecuciones. Resultado final de esa configuración: `cov: 2368`, 6.109 características y 313 entradas.

## Recursos de la pila

El intérprete almacena sus marcos de llamada en memoria dinámica. Una prueba alcanza 1.500 llamadas con un presupuesto ampliado de 2.000. El límite ordinario es de 256 llamadas simultáneas.

El comprobador limita los tipos a 32 niveles y a una estimación conservadora de 4.096 bytes de disposición por tipo. En las funciones reserva un presupuesto estimado de 16.384 bytes, con 256 fijos y dos veces el coste de cada parámetro, enlace local y expresión con valor. Carga también el tipo devuelto. Un escalar cuesta 16 bytes en este cálculo. Dos pruebas cubren la aceptación y el rechazo por tamaño combinado de los parámetros. El enlazador reserva 32 MiB de pila para los ejecutables que produce la CLI en Windows x64.

La estimación se calcula sobre la fuente. La cantidad exacta de pila depende de las instrucciones emitidas por LLVM y de la ABI. Los ensayos en esta máquina cubren los límites del comprobador y el comportamiento de las llamadas del intérprete.
