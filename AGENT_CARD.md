# Turi 0.5 (Rust/TS/Py/C++)

Solo Turi sin prosa. `{}` misma línea; `\n` o `,` final en `()`,`[]`,`{}` separan; `} else {`, `} else if c {`. `if`/`match` son expresiones. `//` com.

```turi
const kScale: int = 2
fn sumPos(nums: [int]) -> int requires nums.len() >= 0 {
    var acc = 0
    for x in nums {
        acc += if x > 0 { x * kScale } else { 0 }
    }
    return acc
}
fn main() {
    var v = [1, -2, 3,]
    v.push(4)
    print(sumPos(v))
}
test "sumPos" { assert sumPos([1, -2, 3]) == 8 }
```

Firmas: `@noCopy/@arena/@compact`, `[verified|trusted|raw] [pure|noalloc|checkfree|io] fn f(x:int)->int` (`void` sin `->`), `requires`/`ensures` (`result`, sin mut/mover). `const kMax:int=100` (evalúa `if`). camelCase valores; PascalCase tipos.

`let x=1` inmut; `var x=1`: `= += -= *= /= %= &= |= ^= <<= >>= >>>=`, `s.f=v`, `s.a[i]=v`. Params: `var a=x`, `var x:int` o `x:mut T`+`f(mut v)` (`mut s.f`; escalares copiables sin `E_ALIAS_MUT`). Sombrea.

Escalares (`int` 64b `0xFF,0b1010`, `i8..u64`, `float/f64,f32`): `i32(x),float(x),int(f)`, `& | ^ ~ << >> >>>`, `.abs/.min/.max/.sqrt/.floor/.ceil/.round/.pow/.isNaN/.isInf`, `wrappingAdd/Sub/Mul,saturatingAdd/Sub/Mul,sin,cos`. `bool`: `&&,||,!`. `str` UTF-8 (`+,"{x}",\{ \}`): `.len/isEmpty/contains/startsWith/endsWith/indexOf`, `s[i],s[lo..hi]`, `str/FromByte,parseInt/Float,tryParseInt/Float`. E/S: `print,write,writeByte,ewrite,eprint,flush`. Control: `if` expr, `while`, `for i in a..b`, `for x in arr`, `'lbl:`, `break,continue,return`.

```turi
@repr(C)
struct Item { count: int, name: str }
struct Box<T> { val: T }
enum Lookup { Empty, Found(item: Item) }
fn getCount(x: Lookup) -> int {
    return match x {
        Lookup.Empty => 0
        Lookup.Found(it) => it.count
    }
}
interface Scored {
    fn scoreOf(self) -> int
    fn doubled(self) -> int { return self.scoreOf() * 2 }
}
impl Scored for Item { fn scoreOf(self) -> int { return self.count } }
impl<T> Box<T> {
    fn new(v: T) -> Box<T> { return Box<T> { val: v } }
    fn get(self) -> T { return self.val }
}
fn apply(x: int, f: fn(int) -> int) -> int { return f(x) }
```

Tipos: structs (`Item{count:1,name:"a",}`, `@repr(C)/@packed/@align(N),sizeOf<T>(),alignOf<T>(),bitcast<T>(x)`); enums (`int(e)`), calificados salvo `Some/None/Ok/Err`. `match`: brazos expr/bloque, sin `,` ni `_`. Genéricos (`Box<T>`) cierran `>>`. `T?/Result<T,E>`+`?` (`.isSome/.isNone/.isOk/.isErr/.unwrapOr`). `impl Tipo/impl<T:I>` (`self/mut self,Tipo.new()`), `interface`, `dyn I`. Closures `fn(...)->R`: `||e,|x|e,|x:T|->R{...}`. Módulos: `src/m.turi`+`veloz.pkg`, `import m`, `m.Item`, `m.C_Fn()`.

Falsos amigos (otro -> Turi):
```text
let mut n=0/x++/elif -> var n=0 / x+=1 / else if
\nelse if / a?x:y -> } else if c { / if a{x}else{y}
println!/s.length/a.size/a.append -> print/s.len()/a.len()/a.push(x)
0..=n/&mut x/_ -> 0..n / x:mut T+f(mut x),mut self / i
fn f(x:int){x+=1} -> var a=x, var x:int o x:mut T
match x{A=>1,B(_)=>2} -> sin , ni _: T.A => 1
if opt isSome v -> match opt{Some(v)=>...}/o.unwrapOr(d)
poolNew<T>()/setNew() -> var p:Pool<T>=poolNew(), var s:Set<int>=setNew()
let a=[]/f"x={x}"/"n="+1 -> let a:[int]=[] / "x={x}" / "n="+str(1)
null/try..catch/HashMap/Set/Arena/Rc -> None/Result<T,E>(?)/Map,Set,Pool,Shared
FileStream/TcpStream/close/lambda x: -> File,Socket,Lock,Region(LIFO) / |x|e
async/await/Thread/unsafe/*mut T/__m256 -> taskScope/Spawn/parallelMap/freeze/trusted/raw,MutPtr,Simd
```

UFCS (`x.m()`/`m(x)`), hardware y CLI:
```text
[T]: [1,2],repeat(v,n),a[lo..hi],.len/cap/isEmpty/push/pop/clear/reserve/sort/sortStable; StringBuilder: sbNew/WithCap,.len/cap/isEmpty/push/pushByte/pushInt/pushFloat/clear/str/take
Map<K,V>/Set<T>: mapNew/WithCap/Secure,.len/isEmpty/get->V?/has/set/del/keys/values/clear/reserve; setNew/WithCap/Secure,.len/isEmpty/has/add/del/items/clear/reserve
Pool<T>/Handle<T>: poolNew/WithCap,.len/isEmpty/add/get/set/has/remove/handles; Shared<T>: sharedNew/Get/Set/Update
Afines: File,TcpListener,Socket,Mutex,Lock,Scope,Task<T>,Region; fileOpen/Read/ReadAll/ReadLine/Write/Flush/Close/IsOpen; tcpListen/LocalPort/Accept/Connect; socketRead/Write/Close/IsOpen; listenerClose/IsOpen; mutexNew/Lock/TryLock,unlock,lockIsHeld
FFI/SIMD(trusted/raw): Ptr/MutPtr<T>(ptrOf/mutPtrOf/Null/IsNull/Read/Write/Add/Cast/Alloc/AllocAligned/Free/Copy/Zero,slice/mutSlicePtr,cstr,strFromCStr/Ptr); @link("lib")extern "C"{fn f(a:i32)->i32}; extern "C" fn cb(...); fnPtr(cb)
Simd<T,N>(8-64B): simdSplat/Load/Store/LoadAligned/StoreAligned/FromArray/ToArray/Get/Set/Add/Sub/Mul/Div/Fma/Min/Max/And/Or/Xor/Eq/Lt/Select/ReduceAdd/Sum; clz/ctz/popcnt/bswap/rotl/rotr/prefetchRead/Write/spinHint
Hilos/Región(E_SEND): taskScope/Spawn/SpawnMove/Join/parallelMap,freeze/frozenGet/Read; Atomic<T>: atomicNew/Load/Store/Swap/Cas/Add/Sub/And/Or/Xor(+Ord); RingQueue<T>: ringNew/Push/Pop/Len/Cap; Region: regionNew/Alloc/AllocAligned/Reset/Used/Capacity
Sis/CLI: readByte/File,write/appendFile,tryReadFile/tryWriteFile,fileExists/remove/makeDir,argsCount/argAt,envGet,clockNs,sleepMs,execCapture/Status; veloz new <n> [--template cli|web|gpu|lib],check,run,bench,build(--release,--web,--gpu,--extreme,--cpu x86-64-v2..v4|znver4|znver5|sapphirerapids|graniterapids|avx10|native),fmt,fix,sig/slice/types/costes|costs(--explain-checks,--max-tokens),patch/patch-body,explain,mcp,lsp
Stdlib ("std"): require <pkg> 0.1.0 "std" (web,gpu,tensor,crypto,data,cli,postgres); ej. servidor.newServer,tls.listenTls,gpu.isAvailable/backendName/availableBackends/saxpyReluResidentF32/matmulReluResidentF32/uploadF32Pinned,tensor.matmul,crypto.aeadSeal,data.parseCsv/Json,cli.parseArgs,postgres.queryJson
```
