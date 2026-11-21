# Turi · núcleo 0.2

Esta especificación describe los programas que acepta la primera implementación. La gramática para generación se distribuye en `grammar/turi.lark` y `grammar/turi.gbnf`. El expediente conserva el diseño de futuras versiones.

## Archivo y declaraciones

Un archivo UTF-8 contiene cero o más declaraciones de función, struct, enum o prueba en cualquier orden. Un salto de línea separa declaraciones y sentencias consecutivas. Se admiten líneas vacías, espacios, tabuladores y comentarios desde `//` hasta fin de línea. Las llaves abren y cierran bloques. Se admite una sentencia corta entre las llaves en la misma línea. La continuación de una expresión se permite inmediatamente tras un operador, una coma o un paréntesis de apertura. Los tipos y valores tienen espacios de nombres separados. Los valores usan `[a-z][A-Za-z0-9]*` y los tipos nominales empiezan por mayúscula ASCII.

```turi
fn max(a: int, b: int) -> int {
    if a > b { return a } else { return b }
}
```

Las firmas requieren tipos. Al omitir el resultado se obtiene `void`. Se permiten llamadas adelantadas y recursión. `fn main() {}` es el punto de entrada de un ejecutable. Un archivo de biblioteca o de pruebas puede carecer de él. Un `test "nombre" { ... }` se ejecuta con `veloz check` después de comprobar todo el archivo. Los nombres de prueba son únicos. Una prueba dispone de su propio presupuesto de ejecución.

## Valores y operaciones

| Tipo | Semántica |
|---|---|
| `int` | Entero de 64 bits con signo. Operaciones `+ - * / %` y menos unario. Una operación que desborda produce `R_OVERFLOW`. La división trunca hacia cero. Divisor cero produce `R_DIV_ZERO`. Tanto `min / -1` como `min % -1` producen `R_OVERFLOW`. |
| `bool` | `true`, `false`, `!`, `&&` y `\|\|`. La derecha de una operación lógica se evalúa solo cuando puede cambiar el resultado. |
| `str` | Texto UTF-8 inmutable. Los escapes incluyen `\n`, `\r`, `\t`, `\\` y `\"`. `len(texto)` cuenta bytes. La igualdad compara el contenido completo. |
| `void` | Retorno sin valor. Una llamada que devuelve void puede ser una sentencia. |

Todos los escalares admiten `==` y `!=` entre valores del mismo tipo. El orden `< <= > >=` requiere enteros. La precedencia de menor a mayor es disyunción, conjunción, igualdad, comparaciones de orden, suma/resta, multiplicación/división/resto, unarios y proyección de campo. Los operadores binarios de un mismo nivel asocian a la izquierda. La evaluación de argumentos y operandos avanza de izquierda a derecha. `print(valor)` escribe un escalar y el byte LF. `assert condición` produce `R_ASSERT` cuando es falsa.

## Ámbitos y control

`let` crea un nombre inmutable. `var` permite reasignarlo, conservando su tipo. Ambos requieren un inicializador y aceptan anotación local. Las asignaciones compuestas `+= -= *= /= %=` aplican las mismas comprobaciones que la operación ordinaria. Un bloque anidado puede ocultar un nombre exterior. Los parámetros y el índice de un rango son inmutables.

`if condición { ... } else { ... }` y `while condición { ... }` exigen booleanos. `} else if` se escribe en la misma línea. `for i in a..b` evalúa los límites enteros una vez y recorre el intervalo ascendente, excluido `b`. Si `a >= b`, el cuerpo se ejecuta cero veces. `return` sale de toda la función. Un resultado con valor requiere retorno explícito en todos los caminos. El comprobador verifica también código inalcanzable.

## Productos, sumas y genéricos

Un struct tiene campos tipados. Un constructor `Point { x: 1, y: 2 }` especifica exactamente una vez cada campo. Los argumentos se evalúan en el orden escrito, aunque difiera del declarado. `p.x` lee un campo. Una asignación de un struct reemplaza el valor entero. Se admite anidar, pasar y devolver valores nominales. La semántica de valor conserva la copia anterior al reasignar otra variable.

Un enum declara variantes vacías o con parámetros nombrados. Se construye con `Search.Missing` o `Search.Found(42)`. En un `match` el discriminante se evalúa una vez. Cada brazo lleva un nombre calificado y cero o más nombres locales ligados por posición. Los brazos deben cubrir exactamente las variantes del tipo. Tras `=>` se admite una sentencia o un bloque. Los agregados carecen de igualdad y de impresión implícita.

Una función `fn id<T>(x: T) -> T { return x }` acepta argumentos de tipo inferidos a partir de sus parámetros. Cada parámetro de tipo necesita al menos una aparición en los parámetros de valor. El compilador comprueba el cuerpo contra tipos opacos incluso para funciones sin llamadas. Comprueba también sus especializaciones concretas y emite una por combinación usada. En esta versión los tipos de datos nominales son monomorfos.

## Comprobaciones y recursos

### Arreglos de enteros y entrada

`repeat(elemento, cantidad)` evalúa dos enteros y devuelve un `[int]` de esa longitud, inicializado con el elemento. Los índices válidos están en `0..len(a)`. Un índice inválido produce `R_BOUNDS`. Un tamaño negativo produce `R_ARRAY_SIZE`. El máximo de implementación es 2²⁴ enteros por arreglo. Una solicitud mayor o un fallo del asignador produce `R_ALLOC`. Un arreglo de longitud cero es válido.

Una variable `var` permite `a[i] = valor`. Se evalúan índice y valor, en ese orden, antes del acceso comprobado. `let` y los parámetros son de solo lectura. Los arreglos se pueden devolver y pasar por valor, con copias independientes al modificar una de las variables. Una asignación conserva el valor anterior de su derecha incluso en una autoasignación. La implementación actual acepta `[int]` como local, parámetro y resultado. Los campos de structs, los payloads de enums y los arreglos multidimensionales quedan fuera de este corte.

El backend reserva en una región los temporales que se demuestran confinados a su bloque. Restaura la marca de la región al salir, también tras un retorno y entre iteraciones. En la ruta general un dueño único libera cada reserva. Las copias con valor independiente son profundas cuando el backend necesita conservar una vista prestada. `veloz costes ARCHIVO [FUNCIÓN]` muestra las operaciones dinámicas del IR y la razón de cada región, dueño o copia antes de la optimización de LLVM.

`readByte() -> int` consume un byte de la entrada estándar, incluidos 0 y 255. Devuelve -1 al llegar al fin de archivo o ante un error de lectura. Las pruebas ejecutadas por `check` disponen de una entrada vacía. En Windows se usa modo binario.

### Límites del núcleo

Los ciclos por valor se rechazan. La implementación limita a 96 niveles el anidamiento sintáctico, a 32 niveles un tipo nominal y a 4096 bytes su tamaño estimado conservador. El estimador añade margen para alineación. El límite de llamadas simultáneas por ejecución es 256. El intérprete dispone por defecto de diez millones de pasos por entrada, modificables con `veloz run archivo --steps N`. Un agotamiento del presupuesto del intérprete produce `R_STEPS`. Los binarios nativos ejecutan los bucles hasta su terminación o una trampa del programa. Las salidas previas a una trampa siguen siendo observables.

## Protocolo de la CLI

El compilador reserva conservadoramente hasta 16 KiB de parámetros, locales y temporales por función. La estimación cuenta dos veces el tamaño de cada valor que podría materializarse. El ejecutable Windows reserva 32 MiB de espacio virtual de pila. El límite por función es un presupuesto de esta versión, independiente del tamaño real del marco después de optimizar.

`check` comprueba sintaxis, tipos y pruebas sin invocar LLVM. La opción `--format` exige además el formato canónico. El primer diagnóstico se escribe como un objeto JSON en stderr. Los offsets `span.start` y `span.end` describen el intervalo semiabierto en **bytes UTF-8**. `line` y `column` comienzan en uno y la columna cuenta bytes. Una reparación aplicable trae un tramo y el texto de sustitución. El éxito de `check` es silencioso.

`run` ejecuta main en el intérprete. `emit-ir` escribe LLVM IR textual. `build ARCHIVO -o SALIDA` enlaza un binario a través de Clang. En la ruta nativa una trampa produce código de salida 1 y JSON con `id`, `start` y `end`. La salida del programa va a stdout.

`build ARCHIVO -o SALIDA --release --cpu x86-64-v3` reproduce la configuración Turi del ensayo local con LLVM `-O3`, LTO y lld. Omitir `--cpu` usa el objetivo ordinario del driver. `--cpu native` selecciona las instrucciones de la CPU de compilación.

`sig` resume firmas, efectos conservadores de E/S y trampas posibles. `types` lista los tipos comprobados por intervalo. Ambos devuelven JSON por línea y aceptan filtro por símbolo y `--offset N`. La paginación indica `nextOffset`. `explain` amplía un diagnóstico. `fmt` escribe el formato canónico conservando comentarios. `patch ARCHIVO FUNCIÓN HASH REEMPLAZO` sustituye la declaración identificada tras validar tipos, hash de los bytes originales y pruebas. El hash FNV-1a de 64 bits detecta ediciones simultáneas y se obtiene con `sig`.
