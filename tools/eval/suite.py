"""Suite piloto H3. Casos, oráculos y soluciones se fijan antes de la inferencia."""
from dataclasses import dataclass
import json
from math import gcd, isqrt
from functools import reduce
from operator import mul


def literal(x):
    if isinstance(x, bool):
        return str(x).lower()
    if isinstance(x, str):
        return json.dumps(x, ensure_ascii=False)
    return str(x)


@dataclass(frozen=True)
class Task:
    name: str
    signature: str
    request: str
    prelude: str
    public: tuple
    hidden: tuple
    solution: str


TASKS = []


def case(signature, request, solution, oracle, inputs, prelude="", raw=False):
    name = signature.split("(")[0]
    rows = []
    for args in inputs:
        if not isinstance(args, tuple):
            args = (args,)
        spelling = ", ".join(args) if raw else ", ".join(literal(x) for x in args)
        expected = literal(oracle(*args))
        rows.append(f"{name}({spelling}) == {expected}")
    TASKS.append(Task(name, "fn " + signature, request, prelude, tuple(rows[:1]),
                      tuple(rows[1:]), "fn " + signature + " {\n" + solution.strip() + "\n}"))


case("doubleValue(n: int) -> int", "Devuelve el doble de n.",
     "return n * 2", lambda n: 2*n, [3, 0, -5, 17, 100])
case("rectArea(w: int, h: int) -> int", "Calcula el área de un rectángulo de lados no negativos.",
     "return w * h", lambda a,b: a*b, [(4,3),(0,9),(1,7),(13,8)])
case("absolute(n: int) -> int", "Devuelve el valor absoluto. El mínimo int64 queda fuera del dominio.",
     "if n < 0 { return -n }\nreturn n", abs, [-4,0,1,-17,90])
case("smaller(a: int, b: int) -> int", "Devuelve el menor de dos enteros.",
     "if a < b { return a }\nreturn b", min, [(3,5),(0,0),(-8,-3),(80,-4)])
case("medianThree(a: int, b: int, c: int) -> int", "Devuelve la mediana de los tres enteros.",
     """if a <= b {
        if b <= c { return b }
        if a <= c { return c }
        return a
    }
    if a <= c { return a }
    if b <= c { return c }
    return b""", lambda *x: sorted(x)[1], [(2,1,3),(4,4,0),(8,7,2),(-8,0,-3),(3,8,1),(1,3,8)])
case("clamp(x: int, lo: int, hi: int) -> int", "Acota x al intervalo inclusivo [lo,hi], con lo <= hi.",
     "if x < lo { return lo }\nif x > hi { return hi }\nreturn x",lambda x,a,b:max(a,min(x,b)),
     [(3,0,4),(-2,0,4),(5,0,4),(9,-1,-1),(0,0,4)])
case("signOf(n: int) -> int", "Devuelve -1, 0 o 1 según el signo de n.",
     "if n < 0 { return -1 }\nif n > 0 { return 1 }\nreturn 0",
     lambda n:(n>0)-(n<0),[-5,0,9,-200,2])
case("evenValue(n: int) -> bool", "Indica si n es par.",
     "return n % 2 == 0",lambda n:n%2==0,[3,-4,-3,0,129])
case("leapYear(year: int) -> bool", "Aplica la regla gregoriana de años bisiestos.",
     "return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)",lambda n:n%4==0 and(n%100!=0 or n%400==0),
     [2024,1900,2000,2023,2100,2400])
case("between(x: int, a: int, b: int) -> bool", "Indica si x pertenece al intervalo cerrado con extremos a,b en cualquier orden.",
     "return a <= x && x <= b || b <= x && x <= a",lambda x,a,b:min(a,b)<=x<=max(a,b),
     [(2,1,4),(4,4,-2),(0,-4,-1),(2,2,2),(0,4,-1)])
case("exclusive(a: bool, b: bool) -> bool","Devuelve la disyunción exclusiva.",
     "return a != b",lambda a,b:a!=b,[(True,True),(False,True),(True,False),(False,False)])
case("majority(a: bool, b: bool, c: bool) -> bool","Indica si al menos dos argumentos son verdaderos.",
     "return a && b || a && c || b && c",lambda *x:sum(x)>=2,
     [(True,False,True),(False,False,False),(True,False,False),(False,True,True),(True,True,True)])
case("distance(a: int, b: int) -> int","Calcula la distancia absoluta entre dos enteros pequeños.",
     "if a >= b { return a - b }\nreturn b - a",lambda a,b:abs(a-b),[(2,7),(-8,-1),(6,6),(10,-3)])
case("ceilChunks(n: int, size: int) -> int","Mínimo número de contenedores de capacidad size>0 para n>=0 objetos.",
     "return n / size + extra(n % size)\n}\nfn extra(r: int) -> int {\nif r == 0 { return 0 }\nreturn 1",
     lambda n,s:(n+s-1)//s,[(5,2),(0,5),(8,4),(1,9),(17,3)])
case("wrapHour(hour: int) -> int","Normaliza una hora entera, incluso negativa, a 0..23.",
     "return ((hour % 24) + 24) % 24",lambda h:h%24,[27,-1,24,0,-49,71])
case("sumTo(n: int) -> int","Suma los enteros de 1 a n inclusive para n>=0.",
     "var sum = 0\nfor i in 1..n + 1 { sum += i }\nreturn sum",lambda n:n*(n+1)//2,[4,0,1,22,101])
case("sumSquares(n: int) -> int","Suma los cuadrados de 1 a n inclusive para n>=0.",
     "var sum = 0\nfor i in 1..n + 1 { sum += i * i }\nreturn sum",lambda n:sum(i*i for i in range(1,n+1)),[3,0,1,14,25])
case("factorial(n: int) -> int","Calcula n!, con 0 <= n <= 12.",
     "var total = 1\nfor i in 2..n + 1 { total *= i }\nreturn total",
     lambda n:reduce(mul,range(2,n+1),1),[5,0,1,4,12])
case("power(base: int, exp: int) -> int","Eleva base al entero exp>=0. La potencia de exponente cero vale 1.",
     "var p = 1\nfor i in 0..exp { p *= base }\nreturn p",pow,[(2,5),(0,0),(-2,3),(-2,4),(7,1)])
def fib(n):
    a,b=0,1
    for _ in range(n): a,b=b,a+b
    return a
case("fib(n: int) -> int","Devuelve Fibonacci con F(0)=0 y F(1)=1, para 0<=n<=20.",
     "var a = 0\nvar b = 1\nfor i in 0..n {\nlet next = a + b\na = b\nb = next\n}\nreturn a",fib,[6,0,1,2,12,20])
case("greatest(a: int, b: int) -> int","Máximo común divisor de dos enteros no negativos. MCD(0,0)=0.",
     "var x = a\nvar y = b\nwhile y != 0 {\nlet r = x % y\nx = y\ny = r\n}\nreturn x",gcd,
     [(12,18),(0,0),(9,0),(21,14),(13,17),(0,33)])
case("least(a: int, b: int) -> int","Mínimo común múltiplo de dos enteros no negativos. Si alguno es cero, devuelve cero.",
     "if a == 0 || b == 0 { return 0 }\nvar x = a\nvar y = b\nwhile y != 0 {\nlet r = x % y\nx = y\ny = r\n}\nreturn a / x * b",
     lambda a,b:0 if not a*b else a*b//gcd(a,b),[(4,6),(0,9),(0,0),(7,13),(8,16)])
case("prime(n: int) -> bool","Indica si n es primo. Los enteros menores que 2 no son primos.",
     "if n < 2 { return false }\nvar d = 2\nwhile d <= n / d {\nif n % d == 0 { return false }\nd += 1\n}\nreturn true",
     lambda n:n>1 and all(n%i for i in range(2,isqrt(n)+1)),[7,-2,1,2,49,97,121])
case("digitSum(n: int) -> int","Suma las cifras decimales de n>=0.",
     "var a = n\nvar s = 0\nwhile a > 0 {\ns += a % 10\na /= 10\n}\nreturn s",
     lambda n:sum(map(int,str(n))),[23,0,1001,9876,5])
case("digitCount(n: int) -> int","Número de cifras decimales de n>=0; cero tiene una cifra.",
     "var a = n\nvar count = 1\nwhile a >= 10 {\ncount += 1\na /= 10\n}\nreturn count",
     lambda n:len(str(n)),[78,0,9,100,9999,10000])
case("reverseDigits(n: int) -> int","Invierte las cifras de n>=0 y descarta ceros iniciales del resultado.",
     "var a = n\nvar r = 0\nwhile a > 0 {\nr = r * 10 + a % 10\na /= 10\n}\nreturn r",
     lambda n:int(str(n)[::-1]),[120,0,7,456,10002])
case("countOnes(n: int) -> int","Número de unos de la representación binaria de n>=0.",
     "var a = n\nvar s = 0\nwhile a > 0 {\ns += a % 2\na /= 2\n}\nreturn s",
     int.bit_count,[7,0,1,64,85,255])
case("powerTwo(n: int) -> bool","Indica si n es una potencia entera de dos con exponente no negativo.",
     "if n <= 0 { return false }\nvar a = n\nwhile a % 2 == 0 { a /= 2 }\nreturn a == 1",
     lambda n:n>0 and not n&(n-1),[8,-2,0,1,3,1024])
case("rootFloor(n: int) -> int","Mayor entero cuyo cuadrado no supera n, para 0<=n<=10000.",
     "var k = 0\nwhile (k + 1) * (k + 1) <= n { k += 1 }\nreturn k",isqrt,[10,0,1,24,25,9999])
case("triangular(n: int) -> bool","Indica si n>=0 es 0 o una suma 1+2+...+k.",
     "var k = 0\nvar sum = 0\nwhile sum < n {\nk += 1\nsum += k\n}\nreturn sum == n",
     lambda n:isqrt(8*n+1)**2==8*n+1,[6,0,2,10,12,55])
case("divisors(n: int) -> int","Cuenta los divisores positivos de n>0.",
     "var count = 0\nfor d in 1..n + 1 {\nif n % d == 0 { count += 1 }\n}\nreturn count",
     lambda n:sum(n%i==0 for i in range(1,n+1)),[6,1,17,25,36])
case("multipleSum(n: int) -> int","Suma los positivos estrictamente menores que n divisibles por 3 o por 5, una sola vez.",
     "var s = 0\nfor i in 1..n {\nif i % 3 == 0 || i % 5 == 0 { s += i }\n}\nreturn s",
     lambda n:sum(i for i in range(1,n) if i%3==0 or i%5==0),[10,0,1,15,16,30])
case("collatz(n: int) -> int","Cuenta los pasos hasta 1 de Collatz para 1<=n<=20; par divide por 2, impar multiplica por 3 y suma 1.",
     "var k = n\nvar count = 0\nwhile k != 1 {\nif k % 2 == 0 { k /= 2 } else { k = 3 * k + 1 }\ncount += 1\n}\nreturn count",
     lambda n:{3:7,1:0,2:1,5:5,7:16,20:7}[n],[3,1,2,5,7,20])
case("choose(n: int, k: int) -> int","Coeficiente binomial C(n,k) para 0<=k<=n<=12.",
     "var p = 1\nfor i in 1..k + 1 { p = p * (n - i + 1) / i }\nreturn p",
     __import__("math").comb,[(5,2),(0,0),(6,0),(6,6),(10,3)])
case("overlap(a: int, b: int, c: int, d: int) -> int","Longitud de la intersección de [a,b) y [c,d), con a<=b y c<=d.",
     "var lo = a\nif c > lo { lo = c }\nvar hi = b\nif d < hi { hi = d }\nif hi <= lo { return 0 }\nreturn hi - lo",
     lambda a,b,c,d:max(0,min(b,d)-max(a,c)),[(0,4,2,7),(0,4,4,7),(-8,-2,-6,-3),(0,0,0,8),(1,9,3,5)])
case("byteSize(text: str) -> int","Número de bytes UTF-8 de text.",
     "return len(text)",lambda s:len(s.encode()),["hola","","á","🍋","Añejo"])
case("sameSize(a: str, b: str) -> bool","Indica si las longitudes en bytes UTF-8 coinciden.",
     "return len(a) == len(b)",lambda a,b:len(a.encode())==len(b.encode()),[("ab","ñ"),("","é"),("♟","123"),("abc","")])
case("longer(a: str, b: str) -> str","Devuelve el texto más largo en bytes UTF-8; gana a en empate.",
     "if len(a) >= len(b) { return a }\nreturn b",lambda a,b:a if len(a.encode())>=len(b.encode()) else b,
     [("a","abcd"),("ñ","xy"),("","z"),("🚲","abc"),("A","B")])
case("textOr(text: str, fallback: str) -> str","Si text es vacío devuelve fallback. En otro caso devuelve text.",
     "if text == \"\" { return fallback }\nreturn text",lambda t,f:t or f,[("","otro"),("uno","dos"),("",""),("ñ","x")])
case("colorCode(color: str) -> int","Devuelve 1 para rojo, 2 para verde, 3 para azul, y 0 para cualquier otro texto. Distingue mayúsculas.",
     'if color == "rojo" { return 1 }\nif color == "verde" { return 2 }\nif color == "azul" { return 3 }\nreturn 0',
     lambda c:{"rojo":1,"verde":2,"azul":3}.get(c,0),["rojo","verde","azul","Rojo",""])
case("bytesFee(text: str, price: int) -> int","Multiplica la longitud de text en bytes UTF-8 por price.",
     "return len(text) * price",lambda t,n:len(t.encode())*n,[("ñ",3),("",8),("abc",0),("🚲",5)])
case("switchText(flag: bool, yes: str, no: str) -> str","Escoge yes si flag es verdadero, y no si es falso.",
     "if flag { return yes }\nreturn no",lambda f,a,b:a if f else b,[(True,"a","b"),(False,"a","b"),(True,"","f"),(False,"é","中")])
POINT = "struct Point { x: int, y: int }"
def pt(x,y): return f"Point {{ x: {x}, y: {y} }}"
POINTS = [(1,2),(0,0),(-3,7),(6,-8),(-1,-4)]
def ptCase(signature, desc, body, oracle):
    rows = {pt(x,y):oracle(x,y) for x,y in POINTS}
    case(signature,desc,body,lambda v:rows[v],list(rows),POINT,True)
ptCase("manhattan(p: Point) -> int","Distancia Manhattan del punto al origen.",
       "var x = p.x\nvar y = p.y\nif x < 0 { x = -x }\nif y < 0 { y = -y }\nreturn x + y",lambda x,y:abs(x)+abs(y))
ptCase("squaredNorm(p: Point) -> int","Devuelve el cuadrado de la distancia euclídea al origen.",
       "return p.x * p.x + p.y * p.y",lambda x,y:x*x+y*y)
ptCase("aboveAxis(p: Point) -> bool","Indica si el punto está estrictamente por encima del eje horizontal.",
       "return p.y > 0",lambda x,y:y>0)
POINT_PAIRS = [((0,0),(1,1)),((5,0),(-2,1)),((2,1),(2,5)),((1,1),(1,1))]
case("displacement(a: Point, b: Point) -> int","Componente horizontal con signo del vector que va de a a b.",
     "return b.x - a.x",lambda a,b:{(pt(*p),pt(*q)):q[0]-p[0] for p,q in POINT_PAIRS}[a,b],
     [(pt(*p),pt(*q)) for p,q in POINT_PAIRS],POINT,True)
OPTION = "enum Lookup { Found(value: int), Missing }"
values = [("Lookup.Found(8)",8),("Lookup.Missing",None),("Lookup.Found(0)",0),("Lookup.Found(-7)",-7)]
case("lookupOr(x: Lookup, backup: int) -> int","Extrae el valor de Found o usa backup para Missing.",
     "match x {\nLookup.Found(n) => return n\nLookup.Missing => return backup\n}",
     lambda x,y: int(y) if dict(values)[x] is None else dict(values)[x],
     [(v,str(i)) for (v,_),i in zip(values,[2,3,9,-8])],OPTION,True)
case("wasFound(x: Lookup) -> bool","Indica si x es la variante Found, cualquiera que sea su valor.",
     "match x {\nLookup.Found(n) => return true\nLookup.Missing => return false\n}",
     lambda x:dict(values)[x] is not None,[v for v,_ in values],OPTION,True)
TRI = "enum Signal { Stop, Wait, Go }"
case("duration(x: Signal) -> int","Da 30 para Stop, 4 para Wait y 20 para Go.",
     "match x {\nSignal.Go => return 20\nSignal.Wait => return 4\nSignal.Stop => return 30\n}",
     lambda x:{"Signal.Stop":30,"Signal.Wait":4,"Signal.Go":20}[x],["Signal.Stop","Signal.Wait","Signal.Go"],TRI,True)
case("messageBytes(x: Message) -> int","Devuelve el tamaño en bytes UTF-8 de Text o cero para Empty.",
     "match x {\nMessage.Text(s) => return len(s)\nMessage.Empty => return 0\n}",
     lambda x:{'Message.Text("sol")':3,"Message.Empty":0,'Message.Text("ñ")':2,'Message.Text("")':0}[x],
     ['Message.Text("sol")',"Message.Empty",'Message.Text("ñ")','Message.Text("")'],
     "enum Message { Text(s: str), Empty }",True)
assert len(TASKS) == 50 and len({t.name for t in TASKS}) == 50
