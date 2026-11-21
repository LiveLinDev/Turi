# Turi para quienes conocen Rust o TypeScript

Un archivo UTF-8 contiene funciones, tipos y pruebas. Omite módulos y dependencias en el núcleo. Escribe un programa Turi directamente, sin Markdown cuando la herramienta espere código.

```turi
fn sumBelow(limit: int) -> int {
    var total = 0
    for i in 0..limit {
        total += i
    }
    return total
}

fn main() {
    print(sumBelow(5))
}

test "límite exclusivo" {
    assert sumBelow(5) == 10
}
```

Usa `fn nombre(x: int) -> int`. Una función sin retorno de valor omite la flecha. Las firmas siempre llevan tipos. Para un ejercicio que pide una función, basta con esa función. Para un ejecutable, añade `fn main() {}`. Puedes llamar a una función declarada más abajo. Usa camelCase ASCII en variables y funciones, PascalCase en tipos y variantes.

Cada sentencia ocupa una línea. Las llaves delimitan bloques y una sentencia corta puede ir entre ellas en una sola línea. Encadena con `} else if condición {`. Para continuar una expresión, termina la línea con un operador, una coma o un paréntesis de apertura. Los comentarios empiezan con `//`.

`let x = 1` declara un nombre inmutable. `var x = 1` permite usar `x = 2`, `+=`, `-=`, `*=`, `/=` y `%=`. Un parámetro y el índice de un `for` son de solo lectura. Si necesitas modificar un parámetro crea una variable local inicializada con él. La declaración puede incluir un tipo, por ejemplo `var total: int = 0`. Un bloque anidado admite nombres locales que ocultan los de fuera.

`int` es un entero con signo de 64 bits. La división entera trunca hacia cero. El resto tiene el signo del dividendo. Un desbordamiento o división por cero detiene el programa. `bool` usa `true` y `false`. Existen `&&`, `||` con cortocircuito, y `!`. Los números se comparan con `== != < <= > >=`. En `bool` y `str` puedes usar `== !=`. Los operadores aritméticos son `+ - * / %`. No hay conversión implícita entre tipos. `len("Perú")` vale 5, la cantidad de bytes UTF-8. `print(valor)` escribe un entero, booleano o texto seguido de un salto de línea.

`if` y `while` requieren un booleano. `for i in inicio..fin` recorre enteros hasta fin exclusivo. Los límites se evalúan una vez. Un rango invertido está vacío. Usa `return expresión` explícito en todos los caminos de una función con resultado. Un `if` es una sentencia. Usa `return` sin expresión para salir de una función sin resultado.

```turi
struct Item {
    count: int
    name: str
}

enum Lookup {
    Empty
    Found(item: Item)
}

fn identity<T>(value: T) -> T {
    return value
}

fn quantity(result: Lookup) -> int {
    match result {
        Lookup.Found(item) => { return item.count }
        Lookup.Empty => { return 0 }
    }
}

test "paquete" {
    let item = Item { count: 3, name: "café" }
    assert quantity(Lookup.Found(identity(item))) == 3
}
```

Construye un struct indicando todos los campos por nombre. Las variantes de enum se califican con el tipo. El patrón de un brazo recibe los campos por posición. Un `match` es una sentencia y cubre cada variante exactamente una vez. Los genéricos de función se infieren desde los argumentos. El cuerpo debe funcionar para un tipo opaco. Los tipos nominales se pasan y se devuelven por valor.

**Falsos amigos**

| Si ibas a escribir | En Turi |
|---|---|
| `let mut n = 0;` | `var n = 0` |
| `println!(n)` o `console.log(n)` | `print(n)` |
| `s.length` o `s.len()` | `len(s)` |
| `elif` | `else if` |
| `0..=n` | `0..n` excluye el extremo. Para incluirlo, comprueba el rango antes de sumar uno. |
| `return a ? x : y` | `if a { return x } else { return y }` |
| `x++` | `x += 1` |

El núcleo carece de arreglos, mapas, índices de texto, concatenación, flotantes, entrada estándar, punteros y `break`. Resuelve las tareas del núcleo con escalares, registros, variantes y funciones. Para terminar anticipadamente una búsqueda usa `return` dentro del bucle.

`veloz check archivo.turi` comprueba el archivo completo y después ejecuta sus bloques `test`. El éxito es silencioso. Lee el JSON de error en stderr. Para aplicar un `fix`, reemplaza el intervalo de bytes UTF-8 `[start,end)` por `replacement`. Usa `veloz run archivo.turi` para ejecutar main, `veloz sig archivo.turi` para consultar firmas y `veloz build archivo.turi -o programa.exe` para generar un ejecutable.
