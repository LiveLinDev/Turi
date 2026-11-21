"""Independent scalar holdout. The model sees the first example only."""
from math import gcd
from suite import Task

def item(name,params,ret,request,oracle,vectors):
    def lit(value):return str(value).lower() if isinstance(value,bool) else str(value)
    checks=[name+"("+", ".join(map(lit,args))+") == "+lit(oracle(*args)) for args in vectors]
    sig="fn "+name+"("+", ".join(p+": "+t for p,t in params)+") -> "+ret
    return Task(name,sig,request,"",tuple(checks[:1]),tuple(checks[1:]),"")

def product_digits(n):
    if n==0:return 0
    result=1
    while n:
        result*=n%10;n//=10
    return result

def collatz_once(n):
    return n//2 if n%2==0 else n*3+1

one=[(x,) for x in (6,0,1,2,9,18,37,92,301)]
pair=[(6,4),(0,3),(7,1),(12,4),(19,5),(100,13),(4,9)]
both=[("n","int"),("k","int")]
single=[("n","int")]
HOLDOUT=[
 item("roundMultiple",both,"int","Devuelve el menor múltiplo de k mayor o igual que n. n >= 0 y k > 0.",lambda n,k:(n+k-1)//k*k,pair),
 item("lastPair",single,"int","Devuelve las dos últimas cifras de n como entero. n >= 0.",lambda n:n%100,one),
 item("weekdayNext",both,"int","Día numerado de 0 a 6 después de avanzar k días desde n, con n y k no negativos.",lambda n,k:(n+k)%7,pair),
 item("digitProduct",single,"int","Producto de las cifras decimales de n no negativo. Para 0 devuelve 0.",product_digits,one),
 item("sumOddBelow",single,"int","Suma los enteros positivos impares estrictamente menores que n.",lambda n:sum(range(1,n,2)),one),
 item("coprimePair",both,"bool","Indica si el máximo común divisor de los dos enteros no negativos es 1.",lambda n,k:gcd(n,k)==1,pair),
 item("sameSign",[("a","int"),("b","int")],"bool","Indica si ambos son positivos, ambos negativos o ambos son cero.",lambda a,b:(a>0)-(a<0)==(b>0)-(b<0),[(1,7),(0,0),(0,-1),(-9,-2),(5,-3),(0,4)]),
 item("withinRadius",[("a","int"),("b","int"),("r","int")],"bool","Indica si la distancia entre a y b es menor o igual que r. r no negativo.",lambda a,b,r:abs(a-b)<=r,[(2,6,4),(7,3,3),(0,0,0),(-4,2,6),(-8,-1,5)]),
 item("nextOrbit",single,"int","Un paso de Collatz. Para n par devuelve la mitad, para impar tres veces n más uno.",collatz_once,one),
 item("sumMultiples",both,"int","Suma los múltiplos positivos de k menores o iguales que n. k > 0.",lambda n,k:sum(range(k,n+1,k)),pair),
 item("ceilHalf",single,"int","Calcula la mitad de n redondeada hacia arriba para n no negativo.",lambda n:(n+1)//2,one),
 item("decimalZeroes",single,"int","Cuenta las cifras cero de n en decimal. El entero 0 contiene una cifra cero.",lambda n:str(n).count("0"),[(10,),(0,),(9,),(1002,),(1900,),(111,)]),
 item("wrapMinute",single,"int","Normaliza cualquier minuto entero al intervalo 0 a 59 de un reloj.",lambda n:n%60,[(71,),(-1,),(-60,),(0,),(60,),(-119,),(234,)]),
 item("stepToward",[("a","int"),("b","int")],"int","Mueve a una unidad hacia b. Si ya coinciden devuelve a.",lambda a,b:a+(a<b)-(a>b),[(3,8),(4,4),(-2,-6),(8,1),(-1,3)]),
 item("powerOfThree",single,"bool","Indica si n es potencia de tres. El 1 sí y el 0 no.",lambda n:n in (1,3,9,27,81,243),[(9,),(0,),(1,),(6,),(27,),(82,),(243,)]),
 item("endRun",both,"int","Número de veces consecutivas que se puede dividir exactamente n por k. n > 0 y k >= 2.",lambda n,k:sum(n%k**i==0 for i in range(1,12)),[(72,3),(7,2),(16,2),(81,3),(100,10),(1,5)])
]
