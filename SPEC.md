# Turi · lenguaje 0.5 (núcleo completo)

Esta especificación describe los programas aceptados en Turi 0.5 (núcleo 0.3 y ampliaciones verificadas de Turi 0.4 y Turi 0.5). La gramática para generación se distribuye en `grammar/turi.lark` y `grammar/turi.gbnf`. El expediente conserva la historia de decisiones y mediciones.

## Archivo y declaraciones

Un archivo UTF-8 contiene cero o más declaraciones de constante (`const nombre: tipo = expr`), función, struct, enum, interfaz, impl o prueba en cualquier orden, tras sus imports. Un salto de línea separa declaraciones y sentencias consecutivas. Se admiten líneas vacías, espacios, tabuladores y comentarios desde `//` hasta fin de línea. Las llaves abren y cierran bloques. Se admite una sentencia corta entre las llaves en la misma línea. La continuación de una expresión se permite inmediatamente tras un operador, una coma o un paréntesis de apertura. Los tipos y valores tienen espacios de nombres separados. Los valores usan `[a-z][A-Za-z0-9]*` y los tipos nominales empiezan por mayúscula ASCII.

```turi
fn max(a: int, b: int) -> int {
    if a > b { return a } else { return b }
}
```

Las firmas requieren tipos. Al omitir el resultado se obtiene `void`. Se permiten llamadas adelantadas y recursión. `fn main() {}` es el punto de entrada de un ejecutable. Un archivo de biblioteca o de pruebas puede carecer de él. Un `test "nombre" { ... }` se ejecuta con `veloz check` después de comprobar todo el archivo. Los nombres de prueba son únicos. Una prueba dispone de su propio presupuesto de ejecución.

## Paquetes y módulos

Un archivo sin manifiesto sigue siendo un programa monofichero. Para formar un paquete, la entrada está bajo `src/` y la raíz contiene `veloz.pkg`. Cada `src/nombre.turi` define el módulo `nombre`; los nombres de módulo son únicos en todo el grafo. El manifiesto usa líneas estrictas, sin rangos ni registro:

```text
package app
version 1.0.0
require contracts 1.1.0 "../contracts"
```

`package` y `version` son las dos primeras líneas; después pueden aparecer dependencias `require`. Las versiones son exactas `MAJOR.MINOR.PATCH`. Las rutas son relativas al manifiesto y deben existir. El cargador comprueba nombre y versión del destino y rechaza conflictos de versión o ruta en un diamante, manifiestos inválidos y ciclos de paquetes. No accede a la red.

Los `import model` preceden a todas las declaraciones. Un módulo puede importar módulos de su paquete o de dependencias directas, nunca una dependencia transitiva. No hay alias, comodines, reexportaciones ni ciclos de imports. Los símbolos de otro módulo se escriben siempre cualificados: `stats.evaluate`, `model.Item` y `model.State.Ready`. Las declaraciones propias usan su nombre local. Toda declaración es visible; no existe `pub`.

El compilador conserva cada fuente y los offsets se refieren al archivo indicado en `file`. Produce un único programa para intérprete y LLVM. Solo `main` del módulo de entrada es entrypoint; una función llamada `main` en otro módulo es ordinaria. `check` valida todo el grafo y ejecuta únicamente las pruebas del paquete raíz. `fmt` y las reparaciones impresas por `fix` operan sobre el archivo de entrada; `patch` valida el proyecto completo antes de escribir la fuente propietaria del símbolo.

## Métodos, interfaces, `dyn` y closures

```turi
interface Shape {
    fn area(self) -> int
    fn scale(mut self, k: int)
    fn tag(self) -> str { return "shape" } // método por defecto opcional
}

struct Rect { w: int, h: int }

impl Rect {
    fn square(side: int) -> Rect { return Rect { w: side, h: side } }
    fn doubled(self) -> Rect { return Rect { w: self.w * 2, h: self.h * 2 } }
}

impl Shape for Rect {
    fn area(self) -> int { return self.w * self.h }
    fn scale(mut self, k: int) { self.w *= k; self.h *= k }
}
```

Un bloque `impl Tipo { ... }` o `impl<T> Stack<T> { ... }` (restricción opcional `<T: Interfaz>`) define métodos inherentes sobre un `struct` o `enum` en el mismo módulo donde se declaró el tipo (`E_IMPL_COHERENCE`, `E_IMPL_INHERENT`). El primer parámetro de un método de instancia es `self` (solo lectura) o `mut self` (mutable por referencia; `&self`/`&mut self` produce `E_SELF_MUT` con `fix`). Sin `self`, el método es asociado y se invoca como `Tipo.metodo(...)` o `Stack<int>.new()` (`E_METHOD_STATIC` si se llama sobre un valor). Se admiten varios bloques `impl` por tipo siempre sin duplicar nombres ni sobrecargar (`E_DUP_FUNCTION`, `E_OVERLOAD`). La llamada `x.metodo(a)` resuelve primero métodos inherentes (o campos de tipo función `x.cb(a)`) y luego impls visibles de interfaz; `E_METHOD_UNKNOWN` ofrece reparación automática ante falsos amigos en colecciones/`str` (`x.len()`, `a.push(v)`, `m.get(k)`, `s.add(v)`).

Una `interface` no es genérica y sus métodos tampoco; `Self` debe aparecer en al menos un parámetro (`E_INTERFACE`). Sus métodos pueden incluir cuerpo por defecto (`{ ... }`), comprobado de antemano con `Self` opaco y heredado en cada `impl Interfaz for Tipo` o `impl<T: D> D for Caja<T>` salvo sobrescritura explícita. `T` restringido con `<T: Interfaz>` permite tanto `value.metodo()` como `metodo(value)`.

El tipo explícito `dyn Interfaz` provee despacho dinámico con semántica de valor independiente (16 bytes `{data, vtable}`, clonación y liberación automática; admite `[dyn Shape]`, `dyn Shape?`, `Result<dyn Shape, E>`, `Map<str, dyn Shape>`, structs recursivos `next: dyn Eval?` y tipos de tamaño cero). Exige seguridad de objeto (`E_DYN_SAFETY`: interfaz no genérica `E_DYN_TYPE`, primer parámetro `self`/`mut self` y sin otras apariciones de `Self` en parámetros o retorno). La conversión desde cualquier tipo concreto compatible se infiere al asignar, retornar, pasar argumento o construir agregados/opciones.

Las llamadas mutables aceptan también cadenas de campos de una raíz `var` o `mut` (`push(mut self.items, x)`, `sbPush(mut w.buf, "a")`), permitiendo argumentos disjuntos (`swap(mut p.a, mut p.b)`) y rechazando rutas solapadas o con el mismo prefijo (`E_ALIAS_MUT`).

### Funciones de primera clase y closures

```turi
fn apply<T, U>(x: T, f: fn(T) -> U) -> U { return f(x) }

fn makeAdder(base: int) -> fn(int) -> int {
    return |x| base + x
}
```

El tipo función se escribe `fn(T1, T2) -> R` (omite `-> R` cuando devuelve `void`). Una closure se escribe `|| expr`, `|x| expr`, `|x: T| -> R expr` o con bloque `{ ... }`; sus tipos de parámetro y retorno pueden omitirse cuando el contexto o el bloque los determinan (`E_INFER_TYPE` si faltan datos), nunca llevan `mut` (`E_CLOSURE_PARAM`) ni usan `lambda`, `=>` o `fn(...) { ... }` (`E_CLOSURE_SYNTAX`). Capturan automáticamente por valor inmutable (instantánea al crearse), rechazando la mutación o paso `mut` de capturas con `E_CLOSURE_CAPTURE_MUT`. Cualquier función declarada sin parámetros genéricos ni `mut` puede usarse directamente como valor `fn(...)`. Pueden almacenarse en variables, arreglos, mapas o campos de `struct` e invocarse sobre cualquier expresión callable (`E_CALL_TYPE` si no es `fn(...)`); carecen de `==`/`!=` (`E_EQUALITY`) y no son claves de `Map`/`Set` (`E_KEY_TYPE`). El compilador especializa HOFs y devirtualiza closures que no escapan en su marco local con 0 asignaciones en heap.

## Valores y operaciones

| Tipo | Semántica |
|---|---|
| `int` | Entero de 64 bits con signo (literales decimales, `0x...`, `0b...`). También hay anchos exactos con signo/sin signo: `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64` con conversión explícita `T(x)` y `bitcast<T>(x)`. Operaciones `+ - * / %`, menos unario, bits `& \| ^ ~ << >> >>>` y helpers `wrapping*` / `saturating*`. Una operación aritmética con desbordamiento produce `R_OVERFLOW`. Divisor cero produce `R_DIV_ZERO`. Tanto `min / -1` como `min % -1` producen `R_OVERFLOW`. |
| `float` | Flotante IEEE-754 de 64 bits (`float`/`f64`) y 32 bits (`f32`). Operaciones `+ - * /` y `-x`, comparaciones, `int(f)`, `float(i)`, `f32(x)`, `bitcast<T>(x)` y primitivas matemáticas (`abs`, `sqrt`, `floor`, `ceil`, `round`, `min`, `max`, `pow`, `sin`, `cos`, `isNaN`, `isInf`, etc.). |
| `bool` | `true`, `false`, `!`, `&&` y `\|\|`. La derecha de una operación lógica se evalúa solo cuando puede cambiar el resultado. |
| `str` | Texto UTF-8 inmutable (`<= 12 B` en línea, `> 12 B` en `.rodata` o heap con dueño único; máximo `2³⁰ - 1` bytes). Escapes `\n`, `\r`, `\t`, `\\`, `\"`, `\{`, `\}`, `{{`, `}}`. Las cadenas con comillas dobles interpolan `"{expr}"` (`int`, `float`, `bool`, `str` anidables; excluido en `test "nombre"`). Admite `+` / `+=`, `== != < <= > >=` en orden de bytes UTF-8 sin signo, `len(s)` (bytes), `s[i]` (byte `0..255`, `R_BOUNDS`), corte `s[lo..hi]` (`R_BOUNDS`), `str(x)` / `strFromByte(b)`, `parseInt` / `tryParseInt` / `parseFloat` / `tryParseFloat` (`R_PARSE`) y búsqueda (`strContains`, `strStartsWith`, `strEndsWith`, `strIndexOf`). |
| `void` | Retorno sin valor. Una llamada que devuelve void puede ser una sentencia. |

Todos los escalares (y enums sin carga o con valor explícito `= expr`) admiten `==` y `!=` entre valores del mismo tipo; `int(e)` obtiene el entero asociado de un enum. El orden `< <= > >=` aplica a `int`, `float` y `str`. La precedencia de menor a mayor es disyunción, conjunción, igualdad, comparaciones de orden, OR bit a bit, XOR bit a bit, AND bit a bit, desplazamientos, suma/resta, multiplicación/división/resto, unarios y proyección de campo/corte. Los operadores binarios de un mismo nivel asocian a la izquierda. La evaluación de argumentos y operandos avanza de izquierda a derecha. `print(valor)` escribe un escalar (`int`, `float`, `bool`, `str`) y el byte LF. `assert condición` produce `R_ASSERT` cuando es falsa. `StringBuilder` construye texto mutable con `sbNew()`, `sbWithCap(n)` (`R_ARRAY_SIZE` si `n < 0`), `sbPush(mut sb, s)`, `sbPushByte(mut sb, b)`, `sbPushInt(mut sb, n)`, `sbPushFloat(mut sb, x)`, `sbClear(mut sb)`, `sbStr(sb)`, `sbTake(mut sb)` (mueve al `str` y vacía `sb`), `len(sb)` y `cap(sb)`; admite structs/enums/`[StringBuilder]` y rechaza `==` (`E_EQUALITY`).

## Ámbitos y control

`let` crea un nombre inmutable. `var` permite reasignarlo, conservando su tipo. Ambos requieren un inicializador y aceptan anotación local. Las asignaciones compuestas `+= -= *= /= %= &= \|= ^= <<= >>= >>>=` aplican las mismas comprobaciones que la operación ordinaria. En variables `var` y parámetros `mut`, también se admite mutación de campos (`s.f = v`, `s.f += v`, `s.items[i] = v`). Los parámetros son de solo lectura por defecto; `fn f(x: mut T)` y `f(mut arg)` pasan por referencia mutable sin copiar; `fn next(var n: int)` recibe copia local modificable de escalar. El índice de un bucle siempre es inmutable.

`if condición { ... } else { ... }` y `while condición { ... }` exigen booleanos. `} else if` se escribe en la misma línea. `for i in a..b` evalúa los límites enteros una vez y recorre el intervalo ascendente, excluido `b`. `for x in arreglo` recorre una instantánea de los elementos de `[T]`. Tanto `while` como `for` admiten etiquetas `'nombre:` y sentencias `break` / `continue` (con etiqueta opcional `'nombre`). `return` sale de toda la función. Un resultado con valor requiere retorno explícito en todos los caminos. El comprobador verifica también código inalcanzable.

## Productos, sumas y genéricos

Un struct tiene campos tipados (incluidos `[T]`, permitiendo recursión indirecta como `kids: [Node]`). Un constructor `Point { x: 1, y: 2 }` especifica exactamente una vez cada campo. Los argumentos se evalúan en el orden escrito, aunque difiera del declarado. `p.x` lee un campo; si la variable o parámetro `mut` es modificable, `p.x = v` muta el campo conservando semántica de valor en copias. La semántica de valor conserva la copia anterior al reasignar otra variable.

Un enum declara variantes vacías, con valor entero explícito (`enum TokenKind { Ident = 1, ... }`) o con parámetros nombrados (no combinables en el mismo enum). Se construye con `Search.Missing` o `Search.Found(42)`. En un `match` el discriminante se evalúa una vez. Cada brazo lleva un nombre calificado y cero o más nombres locales ligados por posición. Los brazos deben cubrir exactamente las variantes del tipo. Tras `=>` se admite una sentencia o un bloque. Los agregados con campos carecen de `==` e impresión implícita.

Una función `fn id<T>(x: T) -> T { return x }` acepta argumentos de tipo inferidos a partir de sus parámetros. Cada parámetro de tipo necesita al menos una aparición en los parámetros de valor. Los `struct` y `enum` admiten también parámetros de tipo (`struct Stack<T> { ... }`, `enum Tree<T> { Empty, Node(val: T, kids: [Tree<T>]) }`) con cierre anidado `>>` / `>>>` y especialización monomórfica por combinación concreta usada (los ciclos directos por valor siguen rechazándose tras instanciar con `E_TYPE_CYCLE`). El compilador comprueba las plantillas de función contra tipos opacos incluso sin llamadas.

Para valores ausentes y errores explícitos existen los tipos genéricos reservados `Option<T>` (abreviado `T?`, variantes sin prefijo `Some(v)` y `None`) y `Result<T, E>` (variantes sin prefijo `Ok(v)` y `Err(e)`), sin `null`/`nil`/`undefined` (`E_NULL`) ni `try {`/`throw` (`E_EXCEPTIONS`). La inferencia contextual deduce el tipo faltante en `None`, `Ok(v)` y `Err(e)` desde asignaciones, retornos, argumentos o constructores (`E_INFER_TYPE` si falta contexto). El operador postfijo `expr?` extrae `Some(v)`/`Ok(v)` y propaga anticipadamente `None` en funciones `-> U?` o `Err(e)` en funciones `-> Result<U, E>` (`E_TRY_TYPE`, `E_TRY_RETURN`, `E_TRY_ERR`); dentro de un bloque `test`, `expr?` falla con `R_ASSERT` ante `None` o `Err`. Las primitivas sombreables `isSome`, `isNone`, `isOk`, `isErr` y `unwrapOr(v, respaldo)` consultan o resuelven alternativas evaluando izquierda a derecha.

## Comprobaciones y recursos

### Arreglos genéricos `[T]`, crecimiento, cortes y E/S de sistema

Los arreglos `[T]` admiten cualquier tipo de elemento (`[int]`, `[float]`, `[str]`, structs, enums o matrices `[[int]]`). Pueden construirse con literales `[a, b, c]` (vacío tipado `let a: [int] = []`), `repeat(elemento, cantidad)` o cortes `a[lo..hi]`. Para crecer y mutar arreglos `var` o `mut`, se dispone de `push(mut a, x)`, `pop(mut a)`, `clear(mut a)`, `truncate(mut a, n)`, `reserve(mut a, minCap)`, `len(a)` y `cap(a)` (sombreables por funciones de usuario). `truncate` conserva los primeros `min(max(n, 0), len(a))` elementos y libera el resto en orden inverso, manteniendo la capacidad reservada. Los índices válidos están en `0..len(a)` y los cortes en `0 <= lo <= hi <= len(a)`. Un índice, `pop` vacío o corte inválido produce `R_BOUNDS`. Un tamaño negativo de reserva o construcción produce `R_ARRAY_SIZE`. El máximo de implementación es 2³⁰ elementos por arreglo. Una solicitud mayor o un fallo del asignador produce `R_ALLOC`. Un arreglo de longitud cero es válido.

Una variable `var` o parámetro `mut` permite `a[i] = valor` y `s.items[i] = valor`. Los arreglos se pueden devolver, almacenar en structs/enums y pasar por valor, con copias independientes profundas cuando un dueño muta su contenido. El backend reserva en una región los `[int]` temporales confinados a su bloque y usa dueños únicos + destructores tipados automáticos para arreglos, structs y enums con recursos anidados. `veloz costes ARCHIVO [FUNCIÓN]` muestra las operaciones dinámicas del IR antes de LLVM.

Además de `readByte() -> int`, la CLI y el núcleo proveen `writeByte`, `write`, `ewrite`, `eprint`, `flush`, `readFile`, `writeFile`, `appendFile`, `fileExists`, `removeFile`, `makeDir`, `argsCount`, `argAt`, `envGet`, `clockNs`, `sleepMs`, `execCapture` y `execStatus`.

### Colecciones Hash (`Map<K, V>`, `Set<T>`) y ordenación (`sort`, `sortStable`)

`Map<K, V>` y `Set<T>` son tipos genéricos reservados implementados como tablas hash SwissTable contiguas (factor de carga máximo `7/8`, búsqueda por grupo de 16 bytes con SIMD SSE2/NEON o respaldo portátil y sondeo triangular), permitiendo recursión indirecta en structs/enums (por ejemplo `struct Trie { end: bool, kids: Map<str, Trie> }`) y semántica de valor con copia profunda al mutar. Solo admiten claves hashables: `int`, `str`, `bool` y enums sin campos (`E_KEY_TYPE`). Construir `mapNew()`, `mapWithCap(n)`, `mapSecure()`, `setNew()`, `setWithCap(n)` o `setSecure()` deduce el tipo desde asignación, campo, argumento o retorno (`E_INFER_TYPE` si falta contexto). Por defecto usan hash rápido `foldhash`; `mapSecure()` y `setSecure()` usan **SipHash-1-3** con semilla aleatoria CSPRNG del sistema operativo.

Las operaciones de mapa son `mapSet(mut m, clave, valor)`, `mapGet(m, clave) -> V?`, `mapHas(m, clave) -> bool`, `mapDel(mut m, clave) -> bool`, `mapKeys(m) -> [K]` y `mapValues(m) -> [V]`; las de conjunto son `setAdd(mut s, elem) -> bool`, `setHas(s, elem) -> bool`, `setDel(mut s, elem) -> bool` y `setItems(s) -> [T]`. `len(c)`, `cap(c)`, `clear(mut c)` (conserva capacidad y semilla) y `reserve(mut c, minCap)` operan de forma uniforme sobre `[T]`, `Map<K, V>` y `Set<T>`. Todos sus nombres pueden sombrearse por funciones de usuario. Se diagnostican con reparación `HashMap`/`Dict` (`E_MAP_NAME`), `HashSet` (`E_SET_NAME`), la indexación `m[k]` / `m[k] = v` (`E_MAP_INDEX`), `.length` (`E_LENGTH`) y `==`/`!=` (`E_EQUALITY`).

Para ordenar arreglos mutables in situ se proveen `sort(mut a)` (inestable `ipnsort` sin asignaciones en heap) y `sortStable(mut a)` (estable adaptativa `driftsort`) sobre `[int]`, `[float]` (orden total IEEE-754 con `-NaN < -Inf < -0.0 < +0.0 < +Inf < +NaN`), `[str]` (bytes UTF-8 sin signo) y `[bool]`.

### Memoria automática de niveles 2–5: `Pool<T>`/`Handle<T>`, `Shared<T>` y `costes` (Hito 2.6A)

El compilador asigna el nivel mínimo necesario sin intervención manual y lo expone con `veloz costes`: regiones acotadas al bloque (nivel 2), dueños únicos con movimiento por último uso en variables, campos y colecciones respetando préstamos vivos (nivel 3), `Pool<T>` con asas generacionales `Handle<T>` para grafos y estructuras cíclicas (nivel 4) y `Shared<T>` acíclico con conteo de referencias Perceus y reutilización guiada por caída (`sharedSet`/`sharedUpdate`, nivel 5). Tanto `check`, `build` como `costes` aceptan `--max-level N` para rechazar niveles superiores (`E_MEMORY_LEVEL`).

`Pool<T>` y `Handle<T>` ofrecen `poolNew()`, `poolWithCap(n)`, `poolAdd(mut p, v) -> Handle<T>`, `poolGet(p, h) -> T?`, `poolSet(mut p, h, v) -> bool`, `poolHas(p, h) -> bool`, `poolRemove(mut p, h) -> T?`, `poolHandles(p) -> [Handle<T>]`, además de `len`/`cap`/`clear`/`reserve` (conservando generaciones para invalidar asas antiguas). Cuando ningún camino llama a `poolRemove` o `clear`, el backend elimina las comprobaciones generacionales (`poolGet`/`poolHas` monótonos). `Handle<T>` admite `==`/`!=` y clave de `Map`/`Set`. `Shared<T>` provee `sharedNew(v)`, `sharedGet(s) -> T`, `sharedSet(mut s, v)` y `sharedUpdate(mut s, f)`. Se diagnostican `Arena`/`SlotMap`/`Slab` (`E_POOL_NAME`), `p[h]` (`E_POOL_INDEX`) y `Rc`/`Arc`/`SharedPtr` (`E_RC_NAME`).

### Recursos RAII deterministas y movimiento afín (Hito 2.6B)

`File`, `TcpListener`, `Socket`, `Mutex`, `Lock`, `Scope` y `Task<T>` (y cualquier `struct`, `enum`, `[R]`, `R?` o `Result<R, E>` que los contenga) son tipos afines no copiables (nivel 3 `"resource"`): tienen un único dueño y se cierran o desbloquean de forma determinista en **orden inverso de declaración** (y orden inverso en campos de agregado y arreglos) al salir del ámbito, incluso mediante `return`, `break`, `continue`, `'etiqueta`, `?`, sobrescritura o `clear(mut arr)`. Leer un nombre de recurso como rvalue en `let`/`var`, reasignación, `return`, literal de struct/enum/array, `push`, `match` o `?` transfiere su propiedad; el comprobador afín sensible al flujo rechaza usar tras mover (`E_USE_AFTER_MOVE`, reasignar un `var` restaurándolo lo vuelve válido) y prohíbe copiar, mover parámetros prestados `f(x)`, extraer parcialmente campos `s.f`, indexar `a[i]` (usa `pop(mut a)`), cortar `a[lo..hi]`, `repeat(r, n)`, `for x in arr`, `Map`/`Pool`/`Shared`/`Frozen`/`dyn` o capturar recursos en closures (`E_RESOURCE_COPY`), así como falsos amigos (`FileStream`/`BufReader`/`TcpStream`/`MutexGuard`, `E_RESOURCE_NAME`). Nunca usan regiones de nivel 2.

Operaciones integradas: `fileOpen(ruta, modo) -> Result<File, str>` (`"r"`, `"w"`, `"a"`, `"r+"`, `"w+"`, `"a+"`), `fileRead(mut f, maxBytes) -> Result<str, str>` (`R_ARRAY_SIZE`/`R_ALLOC`), `fileReadAll(mut f) -> Result<str, str>`, `fileReadLine(mut f) -> Result<str?, str>`, `fileWrite(mut f, texto) -> Result<int, str>`, `fileFlush(mut f) -> Result<bool, str>`, `fileClose(mut f)`, `fileIsOpen(f) -> bool`; `tcpListen(host, puerto) -> Result<TcpListener, str>`, `tcpLocalPort(l) -> int`, `tcpAccept(mut l) -> Result<Socket, str>`, `tcpConnect(host, puerto) -> Result<Socket, str>`, `socketRead(mut s, maxBytes) -> Result<str, str>`, `socketWrite(mut s, texto) -> Result<int, str>`, `socketClose(mut s)`, `socketIsOpen(s) -> bool`, `listenerClose(mut l)`, `listenerIsOpen(l) -> bool`; `mutexNew() -> Mutex`, `mutexLock(mut m) -> Lock`, `mutexTryLock(mut m) -> Lock?`, `unlock(mut g)` y `lockIsHeld(g) -> bool`.

### Efectos inferidos y comprobados (Hito 2.7A)

Las declaraciones de función de nivel superior, en `interface` y en `impl` admiten modificadores contextuales opcionales antes de `fn`: `pure`, `noalloc`, `checkfree` e `io` (`pure` e `io` son excluyentes; no pueden repetirse, `E_EFFECT`). No son palabras reservadas y conservan compatibilidad con identificadores homónimos. Cada método en `impl Interfaz for Tipo` debe declarar exactamente los mismos modificadores de efecto que su método en la interfaz (`E_IMPL_SIGNATURE`).

El analizador de efectos interprocedural trabaja sobre el IR optimizado tras el pase de rangos (`optimizeIR` y `safeDepths`), propagando E/S (`print`, `readByte`, archivos, red, proceso, reloj, semilla segura, closures y tareas), asignaciones en heap y comprobaciones vivas (`R_OVERFLOW`, `R_DIV_ZERO`, `R_BOUNDS`, `R_ALLOC`, `R_ASSERT`, `R_DEPTH`, llamadas indirectas). Si una función exige `pure`, `noalloc` o `checkfree` y su cuerpo o grafo de llamadas viola la garantía, `veloz check`, `build` y `patch` rechazan con `E_EFFECT` señalando la operación responsable y el tramo de origen.

### Concurrencia estructurada, robo de trabajo y congelación (Hito 2.7B)

Turi provee concurrencia estructurada sin «funciones de color» ni carreras de datos mediante `Scope`, `Task<T>` y `Frozen<T>`:

- `taskScope() -> Scope` crea un ámbito estructurado; al salir del bloque o función (`Drop`), `Scope` espera automáticamente a todas las tareas lanzadas en él aún no unidas y libera los resultados no consumidos en orden LIFO.
- `taskSpawn(mut s, || ...)` lanza una tarea que devuelve `T` (incluido `void`) en un pool de trabajo compartido con robo de trabajo (*work stealing*) entre hilos de trabajo (hasta 16 en nativo, con ayuda cooperativa dentro de `taskJoin`/`Scope` drop para evitar bloqueos en recursión paralela). `taskJoin(t) -> T` espera y consume una sola vez el `Task<T>` (`E_USE_AFTER_MOVE` si se repite). Un `Task<T>` devuelto fuera de su `Scope` mantiene viva la celda hasta su `taskJoin` sin UAF.
- `taskSpawnMove(mut s, val, |v| ...)` mueve la propiedad de un valor o recurso (`File`, `Mutex`, `TcpListener`, `[T]`, structs) hacia la tarea, donde el parámetro movido es mutable y se cierra automáticamente si la tarea no lo devuelve.
- `parallelMap(n, |i| ...) -> [T]` reparte `0..n` en paralelo entre trabajadores (`R_ARRAY_SIZE` si `n < 0`).
- `freeze(val) -> Frozen<T>` (nivel 5 `"frozen"`) crea una cápsula inmutable compartible entre hilos con conteo de referencias atómico (prohíbe recursos afines, `E_RESOURCE_COPY`); `frozenGet(fz) -> T` devuelve copia propia y `frozenRead(fz, |x| ...)` presta el dato sin copiarlo.

El comprobador verifica enviabilidad entre hilos (`Sendable`): rechaza capturar o devolver `Shared<T>`, `Lock`, `Scope`, `Task<T>`, `Region`, `dyn` o `fn(...)` entre hilos con `E_SEND`, y diagnostica falsos amigos (`async fn`, `await`, `Thread`, `JoinHandle`, `Future`, `Promise`, `Channel`, `RwLock`) con `E_CONC_NAME`.

### Niveles de seguridad, punteros, FFI C y disposición física (Turi 0.4)

- **Disposición física de memoria:** `struct` y `enum` admiten atributos `@repr(C)` (orden lógico de declaración), `@packed` (sin relleno interno) y `@align(N)` (alineación mínima potencia de 2). `sizeOf<T>()` y `alignOf<T>()` devuelven tamaño y alineación reales en bytes; `bitcast<T>(x)` reinterpreta patrones de bits del mismo ancho entre escalares/punteros (`E_TYPE`). Por defecto, los structs y enums sin `@repr(C)` aplican reordenamiento compacto automático en LLVM.
- **Niveles configurables (`verified`, `trusted`, `raw`):** Las funciones (y bloques `trusted { ... }` / `raw { ... }`) declaran su nivel antes de los efectos (por defecto `verified`). En `verified` toda comprobación de memoria y punteros se verifica estáticamente o conserva guarda; `trusted { ... }` habilita primitivas de bajo nivel (`ptrRead`, `ptrWrite`, `ptrAdd`, `ptrCast`, `ptrAlloc`, `ptrAllocAligned`, `ptrFree`, `ptrCopy`, `ptrZero`, `strFromCStr`, `strFromPtr`, `extern "C"`, etc.) manteniendo guardas de puntero nulo y alineación; `raw` omite las guardas de puntero, carril/alineación SIMD y contratos `requires`/`ensures` en ejecución manteniendo su semántica de asunción estática.
- **Punteros explícitos y FFI C:** `Ptr<T>` (lectura) y `MutPtr<T>` (lectura/escritura) se obtienen con `ptrOf(x)`, `mutPtrOf(mut x)`, `ptrNull<T>()`, `slicePtr(a)` / `mutSlicePtr(mut a)` o reserva explícita (`ptrAlloc`/`ptrAllocAligned`/`ptrFree`), rechazando aritmética/desreferencia en `verified` (`E_SAFETY`). `@link("lib")` y bloques `extern "C" { fn ... }` enlazan bibliotecas nativas, mientras `extern "C" fn cb(...)` + `fnPtr(cb)` exponen punteros de función ABI C sin captura.

### SIMD explícito, intrínsecas de hardware, atómicos y regiones (Turi 0.4)

- **SIMD e intrínsecas (`Simd<T, N>`):** Vectores portables sobre tipos escalares numéricos (`i8..u64`, `f32`, `float`), `N` en `{2, 4, 8, 16, 32}` y ancho total entre `8` y `64` bytes (`simdSplat`, `simdLoad`, `simdLoadAligned` con `R_ALIGN`, `simdStore`, `simdStoreAligned`, `simdFromArray`, `simdToArray`, `simdGet`, `simdSet`, aritmética/FMA/bits por carril, máscaras `simdEq`/`simdLt`/`simdSelect`, `simdReduceAdd`/`simdSum`). Además se proveen intrínsecas de bits y hardware (`clz`, `ctz`, `popcnt`, `bswap`, `rotl`, `rotr`, `prefetchRead`, `prefetchWrite`, `spinHint`).
- **Primitivas concurrentes lock-free y `Region`:** `Atomic<T>` (`bool` o enteros `i8..u64`) ofrece celdas atómicas compartidas entre hilos (`Send`) sin aliasing (`atomicNew`, `atomicLoad`, `atomicStore`, `atomicSwap`, `atomicCas`, `atomicAdd`, `atomicSub`, `atomicAnd`, `atomicOr`, `atomicXor` y variantes `*Ord` con ordenamiento `"relaxed"`, `"acquire"`, `"release"`, `"acqRel"`, `"seqCst"`, más `atomicFence(ord)`). `RingQueue<T>` implementa una cola circular acotada MPMC lock-free de Vyukov enviables entre hilos (`ringNew<T>(cap)`, `ringPush`, `ringPop`, `ringLen`, `ringCap`). `Region` expone asignadores bump-pointer RAII no copiables con liberación en bloque (`regionNew`, `regionAlloc<T>`, `regionAllocAligned<T>`, `regionReset`, `regionUsed`, `regionCapacity`).

### Contratos verificables (`requires`/`ensures`) y explicación de guardas

- **Precondiciones y postcondiciones:** Cualquier función o método puede declarar cláusulas `requires expr` y `ensures expr` tras su tipo de retorno (antes de `{`). Evalúan a `bool` en modo lectura pura sin mutar variables, mover recursos ni usar `?` (`E_CONTRACT`); dentro de `ensures`, `result` enlaza el valor devuelto cuando la función no es `void`. En modos `verified` y `trusted`, una violación en ejecución produce `R_CONTRACT`; en `raw`, actúan como asunciones estáticas sin guarda en tiempo de ejecución.
- **Análisis estático y explicabilidad (`veloz costes --explain-checks`):** El analizador de rangos y grafo de diferencias (`FunctionProver`) inyecta las precondiciones `requires` al inicio de la función (eliminando automáticamente guardas `R_BOUNDS` en `[T]`, `R_DIV_ZERO` en `/` o `%`, y `R_OVERFLOW` en aritmética acotada), demuestra `ensures` estáticamente cuando los caminos de retorno garantizan la postcondición e informa cada comprobación eliminada o conservada y su motivo mediante `veloz costes ARCHIVO [FUNCIÓN] --explain-checks` (`status: "eliminated" | "kept"`, `check`, `reason`).

### Ampliaciones ergonómicas y optimización Turi 0.5

- **`if` como expresión, multilínea y comas finales:** `if cond { ... } else if cond2 { ... } else { ... }` puede devolver un valor de cualquier tipo unificado en todas sus ramas (incluyendo evaluación en `const`). Se admiten listas multilínea y coma final opcional en parámetros, argumentos, campos de `struct` y literales `[T]`.
- **Métodos UFCS y ergonomía estándar:** Cualquier función o primitiva estándar (`[T]`, `str`, `StringBuilder`, `Map<K, V>`, `Set<T>`, `Pool<T>`, `Option`, `Result` y escalares numéricos `int`/`float`) puede invocarse con sintaxis de método (`x.len()`, `a.push(v)`, `s.contains(sub)`, `opt.unwrapOr(d)`, `(-7).abs()`). Se añaden `tryReadFile` / `tryWriteFile` (`Result`), literales binarios `0b...`, desplazo lógico sin signo `>>>` / `>>>=`, cierre automático de `>>` en genéricos anidados y anotaciones de memoria `@noCopy`, `@arena` y `@compact`.
- **Optimizaciones universales de compilador (Fase 5):** Rangos idempotentes en DP por filas, promoción de anchos estrechos `i16` hacia aritmética `>= 32` bits, conversión a flujo sin ramas sobre `IndexSet` y escalares vectorizables, supresión de rellenos muertos en arreglos sobrescritos completamente, reducción afín de módulo constante y desenrollado periódico en IR propio, y elisión de marcas de arena en stack cuando no hay bucles.

### Front-end tolerante, mutabilidad inferida y sintaxis nueva (Turi 0.9)

La regla de diseño es que **el compilador acepta lo que solo tiene una lectura posible** en vez de devolver un error que cuesta otra ronda de reparación. `veloz fmt` y `veloz fix` siguen escribiendo la forma canónica, y `--strict` (o `VELOZ_STRICT=1`) conserva los diagnósticos antiguos para quien quiera imponer el estilo.

- **Grafías toleradas (se convierten a la forma canónica):** `;` al final de una sentencia, `elif`, `let mut x`, `x++`/`x--` como sentencia, `println(...)`/`println!(...)`/`print!(...)`, `and`/`or`/`not`, `True`/`False`, `null`/`nil` (por `None`) y nombres en `snake_case`. En modo estricto siguen dando `E_SEMICOLON`, `E_ELIF`, `E_LET_MUT`, `E_PRINTLN`, `E_NAME`, `E_NULL` o `E_PARSE`.
- **Mutabilidad inferida:** si el programa modifica un `let`, esa declaración pasa a ser `var`. Si modifica un parámetro por valor, el parámetro pasa a ser una copia local mutable (un escalar equivale a `var n: int`; un arreglo, `str` o `struct` se copia al entrar, así que quien llama nunca ve el cambio). Un método que modifica `self` equivale a `mut self`. El índice de un `for` y las capturas de una closure siguen siendo de solo lectura (`E_IMMUTABLE`, `E_CLOSURE_CAPTURE_MUT`).
- **Sintaxis oficial nueva (también en modo estricto):**
  - Rango inclusivo `a..=b` en `for` y en cortes `x[a..=b]`.
  - Ternario `c ? a : b` (equivale a `if c { a } else { b }`); `x?` sigue siendo la propagación de `Option`/`Result`.
  - Retorno implícito: si una función con resultado termina en una expresión suelta (incluidos `if`/`else` y `match` con valor en todas las ramas), esa expresión es el resultado.
  - Literales de carácter `'a'`, `'\n'`, `'\\'`, `'\''`: son el `int` de su código ASCII, comparables con `s[i]`. Donde se espera un `str` (comparación o concatenación con un `str`, argumento o variable de tipo `str`) valen como el `str` de un carácter, así que `c == 'a'` funciona igual con un byte que con un carácter de `s.chars()`.
  - `for c in s` sobre un `str` recorre sus caracteres; cada `c` es un `str` de un carácter.
  - `str + escalar` y `escalar + "literal"` convierten el escalar con `str()`.
  - `a[i] op= v` con índice sin efectos equivale exactamente a `a[i] = a[i] op v`.
- **Mapas con corchetes:** `m[k] = v` escribe; `m[k]` lee el valor o el valor cero de `V` si la clave no está (como en Go; `m.get(k)` sigue devolviendo `V?`); `m[k] += d` (también `-=` y `*=`) parte de cero si la clave no está y hace una sola búsqueda en la tabla. Vale para valores escalares y `str`.
- **UFCS general:** `x.f(a, b)` es `f(x, a, b)` cuando existe una función libre `f`, propia o de la biblioteca base. Si su primer parámetro es `mut`, el receptor se pasa por referencia.
- **Biblioteca base** (`src/prelude.hpp`, escrita en Turi). Solo se incorporan las funciones que el programa nombra; una función propia con el mismo nombre la sustituye.
  - Cadenas: `split(sep)`, `words()`, `join(sep)` sobre `[str]`, `trim()`, `upper()`, `lower()`, `replace(a, b)`, `repeat(n)`, `reverse()`, `chars()`, `bytes()`, `count(t)`; `isDigit(c)`, `isAlpha(c)`, `isSpace(c)` sobre el código de un carácter.
  - Arreglos: `map(f)`, `filter(f)`, `any(f)`, `all(f)`, `count(f)`, `sum()`, `max()`, `min()`, `indexOf(x)`, `contains(x)`, `reverse()` (arreglo nuevo), `concat(b)`.
  - Se aceptan los nombres habituales de otros lenguajes: `length`/`size`, `append`, `includes`, `toUpperCase`/`toLowerCase`, `strip`.
- **Tuplas:** el tipo `(A, B)` (de 2 a 4 elementos), el literal `(a, b)`, la desestructuración `let (x, y) = t` y el acceso `t.0` o `t[0]`. Son `struct` normales de la biblioteca base (`Tuple2`…`Tuple4`): se copian, se comparan y se devuelven como cualquier valor.
- **Tipos recursivos directos:** un `struct` o `enum` puede contenerse a sí mismo: `struct Node { left: Node?, right: Node? }`, `struct Item { value: int, next: Item? }`, `enum Expr { Num(v: int), Add(l: Expr, r: Expr) }`. No se escribe `Pool`, `Handle`, `Shared` ni ninguna caja: el compilador guarda en una celda del montón el campo que cierra el ciclo y mete y saca el valor en cada uso. La semántica sigue siendo de valor (asignar un árbol no deja dos nombres apuntando a lo mismo de forma observable). Reglas que el programador no ve pero que el compilador garantiza:
  - Leer un campo, pasar un subárbol a una función o hacer `match` sobre un valor que no cambia **no copia** ni cuenta referencias; el valor se toma prestado.
  - Un árbol o una lista que se crea y muere dentro de una vuelta de bucle se reserva en una **región**: cada nodo cuesta un incremento de puntero y la vuelta lo libera todo de golpe, sin recorrerlo. El compilador solo abre la región cuando demuestra que nada creado dentro sigue vivo fuera; si no, usa celdas normales.
  - Liberar una lista de millones de nodos no usa pila en proporción a su longitud.
  - Un tipo que no se puede construir nunca (`struct Loop { next: Loop }`) sigue siendo `E_TYPE_CYCLE`. En modo estricto lo es cualquier ciclo directo.
  - Límites actuales: no se modifica en sitio a través de un campo recursivo (`t.left.key = 3` no existe); se devuelve el nodo reconstruido (`Tree { key: n.key, left: Some(insert(n.left, k)), right: n.right }`) y el compilador reutiliza la celda si nadie más la usa. La conversión automática entre `T?` y el campo recursivo `T?` existe para los tipos no genéricos; en los genéricos (y en los que lleven prefijo de módulo en su nombre) se usa `match`. Los grafos con ciclos reales siguen usando `Pool`/`Handle`.
- **Coma flotante:** `a * b + c` puede fusionarse en una FMA (un solo redondeo), como en C con su `-ffp-contract=on` por defecto y en Go. La acumulación de un escalar flotante local `x += e`, `x -= e` o `x *= e` puede agruparse en cualquier orden (el compilador la vectoriza); el resultado es determinista para un mismo binario. Cualquier otra expresión conserva el orden de evaluación IEEE-754 (una suma compensada tipo Kahan no se altera).

### Límites del núcleo

Un tipo que se contiene a sí mismo se guarda en celdas (ver «Tipos recursivos directos»); solo se rechaza con `E_TYPE_CYCLE` el que no tiene forma de construirse, y en modo estricto cualquier ciclo directo por valor (siguen permitidos los indirectos vía `[T]`, `Map<K, V>`, `Set<T>` o `dyn Interfaz`). La recursión de una función sobre una estructura sigue sujeta al tope de 256 llamadas: una lista larga se recorre con un bucle. La implementación limita a 96 niveles el anidamiento sintáctico, a 32 niveles la recursión de layout nominal directo, a 4096 bytes su tamaño estimado, a `maxTableCap = 14 680 064` entradas el tope de tablas hash (`R_ALLOC`) y a `2³⁰ - 1` bytes (`maxStringLength = 1 GiB - 1`) la longitud máxima de `str` y `StringBuilder` (`R_ALLOC`). El límite de llamadas simultáneas por ejecución (incluyendo `dyn` y closures) es 256 (`R_DEPTH`). El presupuesto de marco local `E_FRAME` (16 KiB) ya no rechaza funciones de usuario; el ejecutable Windows reserva 32 MiB de espacio virtual de pila. El intérprete dispone por defecto de diez millones de pasos por entrada, modificables con `veloz run archivo --steps N` (`R_STEPS`). `parseInt`/`parseFloat` fallan con `R_PARSE`; los prefijos `f"..."` y las comillas invertidas `` `...` `` se diagnostican con `E_FSTRING` y `E_CHAR`.

## Protocolo de la CLI

El ejecutable Windows reserva 32 MiB de espacio virtual de pila y compila funciones de usuario sin presupuesto artificial de 16 KiB por marco.

`check` comprueba sintaxis, tipos, nivel de memoria opcional (`--max-level N`, `E_MEMORY_LEVEL`) y pruebas sin invocar LLVM. La opción `--format` exige además el formato canónico. `veloz costes ARCHIVO [FUNCIÓN] [--max-level N] [--explain-checks] [--offset N]` reporta decisiones de memoria (niveles 2–5) o, con `--explain-checks`, explica cada comprobación dinámica eliminada o conservada (`bounds`, `overflow`, `div_zero`, `contract`, `assert`, `null_ptr`, `align`, `depth`). El primer diagnóstico se escribe como un objeto JSON en stderr. Los offsets `span.start` y `span.end` describen el intervalo semiabierto en **bytes UTF-8**. `line` y `column` comienzan en uno y la columna cuenta bytes. Una reparación aplicable trae un tramo y el texto de sustitución (`veloz fix ARCHIVO` aplica reparaciones sintácticas seguras a stdout). El éxito de `check` es silencioso.

`run` ejecuta main en el intérprete. `emit-ir` escribe LLVM IR textual; `emit-ir ARCHIVO --audit-copies` añade contadores externos de copias e inicializaciones sin cambiar el IR ordinario. `build ARCHIVO -o SALIDA [--max-level N]` enlaza un binario a través de Clang. En la ruta nativa una trampa produce código de salida 1 y JSON con `id`, `file`, `start` y `end`. La salida del programa va a stdout.

`build ARCHIVO -o SALIDA --release --cpu x86-64-v3` reproduce la configuración Turi del ensayo local con LLVM `-O3`, LTO y lld. Omitir `--cpu` usa el objetivo ordinario del driver. `--cpu native` selecciona las instrucciones de la CPU de compilación.

`sig` resume todos los símbolos del programa (`const`, `struct`, `enum`, `interface`, métodos `Tipo.m` y `Tipo.Interfaz.m`, y funciones) con `"kind"`, `"hash"` FNV-1a de 64 bits y, para funciones y métodos, `"effects"` (`["pure"|"io", "noalloc"?, "checkfree"?]`), `"io"` y `"mayTrap"`. `veloz slice ARCHIVO SÍMBOLO [--offset N] [--max-tokens N]` extrae la rebanada mínima autocontenida alrededor de un símbolo. `types` lista los tipos comprobados por intervalo y `costes` lista las decisiones de memoria. Los cuatro devuelven JSON por línea, aceptan filtro o símbolo, `--offset N` y `--max-tokens N` (tope predeterminado ≈ 7168 B); cuando truncan emiten un pie con `nextOffset`, `remaining` e `hint`. `explain` amplía un diagnóstico. `fmt` escribe el formato canónico conservando comentarios. `patch ARCHIVO SÍMBOLO HASH REEMPLAZO` y `patch-body` sustituyen funciones, métodos (`Tipo.m` o `Tipo.I.m`), `const`, `struct`, `enum` o `interface` tras validar hash de los bytes originales, tipos, efectos requeridos (`checkEffects`) y pruebas.
