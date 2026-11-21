# PARTE B — Expediente técnico (20/11/2026)

Cada cifra viene de la fuente citada en B8 y es punto de partida, no verdad. Las marcadas «propia» son mediciones orientativas sobre un solo programa.

## B1. Lo que manda en el diseño

1. **El riesgo mayor es que nadie ha visto Veloz.** En lenguajes no vistos los modelos aciertan entre 0 y 33 % sin ayuda. Con un bucle de compilador se recuperan: un modelo pasó de 22/56 a 54/56 en Idris. El producto es ese bucle, no la sintaxis.
2. **A las IA no les falla Rust por la memoria.** Sus errores de compilación son 44 % nombres y rutas y 33 % tipos y traits; los de préstamos son minoría y los más fáciles de reparar. De ahí: nombres planos y sistema de tipos pequeño.
3. **Los tokens totales dependen más de las reparaciones que de la sintaxis.** Entre lenguajes, un agente gasta de 0,85× a 1,69× lo que en Python; la sintaxis mueve entre 4 y 13 %.
4. **La ventaja de Goose sale de la disposición de datos** (datos inline, enlaces de 32 bits), no de liberar en bloque. Flojea en servicios de larga vida, estructuras enlazadas muy mutadas, funciones guardadas como dato, núcleos numéricos y recursión con datos que crecen. Ahí hay que ganarle.
5. **Reordenar datos automáticamente es la palanca que C, C++ y Rust no tienen:** hasta +20 % en un benchmark de punteros, +9 % solo por comprimir punteros a 32 bits.
6. **Comprobar límites cuesta poco** (0,3–1 %). Comprobar desbordamiento cuesta más (3,5 % en Rust) salvo que se inserte tarde.
7. **Apuesta sin demostrar:** ningún modelo de memoria sin anotaciones ha igualado a Rust en una suite amplia e independiente. Por eso se mide en el hito 4 y no al final.

## B2. Superficie del lenguaje

| Tema | Decisión | Motivo | Se cambia si |
|---|---|---|---|
| Bloques | Llaves; el formateador es dueño del espacio en blanco | El sangrado ahorra 9–12 % de tokens (propia), pero impide la generación restringida por gramática y rompe las ediciones de coincidencia exacta | Una variante con sangrado da ≥ 5 % menos tokens totales hasta verde y como mucho +1 punto de fallos |
| Sentencias | Una por línea, sin punto y coma; una línea solo continúa tras operador, coma o paréntesis abierto | El `;` cuesta 0–2 % (propia); lo que importa es que la regla sea trivial | Más del 1 % de los errores al primer intento son de continuación |
| Condiciones | Sin paréntesis | −4 % de tokens (propia) | Sube la tasa de errores de parseo |
| Palabras clave | `fn let var struct enum match if else for in while return import test` | Transferencia desde Rust, TypeScript y Go; todas son un token en los cinco tokenizadores medidos. Evita `defer`, `elsif` y `..=` | — |
| Nombres | `camelCase` para valores, `PascalCase` para tipos | `snake_case` nunca sale más barato y cuesta 2,2× en algún tokenizador (propia) | — |
| Tipos | Inferencia solo local; tipos obligatorios en toda firma de nivel superior; forma `x: T` | El 94 % de los errores de compilación de IA en TypeScript son de tipos, y la realimentación tipada sube la reparación (33 → 53 % en un modelo pequeño) | Los agentes piden tipos inferidos en más del 10 % de las ediciones |
| Tipos base | `int` y `float` de 64 bits, `bool`, `str` (UTF-8); `i32`, `u8`, `f32`… opcionales | Uno frente a 2–3 tokens (propia). El compilador estrecha el almacenamiento si demuestra el rango | El dominio es sobre todo de ancho fijo |
| Colecciones | `[T]`, `Map<K, V>`, `Set<T>` | `[T]` es la forma más corta medida | — |
| Ausencia y error | `T?` con `Some`/`None`; `Result<T, E>` con `Ok`/`Err`; `?` postfijo; `match` exhaustivo; sin `null` ni excepciones | Transferencia desde Rust y Swift; `?` es la propagación más barata medida | Una prueba A/B no muestra menos fallos en ejecución |
| Mutación | Parámetros de solo lectura; `mut` en el parámetro y en el argumento de la llamada | La mutación se ve donde ocurre | — |
| Genéricos | `<T>` con monomorfización; interfaces pequeñas de implementación explícita; sin tipos asociados, herencia, sobrecarga ni operadores definidos por el usuario | Un tercio de los errores de IA en Rust son de tipos y traits | — |
| Módulos | Nombres calificados por módulo; sin reexportaciones, alias ni ciclos de importación | El 44 % de los errores de IA en Rust son de nombres; todo debe encontrarse con una búsqueda de texto | — |
| Efectos | No se escriben: el compilador los infiere con el programa entero y `veloz sig` los muestra. Una palabra opcional los exige (`pure`, `noalloc`) | Dan velocidad sin costar tokens | Un benchmark muestra que declararlos reduce fallos |
| Concurrencia | Tareas estructuradas por ámbito; sin funciones «de color»: el compilador infiere cuáles suspenden | Argumento, sin medición | Aparece evidencia en contra |
| Enteros | El desbordamiento es una trampa, que puede detectarse con retraso al final del bucle; operaciones `wrapping` y `saturating` explícitas | Comportamiento definido y bucles vectorizables | Tras el análisis de rangos el coste residual supera el 2 % de media geométrica: pasa a envolver |
| Gramática | LL(1), léxico independiente del contexto, tipado comprobable de izquierda a derecha. Se publica en Lark y GBNF | Es lo que hace barata la generación restringida | — |
| Metaprogramación | Ninguna en la primera versión | Obliga a reconstruir contexto | Se demuestra una necesidad medida |
| Grafías ajenas | El compilador reconoce `elif`, `.length`, `println!`, `let mut` o un `;` final y devuelve la corrección exacta | Del 80 al 94 % del código que generan los modelos sigue costumbres de lenguajes mayoritarios | Las API inventadas bajan del 2 % de los errores |

Estilo de referencia (ilustrativo; la gramática exacta se fija en `SPEC.md`):

```
struct User {
    name: str
    age: int
}

enum LoadError {
    NoComma(line: int)
    BadAge(line: int)
}

fn parseUser(text: str, line: int) -> Result<User, LoadError> {
    match text.splitOnce(",") {
        Some((name, age)) => {
            let years = age.trim().toInt().okOr(LoadError.BadAge(line))?
            return Ok(User { name: name.trim(), age: years })
        }
        None => return Err(LoadError.NoComma(line))
    }
}

fn birthday(user: mut User) {
    user.age += 1
}

test "parseUser lee nombre y edad" {
    var u = parseUser("Ana, 31", 1)?
    birthday(mut u)
    assert u.age == 32
}
```

## B3. Memoria

Semántica de valor mutable con referencias de segunda clase: una referencia no se guarda en un campo ni se devuelve. Los valores se construyen directamente en su destino, también a través de llamadas, así que devolver por valor no copia.

El compilador elige para cada valor el nivel más barato que pueda demostrar. Si una prueba falla, el valor baja al nivel siguiente; nunca es un error de compilación.

| Nivel | Qué es | Detalle |
|---|---|---|
| 0 | Disposición | Structs inline, enums con tamaño según la variante, campos de tamaño variable inline, índices de 32 bits. De aquí sale el mejor resultado de Goose: 2× en tiempo y 4× en memoria |
| 1 | Registros y pila | — |
| 2 | Regiones de ámbito inferidas | Puntero que avanza sobre espacio virtual reservado. Cada iteración de bucle, petición y tarea es un ámbito. Contenedores que crecen sin comprobar capacidad. Solo datos sin destructor |
| 3 | Dueño único en heap | Se mueve (movimientos inferidos por último uso) y se libera al terminar su dueño |
| 4 | Pools con asas generacionales | Índice u32 + generación u32, para listas enlazadas, árboles y grafos, también cíclicos. La comprobación se elimina si se demuestra que del pool no se libera |
| 5 | Conteo de referencias | Solo para datos realmente compartidos: Perceus con reutilización guiada por `drop`, no atómico, confinado a un hilo, sobre tipos acíclicos por construcción |

Reglas que completan el modelo:

- **Entre hilos:** se mueven valores únicos. Para compartir, se congela en profundidad y solo la raíz congelada lleva contador atómico.
- **Closures:** las que no escapan se integran. Las que escapan capturan por movimiento o copia y su entorno vive inline en su dueño.
- **Tareas:** sus marcos van en un bloque por tarea con disciplina de pila. Una tarea desligada es dueña de una región que muere con ella.
- **Recursos** (archivos, sockets, locks): tipos no copiables que se cierran al final del ámbito de su dueño, en orden inverso, también con error o cancelación. Prohibidos en el nivel 2, porque liberar en bloque no ejecuta destructores.
- **FFI:** solo el heap de dueño único fijado tiene direcciones estables. Punteros crudos solo en módulos declarados de confianza.
- **Devolver RAM:** las regiones devuelven al sistema las páginas por encima de su marca al salir del ámbito, el heap purga páginas vacías y los heaps por tarea se destruyen de golpe.
- **Visibilidad:** `veloz costes` dice en qué nivel cayó cada punto de asignación y por qué. Una palabra opcional convierte en error caer de nivel.

| Se cambia si | Alternativa |
|---|---|
| El máximo de una región supera 2× los bytes vivos, o más del 10 % de los bytes quedan en la región más externa | Revisar la inferencia de ámbitos antes que el modelo |
| Las asas generacionales cuestan más del 5 % en la suite de grafos | Más eliminación estática de la comprobación |
| La regla de tipos acíclicos rechaza más del 5 % de los programas escritos por IA | Colector de ciclos por borrado de prueba, solo para esos tipos |
| El contador atómico de la raíz cuesta más del 5 % en la suite paralela | Conteo de referencias sesgado (biased RC) |

## B4. Compilador y optimización

| Pieza | Elección | Reserva | Se cambia si |
|---|---|---|---|
| Lenguaje del compilador | C++23 | Rust con capa en C++ | — |
| IR de alto nivel | Propio, con efectos, propiedad, rangos y forma de los datos | Dialecto MLIR | Aparece un objetivo de tensores o aceleradores |
| Backend optimizador | LLVM 23.1.x, versión fijada | GCC a través de C emitido | GCC gana más del 3 % de media geométrica |
| Backend de depuración | TPDE-LLVM (8–24× más rápido que `-O0`) | LLVM `-O0`, Cranelift | No soporta LLVM 23 o el IR necesario |
| `veloz check` | No genera código | — | — |
| Unidad de compilación | El programa entero en un módulo; se parte solo para paralelizar | — | — |
| Perfiles | PGO por instrumentación a nivel de IR, segunda pasada sensible al contexto, MemProf y perfil temporal para el arranque | CSSPGO | Las ejecuciones de entrenamiento no son representativas |
| Disposición del código | BOLT (hasta +8 % sobre PGO y LTO) | Propeller | Propeller gana ≥ 1 % o BOLT no cabe en memoria |
| Disposición de datos | Siempre: reordenar campos, empaquetar nichos, eliminar campos muertos. Guiado por perfil: SoA, separar caliente y frío, comprimir a índices de 32 bits. Anulable por tipo | Anotaciones | La elección automática empeora más del 2 % algún caso |
| Vectorización | Autovectorización de LLVM, construcción SPMD propia (modelo `foreach`/`varying` de ISPC) y multiversión de los bucles calientes: base x86-64-v3 con clones AVX-512/AVX10 | Biblioteca tipo Highway | SPMD no da ≥ 1,5× sobre la automática en los núcleos propios |
| Contratos | `noalias` en todo, reasociación de coma flotante por función y solo si se pide, aritmética con trampa retrasada en bucles | — | — |
| Comprobaciones | Activas. El fallo salta a un único bloque frío sin desenrollado. Iteradores y rangos primero; análisis de rangos en el IR propio y versionado de bucles; las de desbordamiento se insertan tarde. Una palabra (`checkfree`) exige que no quede ninguna | — | — |
| `--Omax` | Autoajustador propio y reanudable sobre mandos que conservan la semántica: inlining, factores de desenrollado y de vector, disposición de código y de datos. Oráculo: los benchmarks. Puerta: la suite diferencial | Inlining aprendido estilo MLGO | La política aprendida gana con el mismo presupuesto |
| Corrección | Generador de programas aleatorios sin comportamiento indefinido contra el intérprete de referencia, mutación guiada por cobertura y compilador construido con ASan y UBSan | — | Nunca |

Dos avisos. Alive2 solo valida transformaciones de LLVM IR a LLVM IR dentro de una función: sirve para los pases propios, no para el descenso del IR propio. Y `noalias` ya provocó errores de compilación en LLVM con Rust: esas rutas necesitan fuzzing propio.

Fuera de la primera versión: MLIR, backend GCC, superoptimizadores en línea (Minotaur e Hydra solo para extraer reglas) y optimización guiada por modelos de lenguaje.

## B5. Runtime y biblioteca

| Componente | Diseño a copiar | Reserva |
|---|---|---|
| Asignador del heap | Heaps por hilo y por tarea estilo mimalloc v3: listas libres sin atómicos, destrucción del heap entero, pistas caliente/frío del perfil | Diseño tcmalloc (cachés por CPU, páginas enormes) |
| Mapa hash | SwissTable (hashbrown) con hash rápido tipo foldhash y semilla; variante resistente a HashDoS para datos de red | Disposición de Go 1.24 si importa la latencia al crecer |
| Ordenación | ipnsort (inestable) y driftsort (estable) | vqsort para claves primitivas |
| Texto | UTF-8; cadena «alemana» de 16 bytes (12 inline, o prefijo de 4 más referencia); validación estilo simdutf | Optimización clásica de cadena corta |
| Números | fast_float para leer; zmij para imprimir | Dragonbox |
| JSON | Diseño de simdjson | — |
| Tareas | Robo de trabajo con promoción por latido (heartbeat); marcos sin pila generados por el compilador | Fork-join estilo Rayon |
| E/S asíncrona | Sin investigar: mide io_uring frente a epoll antes de fijarla | — |

## B6. Herramientas para agentes

Es la pieza que más mueve el objetivo 2: el formato de edición por sí solo cambió el éxito 15 puntos en un estudio, y leer código es el cuello de botella de los agentes en lenguajes poco vistos.

- `veloz check`: formato, tipos y pruebas del archivo en una sola orden, en milisegundos, sin generar código. No ejecuta pruebas si hay errores de tipo.
- **Diagnósticos:** JSON con identificador estable, los dos tipos en conflicto, todos los tramos de código relacionados, la corrección aplicable y un ejemplo mínimo. Completos, no escuetos: la tasa de reparación sube con el detalle. Un error por causa raíz. Los de memoria nombran los dos extremos: qué valor y qué dueño.
- `veloz sig <módulo>`: firmas, efectos inferidos y una línea de documentación. Es lo que el agente carga en vez del código.
- `veloz types <archivo>`: tipos inferidos.
- `veloz explain <id|tema>`: la versión larga, a demanda.
- `veloz patch`: edición dirigida por símbolo (sustituir una función por su nombre), protegida por el hash del original.
- `veloz fmt` y `veloz costes`.
- **Salidas:** con tope de tokens y pista de cómo continuar; sin banners ni colores; si todo va bien, silencio. La línea de órdenes funciona sola; LSP llega después.
- **Documentación:** tarjeta de 2.000 tokens escrita como diferencias respecto a Rust y TypeScript, con lista de falsos amigos; corpus de ejemplos compilados en CI (añadir ejemplos verificados casi dobló el acierto en un lenguaje no visto: 16,7 → 30,8 %); índice comprimido de la documentación en `AGENTS.md`. Meta: 90 % de compilación al primer intento con solo la tarjeta.
- **Generación restringida:** gramática Lark donde la API la admite (OpenAI, y vLLM o llama.cpp con llguidance). Anthropic y Google solo ofrecen JSON Schema, así que nunca es requisito.

## B7. Medición

**Versiones a 20/11/2026.** Go 1.27.4 · Rust 1.100.0 · GCC 16.2 · Clang/LLVM 23.1.6 · Goose v2026.4. Fíjalas al empezar una comparación y no las muevas a mitad.

**Tres filas por lenguaje, Veloz en las tres:** `default` (la compilación de release documentada), `max` y `paridad-segura` (C++ con `_LIBCPP_HARDENING_MODE=fast`; Rust, Go y Goose tal cual).

| Lenguaje | Configuración `max` |
|---|---|
| Go | `GOAMD64=v3 go build -pgo=default.pgo -trimpath`. Dos filas de ejecución: por defecto y `GOGC=off GOMEMLIMIT=<presupuesto>`. Nunca `-gcflags=-B` |
| Rust | `opt-level=3`, `lto="fat"`, `codegen-units=1`, `panic="abort"`, `-C target-cpu=x86-64-v3`, mimalloc, `cargo pgo` y BOLT |
| C y C++ | GCC y Clang: `-O3 -march=x86-64-v3 -flto -fno-semantic-interposition -static -DNDEBUG`, `-std=c23` / `-std=c++23`, PGO, BOLT, mimalloc |
| Goose | `goose build`, y una fila experta: `goose -O2 --standalone -o x.c` seguido de `clang -O3 -march=x86-64-v3 -flto` |

Las cifras que publica Goose no sirven de vara: se midieron contra Rust con `-O -C codegen-units=1` (sin LTO ni CPU objetivo) y C++ con `/O2`, en Windows, tomando el mejor de 3 y sin fijar núcleos. Reprodúcelas con la fila `max`.

**Máquina.** Una sola, física, Intel x86-64, Linux ≥ 6.12. Governor `performance`, turbo apagado, hermano SMT fuera de línea, núcleos aislados, E/S en tmpfs. Nada de máquinas virtuales ni portátiles.

**Ejecuciones.** 5 compilaciones con la disposición barajada (lld `--shuffle-sections`) × 30 ejecuciones:
`hyperfine -N --warmup 3 --runs 30 --metrics time_wall_clock,time_user,memory_peak_resident,instructions,cache_misses,branch_misses --export-json out.json './a' './b'`
Se informa la mediana y el intervalo de confianza bootstrap del 95 % del cociente. Media geométrica solo sobre cocientes, y siempre junto a los resultados por prueba.

**Memoria y energía.** RSS máximo y `memory.peak` de cgroup v2; nunca el tamaño virtual, porque Veloz y Goose reservan direcciones que no usan. Energía con `perf stat -a -e power/energy-pkg/,power/energy-ram/`, restando el reposo y con los mismos núcleos activos; nunca entre máquinas distintas.

**Suite.** Unos 24 programas sin bibliotecas externas, con el mismo algoritmo, suma de control y tres tamaños de entrada: los 4 de plb2; brainfuck, primes y matmul de kostya; 8 clásicos del Benchmarks Game; los 16 de Goose sin duplicados; y 3 grandes (parser JSON escrito a mano, almacén LRU, trazador de rayos). Tres pistas que no se mezclan: idiomática sin intrínsecos ni código inseguro, experta, y generada por IA. Un hilo y varios hilos por separado. Además, tres aplicaciones completas: herramienta de datos, servicio concurrente de larga vida y simulación.

**Evaluación con IA.**

- Lenguajes: Veloz, Goose, Go, Rust, C y C++. Goose es el control: también es un lenguaje que los modelos no han visto. Los dos reciben documentación del mismo tamaño en tokens.
- 200 tareas neutrales de entrada y salida estándar con pruebas ocultas: 100 de concurso publicadas después de la fecha de corte de los modelos, 60 de varios archivos y 40 de sistemas (parsers, estructuras de datos, concurrencia).
- Dos condiciones: un solo intento, y agente (compilador más pruebas públicas, 30 turnos, 500.000 tokens) sobre el arnés Inspect.
- Modelos: el de programación más reciente de cuatro familias, uno de ellos de pesos abiertos y autoalojado. Tres repeticiones por celda.
- Métricas: pruebas ocultas superadas, compilación al primer intento, turnos, llamadas a herramientas y tokens de entrada, salida y razonamiento de todas las ejecuciones, no solo de las resueltas.
- Análisis pareado por tarea, con intervalos agrupados por tarea y tokens como log-cocientes. Con unas 165 tareas se detecta una diferencia de 10 puntos.
- Ablaciones: sin tarjeta, sin ejemplos, con diagnósticos escuetos y con generación restringida.

**Invalidan una comparación:** opciones de compilación desiguales; algoritmos o E/S distintos; tareas dominadas por una biblioteca; asimetría de seguridad (C++ sin comprobar frente a lenguajes que comprueban, matemáticas rápidas en un solo lado); SIMD a mano solo en algunos; mezclar número de hilos; una sola compilación o «el mejor de N»; menos de 10 ejecuciones; problemas anteriores al corte del modelo; contar tokens solo de los aciertos.

## B8. Fuentes

Recogidas el 20/11/2026. Comprueba que cada enlace sigue vivo y qué versión lees.

- **Goose:** https://github.com/aardappel/goose — `docs/goose_spec.md`, `docs/command_line.md`, `bench/results.md`, `bench/design.md`, `bench/summary.md`
- **Lobster, memoria:** https://aardappel.github.io/lobster/memory_management.html
- **Perceus y reutilización:** https://www.microsoft.com/en-us/research/uploads/prod/2020/11/perceus-tr-v4.pdf · https://www.microsoft.com/en-us/research/uploads/prod/2023/07/flreuse.pdf
- **Semántica de valor mutable:** https://arxiv.org/abs/2106.12678 · https://www.jot.fm/issues/issue_2022_02/article2.pdf
- **Aislamiento por regiones (Swift SE-0414):** https://github.com/apple/swift-evolution/blob/main/proposals/0414-region-based-isolation.md
- **Conteo sesgado:** https://docs.rs/crate/biasedrc/latest/source/paper.pdf
- **Asas generacionales:** https://docs.rs/slotmap/latest/slotmap/ · https://verdagon.dev/blog/generational-references
- **Por qué fallaron las regiones puras (MLKit):** https://elsman.com/mlkit/pdf/retro.pdf
- **Errores de IA en Rust:** https://arxiv.org/abs/2602.22764
- **Tokens por lenguaje en agentes:** https://arxiv.org/abs/2607.22807 · https://arxiv.org/html/2508.13666v1 · https://martinalderson.com/posts/which-programming-languages-are-most-token-efficient/
- **Tipos y reparación:** https://arxiv.org/abs/2504.09246 · https://arxiv.org/html/2606.01522v1 · https://arxiv.org/abs/2602.11481
- **Lenguajes no vistos:** https://arxiv.org/abs/2603.09678 · https://huggingface.co/papers/2606.16827 · https://arxiv.org/abs/2602.09447
- **Lenguajes pensados para IA:** https://www.moonbitlang.com/blog/moonbit-ai · https://zerolang.ai/reference · https://lucumr.pocoo.org/2026/2/9/a-language-for-agents/
- **Edición y herramientas:** https://stencil.so/blog/the-harness-problem · https://www.anthropic.com/engineering/writing-tools-for-agents
- **Generación restringida:** https://github.com/guidance-ai/llguidance · https://developers.openai.com/cookbook/examples/gpt-5/gpt-5_new_params_and_tools
- **LLVM 23, TPDE, BOLT, Propeller, MemProf:** https://releases.llvm.org/23.1.0/docs/ReleaseNotes.html · https://arxiv.org/abs/2505.22610 · https://arxiv.org/abs/1807.06735 · https://snehasish.github.io/assets/pdf/shen-asplos23.pdf · https://llvm.org/docs/MemProf.html
- **Disposición de datos:** https://gcc.gnu.org/wiki/GCCSpec2017/mcf · https://arxiv.org/abs/2405.12507
- **Comprobaciones:** https://security.googleblog.com/2024/11/retrofitting-spatial-safety-to-hundreds.html · https://blog.regehr.org/archives/1384
- **Corrección del compilador:** https://ralfj.de/blog/2024/11/25/rustlantis.html · https://users.cs.utah.edu/~regehr/yarpgen-oopsla20.pdf · https://blog.regehr.org/archives/1722
- **Runtime:** https://microsoft.github.io/mimalloc/ · https://github.com/rust-lang/hashbrown · https://github.com/rust-lang/rust/pull/124032 · https://cedardb.com/blog/german_strings · https://vitaut.net/posts/2025/faster-dtoa/ · https://users.cs.northwestern.edu/~pdinda/Papers/asplos24-hbc.pdf
- **Competidores:** https://go.dev/doc/pgo · https://go.dev/doc/gc-guide · https://nnethercote.github.io/perf-book/build-configuration.html · https://github.com/Kobzol/cargo-pgo · https://libcxx.llvm.org/Hardening.html · https://github.com/llvm/llvm-project/blob/main/bolt/README.md
- **Medir bien:** https://llvm.org/docs/Benchmarking.html · https://github.com/sharkdp/hyperfine · https://sape.inf.usi.ch/publications/asplos09.html · https://arxiv.org/abs/2410.05460 · https://arxiv.org/abs/2411.00640
- **Evaluar IA en un lenguaje nuevo:** https://inspect.aisi.org.uk/ · https://arxiv.org/abs/2508.04865 · https://nuprl.github.io/MultiPL-E/
