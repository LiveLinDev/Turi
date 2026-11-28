"""Python completo sobre Turi (`tools/pyfront/turipy.py`).

Cada programa se ejecuta con CPython a secas y con el motor híbrido: en sus dos modos de coma
flotante comprobando contra Python cada llamada nativa (`--verificar`), y dos veces más tal como se
usa (la primera ejecución y la siguiente, que arranca de lo guardado). Tienen que coincidir la
salida, el código de salida y los errores; ninguna llamada nativa puede dar un resultado distinto
del de Python, y las funciones marcadas tienen que haber corrido de verdad en nativo.

Los programas mezclan a propósito lo que se compila (bucles, listas, textos, recursión) con lo que
tiene que seguir en Python (clases, generadores, excepciones, enteros sin límite, alias de listas).
"""
import json
import os
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from pathlib import Path

CLI = sys.argv[1]
ROOT = Path(__file__).resolve().parent.parent
RUNNER = ROOT / "tools" / "pyfront" / "turipy.py"
LAUNCHER = ROOT / "tools" / "pyfront" / "run.py"
PYTHON_INTERNAL_HEADERS = Path(sysconfig.get_path("include")) / "internal"
NATIVE_FILE_READ = (
    sys.implementation.name == "cpython"
    and (3, 11) <= sys.version_info[:2] <= (3, 14)
    and not sysconfig.get_config_var("Py_GIL_DISABLED")
    and not sysconfig.get_config_var("WITH_DTRACE")
    and all((PYTHON_INTERNAL_HEADERS / header).is_file()
            for header in ("pycore_interp.h", "pycore_runtime.h"))
)

# (nombre, fuente, funciones que deben acabar en nativo, funciones que deben seguir en Python)
CASES = [
    ("enteros", '''
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def collatz(n):
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps

def gcd(a, b):
    while b:
        a, b = b, a % b
    return a

def divisions(a, b):
    return (a // b, a % b, -a // b, -a % b)

def divisions_negative(a, b):
    return a // -b, a % -b, divmod(a, b)

def power(base, exp):
    return base ** exp

def bits(x):
    return (x << 3, x >> 2, x & 12, x | 5)

def more_bits(x):
    return x ^ 9, ~x, -x, abs(x)

def six(x):
    return (x, x, x, x, x, x)

print(fib(24), collatz(27), collatz(97), gcd(1071, 462), gcd(0, 5))
print(divisions(17, 5), divisions(-17, 5), divisions(0, 3), divisions_negative(17, 5), divisions_negative(-17, 5))
print(power(3, 10), power(2, 62), power(2, 63), power(7, 3), power(-3, 3), power(5, 2), power(1, 100))
print(bits(100), bits(-100), bits(0), more_bits(100), more_bits(-7), six(1))
try:
    print(divisions(1, 0))
except ZeroDivisionError as error:
    print("error", error)
print(power(2, -1), power(10, 0), power(0, 0), power(2, 10), power(3, 3))
''', ["fib", "collatz", "gcd", "divisions", "divisions_negative", "power", "bits", "more_bits"], ["six"]),

    ("desbordes", '''
def factorial(n):
    result = 1
    for i in range(2, n + 1):
        result *= i
    return result

def sum_squares(xs):
    total = 0
    for x in xs:
        total += x * x
    return total

def shift(x, n):
    return x << n

print(factorial(5), factorial(20), factorial(21), factorial(40))
print(sum_squares([1, 2, 3]), sum_squares([10 ** 10, 3]), sum_squares([2 ** 70]))
print(shift(1, 10), shift(1, 62), shift(1, 64), shift(3, 100))
print(factorial(10), sum_squares([4, 5]), shift(2, 3))
''', ["factorial", "sum_squares", "shift"], []),

    ("flotantes", '''
import math

def mean(xs):
    return sum(xs) / len(xs)

def variance(xs):
    m = mean(xs)
    total = 0
    for x in xs:
        total += (x - m) ** 2
    return total / len(xs)

def norm(xs):
    acc = 0.0
    for x in xs:
        acc += x * x
    return math.sqrt(acc)

def scale(xs, k):
    return [x * k for x in xs]

def clamp(x, lo, hi):
    return max(lo, min(x, hi))

def halves(n):
    x = 1.0
    out = []
    for _ in range(n):
        x = x / 2
        out.append(x)
    return out

data = [1.5, 2.25, 3.0, 4.75, 10.0]
print("%.9f %.9f %.9f" % (mean(data), variance(data), norm(data)))
print(scale(data, 2.0), clamp(5.5, 0.0, 3.0), clamp(-1.0, 0.0, 3.0), halves(4))
print(math.floor(norm(data)), math.ceil(norm(data)), int(norm(data)))
for bad in ([], [1.0]):
    try:
        print("%.6f" % variance(bad))
    except ZeroDivisionError as error:
        print("error", error)
try:
    print(norm([float("nan")]) != norm([float("nan")]))
except ValueError as error:
    print("error", error)
''', ["mean", "variance", "norm", "scale", "clamp", "halves"], []),

    ("textos", '''
def count_vowels(s):
    return sum(1 for c in s if c in "aeiou")

def reverse_words(s):
    return " ".join(s.split(" ")[::-1])

def is_palindrome(s):
    clean = [c.lower() for c in s if c.isalnum()]
    return clean == clean[::-1]

def caesar(s, k):
    out = ""
    for c in s:
        if c.islower():
            out += chr((ord(c) - 97 + k) % 26 + 97)
        elif c.isupper():
            out += chr((ord(c) - 65 + k) % 26 + 65)
        else:
            out += c
    return out

def initials(names):
    return "".join(n[0].upper() for n in names if n)

def tokens(s):
    return [w.strip() for w in s.split(",")]

def numbers(s):
    return [int(w) for w in s.split()]

def describe(n):
    return f"n={n} doble={n * 2} par={n % 2 == 0}"

print(count_vowels("murcielago"), count_vowels(""), reverse_words("uno dos tres"))
print(is_palindrome("Anita lava la tina"), is_palindrome("hola"))
print(caesar("Hola, Mundo!", 3), caesar(caesar("Hola", 3), -3))
print(initials(["ana", "", "luis", "eva"]), tokens(" a , b,c "), numbers("1 22  -3"))
print(describe(7), describe(-4))
print(count_vowels("canción"), reverse_words("niño año"), caesar("ñandú", 1))
try:
    print(numbers("1 dos 3"))
except ValueError as error:
    print("error", error)
print(numbers(" 12   7 "), numbers("1_000 +5"))
''', ["count_vowels", "reverse_words", "is_palindrome", "caesar", "initials", "tokens", "numbers", "describe"], []),

    ("listas", '''
def bubble(xs):
    n = len(xs)
    for i in range(n):
        for j in range(n - 1 - i):
            if xs[j] > xs[j + 1]:
                xs[j], xs[j + 1] = xs[j + 1], xs[j]

def prefix_sums(xs):
    out = [0]
    for x in xs:
        out.append(out[-1] + x)
    return out

def evens(xs):
    return [x for x in xs if x % 2 == 0]

def rotate(xs, k):
    k = k % len(xs)
    return xs[k:] + xs[:k]

def dedupe(xs):
    seen = set()
    out = []
    for x in xs:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out

def second_largest(xs):
    ordered = sorted(xs, reverse=True)
    return ordered[1]

def double_in_place(xs):
    for i, x in enumerate(xs):
        xs[i] = x * 2
    return xs

def grow(xs, n):
    for i in range(n):
        xs.append(i)
    xs += [100]

data = [5, 3, 9, 1, 7]
bubble(data)
print(data, prefix_sums(data), evens(list(range(10))), rotate(data, 2), rotate(data, -1))
print(dedupe([3, 1, 3, 2, 1]), second_largest([4, 9, 2]), sorted(data, reverse=True))
same = double_in_place(data)
print(same is data, data)
grow(data, 3)
print(data, len(data))
for bad in ([], [1]):
    try:
        print(second_largest(bad))
    except IndexError as error:
        print("error", error)
try:
    print(rotate([], 1))
except ZeroDivisionError as error:
    print("error", error)
print(prefix_sums([]), evens([]), dedupe([]))
''', ["bubble", "prefix_sums", "evens", "rotate", "dedupe", "second_largest", "double_in_place", "grow"], []),

    ("tablas", '''
def zeros(n, m):
    return [[0] * m for _ in range(n)]

def identity(n):
    grid = zeros(n, n)
    for i in range(n):
        grid[i][i] = 1
    return grid

def matmul(a, b):
    n, m, p = len(a), len(b), len(b[0])
    out = [[0] * p for _ in range(n)]
    for i in range(n):
        for j in range(p):
            total = 0
            for k in range(m):
                total += a[i][k] * b[k][j]
            out[i][j] = total
    return out

def transpose(a):
    return [[a[i][j] for i in range(len(a))] for j in range(len(a[0]))]

def fill_diagonal(grid, value):
    for i in range(len(grid)):
        grid[i][i] = value

def row_sums(grid):
    return [sum(row) for row in grid]

def pascal(n):
    rows = []
    for i in range(n):
        row = [1] * (i + 1)
        for j in range(1, i):
            row[j] = rows[i - 1][j - 1] + rows[i - 1][j]
        rows.append(row)
    return rows

a = [[1, 2], [3, 4], [5, 6]]
b = [[1, 0, 2], [0, 1, 3]]
print(matmul(a, b), transpose(a), identity(3), row_sums(a))
g = zeros(3, 3)
first = g[0]
fill_diagonal(g, 7)
print(g, first is g[0], pascal(6))
shared = [[0] * 3] * 3
fill_diagonal(shared, 9)
print(shared)
try:
    print(matmul(a, [[1.0]]))
except IndexError as error:
    print("error", error)
''', ["zeros", "identity", "matmul", "transpose", "fill_diagonal", "row_sums", "pascal"], []),

    ("pares", '''
def closest(points):
    best = None
    for i in range(len(points)):
        for j in range(i + 1, len(points)):
            dx = points[i][0] - points[j][0]
            dy = points[i][1] - points[j][1]
            d = dx * dx + dy * dy
            if best is None or d < best:
                best = d
    return best

def bounds(xs):
    lo = xs[0]
    hi = xs[0]
    for x in xs:
        if x < lo:
            lo = x
        if x > hi:
            hi = x
    return lo, hi

def pairs(n):
    return [(i, i * i) for i in range(n)]

def swap(p):
    return p[1], p[0]

def edges_total(edges):
    total = 0.0
    for a, b, w in edges:
        total += w * (b - a)
    return total

def list_pair(n):
    first, second = [], []
    for i in range(n):
        first.append(i)
        second.append(i * i)
    return first, second

def borrowed_pair(values):
    first, second = [], values
    first.append(7)
    return first, second

print(closest([(0, 0), (3, 4), (1, 1)]), closest([(0, 0)]), closest([]))
print(bounds([4, -2, 9]), pairs(4), swap((1, 2.5)), swap(("a", True)))
print("%.3f" % edges_total([(0, 1, 2.5), (1, 3, 1.5)]))
first, second = list_pair(3)
first.append(9)
print(first, second, list_pair(0))
values = [1, 2]
value_pair = borrowed_pair(values)
print(value_pair, value_pair[1] is values)
try:
    print(bounds([]))
except IndexError as error:
    print("error", error)
''', ["closest", "bounds", "pairs", "swap", "edges_total", "list_pair"], ["borrowed_pair"]),

    ("diccionarios", '''
def most_common(words):
    counts = {}
    for w in words:
        counts[w] = counts.get(w, 0) + 1
    best = words[0]
    for w in words:
        if counts[w] > counts[best]:
            best = w
    return best

def two_sum(nums, target):
    seen = {}
    for i, x in enumerate(nums):
        if target - x in seen:
            return [seen[target - x], i]
        seen[x] = i
    return [-1, -1]

def distinct(xs):
    return len(set(xs))

def lookup(key):
    table = {"uno": 1, "dos": 2}
    return table[key]

def tally(xs):
    counts = {}
    for x in xs:
        counts[x] = counts.get(x, 0) + 1
    return counts

def ordered_keys(xs):
    counts = {}
    for x in xs:
        counts[x] = 1
    return sorted(counts)

print(most_common(["a", "b", "a", "c", "b", "a"]), two_sum([2, 7, 11, 15], 9), two_sum([1, 2], 9))
print(distinct([1, 2, 2, 3, 1]), lookup("dos"), tally([3, 1, 3]), ordered_keys([3, 1, 2, 3]))
try:
    print(lookup("tres"))
except KeyError as error:
    print("error", error)
''', ["most_common", "two_sum", "distinct", "lookup", "ordered_keys"], ["tally"]),

    ("python_puro", '''
import functools

class Stack:
    def __init__(self):
        self.items = []

    def push(self, x):
        self.items.append(x)
        return self

    def total(self):
        return sum(self.items)

def squares(n):
    for i in range(n):
        yield i * i

@functools.lru_cache(maxsize=None)
def ways(n):
    return 1 if n < 2 else ways(n - 1) + ways(n - 2)

def make_adder(k):
    def add(x):
        return x + k
    return add

def safe_div(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        return float("inf")

def apply(f, xs):
    return [f(x) for x in xs]

def describe(*args, **kwargs):
    return len(args), sorted(kwargs)

def weird(x):
    return eval("x + 1")

def anything(x):
    return x * 2

print(Stack().push(1).push(2).total(), list(squares(5)), ways(80), make_adder(3)(4))
print(safe_div(1, 0), safe_div(1, 4), apply(lambda v: v * 3, [1, 2]), describe(1, 2, a=1))
print(weird(41), anything(3), anything("ab"), anything([1]), anything(2.5), anything(2 ** 80))
print(sorted({"b": 1, "a": 2}.items()), {x: x * x for x in range(3)}, 10 ** 30 % 7)
''', ["anything", "safe_div"], ["apply", "weird", "squares", "ways", "describe"]),

    ("alias", '''
def keep(xs):
    ys = xs
    ys.append(1)
    return ys

def store_twice(n):
    row = [0] * n
    grid = []
    for _ in range(n):
        grid.append(row)
    grid[0][0] = 5
    return grid

def fresh_rows(n):
    grid = []
    for i in range(n):
        row = []
        for j in range(n):
            row.append(i * j)
        grid.append(row)
    grid[0][0] = 5
    return grid

def touch_both(a, b):
    a.append(1)
    return len(b)

def first_row(grid):
    return grid[0]

def rebind(xs):
    xs = xs + [9]
    return len(xs)

def loop_grow(xs):
    for x in xs:
        if x < 3:
            xs.append(x + 1)
    return len(xs)

def default_list(x, acc=[]):
    acc.append(x)
    return len(acc)

base = [7]
kept = keep(base)
print(kept is base, base, store_twice(3), fresh_rows(3))
shared = [0]
print(touch_both(shared, shared), touch_both([1], [2, 3]))
g = [[1, 2], [3]]
row = first_row(g)
row.append(99)
print(g, rebind(base), base, loop_grow([1, 5]), default_list(1), default_list(2))
''', ["fresh_rows", "rebind", "touch_both", "default_list"], ["keep", "store_twice", "first_row", "loop_grow"]),

    ("tipos_dinamicos", '''
def half(x):
    return x / 2

def add(a, b):
    return a + b

def maybe(x):
    if x > 0:
        return x
    return None

def mixed(n):
    total = 0
    for i in range(n):
        total += i * 0.5
    return total

def last_even(xs):
    for x in xs:
        if x % 2 == 0:
            found = x
    return found

def first_positive(xs):
    result = None
    for x in xs:
        if x > 0:
            result = x
            break
    return result

def truthy(a, b):
    return a or b

def flag(x):
    return x and x > 2

print(half(3), half(4.0), half(True), add(1, 2), add("a", "b"), add([1], [2]), add(1.5, 2))
print(maybe(3), maybe(-3), mixed(0), mixed(4), last_even([1, 2, 3, 4]), first_positive([-1, 5]), first_positive([]))
print(truthy(0, 7), truthy(3, 7), flag(0), flag(5))
try:
    print(last_even([1, 3]))
except UnboundLocalError as error:
    print("error", type(error).__name__)
try:
    print(add(1, "x"))
except TypeError as error:
    print("error", type(error).__name__)
''', ["half", "add", "mixed", "last_even"], []),

    ("globales", '''
import math
from math import sqrt, pi

LIMIT = 10
SCALE = 2.5
NAME = "turi"
BIG = 10 ** 9 + 7
counter = 0
cache = {}

def bounded(x):
    return min(x, LIMIT) * SCALE

def circle(r):
    return pi * r * r, 2 * math.pi * r, sqrt(r)

def tag(s):
    return NAME + ":" + s

def modmul(a, b):
    return a * b % BIG

def bump():
    global counter
    counter += 1
    return counter

def remember(k):
    if k not in cache:
        cache[k] = k * k
    return cache[k]

def len(x):
    return 42

def size(xs):
    return len(xs)

print(bounded(4), bounded(40), tag("x"), modmul(123456789, 987654321), bump(), bump(), remember(3))
print("%.6f %.6f %.6f" % circle(2.0), size([1, 2, 3]))
LIMIT = 3
print(bounded(4))
''', ["tag", "modmul", "circle", "size"], ["bump", "remember"]),

    ("argumentos", '''
def area(w, h=2, scale=1.0):
    return w * h * scale

def join(words, sep=", "):
    return sep.join(words)

def type(x):
    return x + 1

def shadow(list, len, id):
    return list + len + id

def window(xs, lo=0, hi=None):
    if hi is None:
        hi = len(xs)
    return xs[lo:hi]

print(area(3), area(3, 4), area(3, h=5), area(w=2, scale=1.5), join(["a", "b"]), join(["a", "b"], sep="-"))
print(type(1), shadow(1, 2, 3), window([1, 2, 3, 4]), window([1, 2, 3, 4], 1, 3), window([1, 2, 3, 4], hi=-1))
try:
    print(area())
except TypeError as error:
    print("error TypeError")
''', ["area", "join", "type", "shadow", "window"], []),

    ("error_sin_capturar", '''
import sys

def checked_div(a, b):
    return a // b

def run(values):
    total = 0
    for v in values:
        total += checked_div(100, v)
    return total

print(run([1, 2, 5]))
sys.stdout.flush()
print(run([4, 0, 1]))
''', ["checked_div", "run"], []),

    ("salida_con_codigo", '''
import sys

def pick(xs, i):
    return xs[i]

print(pick([10, 20, 30], -1), pick("abc", 1))
if pick([1, 2], 1) == 2:
    sys.exit(3)
''', ["pick"], []),

    ("recursion", '''
import sys

def depth(n):
    if n == 0:
        return 0
    return 1 + depth(n - 1)

def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)

def is_even(n):
    return True if n == 0 else is_odd(n - 1)

def is_odd(n):
    return False if n == 0 else is_even(n - 1)

def hanoi(n, a, b, c, moves):
    if n == 0:
        return
    hanoi(n - 1, a, c, b, moves)
    moves.append(a * 10 + c)
    hanoi(n - 1, b, a, c, moves)

def walk(tree, total):
    for item in tree:
        if isinstance(item, list):
            total = walk(item, total)
        else:
            total += item
    return total

print(depth(50), depth(200), depth(900), depth(950), depth(120), depth(60))
print(fib(27), fib(10), is_even(100), is_odd(7), walk([1, [2, [3, 4]], 5], 0))
moves = []
hanoi(10, 1, 2, 3, moves)
print(len(moves), moves[:5])
try:
    depth(5000)
except RecursionError:
    print("demasiado profundo")
print(depth(300))
''', ["fib", "is_even", "hanoi"], ["walk"]),

    ("hilos", '''
import sys
import threading

sys.setswitchinterval(0.00001)

def work(n):
    total = 0
    for i in range(n):
        total += i * i % 7
    return total

def fill(xs, k):
    for i in range(len(xs)):
        xs[i] = xs[i] + k

print(work(1000), work(5))
results = [0] * 8
data = [[i] * 50 for i in range(8)]

def run(slot):
    for _ in range(200):
        results[slot] = work(2000 + slot)
        fill(data[slot], 1)

threads = [threading.Thread(target=run, args=(i,)) for i in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()
print(results, [row[0] for row in data], work(10))
''', ["work"], []),

    ("busquedas", '''
def binary_search(xs, target):
    lo, hi = 0, len(xs) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if xs[mid] == target:
            return mid
        if xs[mid] < target:
            lo = mid + 1
        else:
            hi = mid - 1
    return -1

def primes(n):
    sieve = [True] * (n + 1)
    out = []
    for i in range(2, n + 1):
        if sieve[i]:
            out.append(i)
            for j in range(i * i, n + 1, i):
                sieve[j] = False
    return out

def count_inversions(xs):
    count = 0
    for i in range(len(xs)):
        for j in range(i + 1, len(xs)):
            if xs[i] > xs[j]:
                count += 1
    return count

def longest_run(s):
    best = 0
    run = 0
    prev = ""
    for c in s:
        run = run + 1 if c == prev else 1
        prev = c
        best = max(best, run)
    return best

def countdown(n, step):
    return [i for i in range(n, 0, -step)]

print(binary_search([1, 3, 5, 7, 9], 7), binary_search([1, 3, 5], 4), binary_search([], 1))
print(primes(50), count_inversions([3, 1, 2, 5, 4]), longest_run("aabbbbc"), longest_run(""))
print(countdown(10, 3), countdown(3, 5))
try:
    print(countdown(3, 0))
except ValueError as error:
    print("error", error)
''', ["binary_search", "primes", "count_inversions", "longest_run", "countdown"], []),

    ("salida", '''
import contextlib
import io
import sys

def report(xs, label):
    total = 0
    for i, x in enumerate(xs):
        total += x
        if i % 3 == 0:
            print(f"{label}[{i}] = {x:>6} total={total:,} hex={x:#x} {x:08.2f}")
    print("media:", total / len(xs), "max", max(xs), sep=" | ")
    average = "%.3f" % (total / len(xs))
    print(average, "%.2f" % (total / len(xs)))
    print("lista", xs[:3], (1, 2.5), [[1.5], []], ("a'b", 'c"d', "e"), "fin", end="!\\n")
    print("pct %5.1f%% de %d y %s %r" % (12.345, 7, "t", "q"), file=sys.stderr)
    print("{} y {:08.3f} y {!r} y {name:^9}".format(len(xs), 3.14159, "a'b", name="ok"))
    sys.stdout.write("directo\\n")
    return total

def floats(xs):
    for x in xs:
        print(x, repr(x), str(x), f"{x:.3e}|{x:g}|{x:.4}|{x:+.1%}|{x:,.2f}")

def risky(n):
    print("antes", n)
    big = 1
    for i in range(n):
        big = big * 1000
        print("vuelta", i)
    print("despues", big)
    return big

def chatty(n):
    for i in range(n):
        print(i, end=" ")
    print()
    return n

def first(xs):
    print("primero de", len(xs))
    return xs[0]

ys = [i * i - 7 for i in range(12)]
for _ in range(3):
    print(report(ys, "y"))
floats([0.1 + 0.2, 1e16, 1e-5, -0.0, 123456789.125, 1.5e300, 2.0 ** -1074, float("inf"), 0.0001])
print(risky(3))
print(risky(9))
print(risky(2))
print(chatty(4), chatty(0))
buffer = io.StringIO()
with contextlib.redirect_stdout(buffer):
    chatty(3)
    report([1, 2, 3, 4], "r")
print("capturado:", repr(buffer.getvalue()))
try:
    first([])
except IndexError as error:
    print("error", error)
print(first([5, 6]))
''', ["report", "floats", "risky", "chatty", "first"], []),
    ("azar_y_errores", '''
import random

def simulate(rng, n):
    hits = 0
    total = 0.0
    picks = [0] * 6
    for _ in range(n):
        x = rng.random()
        y = rng.random()
        if x * x + y * y <= 1.0:
            hits += 1
        total += rng.uniform(-2, 5)
        picks[rng.randint(0, 5)] += 1
    return hits, total, picks

def deal(rng, cards):
    rng.shuffle(cards)
    hand = [rng.choice(cards) for _ in range(5)]
    return cards[:7], hand, rng.randrange(10, 100, 7), rng.getrandbits(40)

def global_rolls(n):
    s = 0
    for _ in range(n):
        s += random.randint(1, 6) + random.randrange(3)
    return s

def safe_div(xs, d):
    out = []
    for x in xs:
        try:
            out.append(x // d)
        except ZeroDivisionError:
            out.append(-1)
    return out

def parse_all(texts):
    total = 0
    bad = 0
    for t in texts:
        try:
            total += int(t)
        except ValueError:
            bad += 1
    return total, bad

rng = random.Random(12345)
print(simulate(rng, 200000))
print(deal(rng, list(range(52))))
random.seed(7)
print(global_rolls(100000), random.random())
print(simulate(random.Random(99), 1000), rng.random())
print(safe_div([10, 20, 35], 3), safe_div([1, 2], 0))
print(parse_all(["1", "22", "x", "4"]), parse_all(["5", "6"]))
for _ in range(3):
    print(global_rolls(10), deal(random.Random(_), list(range(10)))[1])
''', ["simulate", "global_rolls", "safe_div", "parse_all"], []),
    ("none", '''
def find(xs, target):
    for i, x in enumerate(xs):
        if x == target:
            return i
    return None

def first_negative(xs):
    for x in xs:
        if x < 0:
            return x

def smallest(xs):
    best = None
    for x in xs:
        if best is None or x < best:
            best = x
    return best

def count_found(xs, targets):
    hits = 0
    for t in targets:
        pos = find(xs, t)
        if pos is not None:
            hits += pos
    return hits

def span(xs, lo=0, hi=None):
    if hi is None:
        hi = len(xs)
    total = 0
    for i in range(lo, hi):
        total += xs[i]
    return total

def describe(xs):
    m = smallest(xs)
    print("minimo:", m, "posicion de 7:", find(xs, 7))
    return m

data = [5, 3, 9, -2, 7, 1]
print(find(data, 7), find(data, 42), first_negative(data), first_negative([1, 2]))
print(smallest(data), smallest([]), count_found(data, [7, 9, 100]))
print(span(data), span(data, 2), span(data, 1, 4))
print(describe(data), describe([]))
for _ in range(4):
    print(find(list(range(1000)), 999), smallest(list(range(50, 0, -1))), span(list(range(100)), 10, None))
''', ["find", "first_negative", "smallest", "count_found", "span", "describe"], []),
    ("clases", '''
class Account:
    def __init__(self, number, balance):
        self.number = number
        self.balance = balance
        self.history = []

    def deposit(self, amount):
        self.balance += amount
        self.history.append(amount)

    def withdraw(self, amount):
        if amount > self.balance:
            return False
        self.balance -= amount
        self.history.append(-amount)
        return True


class Node:
    def __init__(self, key):
        self.key = key
        self.left = None
        self.right = None


def insert(root, key):
    if root is None:
        return Node(key)
    if key < root.key:
        root.left = insert(root.left, key)
    else:
        root.right = insert(root.right, key)
    return root


def height(node):
    if node is None:
        return 0
    return 1 + max(height(node.left), height(node.right))


def total(accounts):
    s = 0.0
    for a in accounts:
        s += a.balance
    return s


def simulate(accounts, n):
    fails = 0
    for i in range(n):
        src = accounts[(i * 7) % len(accounts)]
        dst = accounts[(i * 13 + 5) % len(accounts)]
        amount = float((i * 37) % 500)
        if src.withdraw(amount):
            dst.deposit(amount)
        else:
            fails += 1
    return fails


accounts = [Account(i, 1000.0) for i in range(10)]
print(simulate(accounts, 20000), round(total(accounts), 2))
print([len(a.history) for a in accounts][:5], accounts[3].balance)
root = None
seed = 7
for i in range(2000):
    seed = (seed * 1103515245 + 12345) % 2147483648
    root = insert(root, seed % 100000)
print(height(root), root.key, root.left.key if root.left else None)
''', ["height", "total", "simulate"], []),

    ("orden_y_constantes", '''
import contextlib
import io

WEIGHTS = [-2, 4, 6]
LABELS = ("uno", "dos")
MUTABLE = [3, 4]
OUTPUT_VALUES = [4, 8]

class MutatingStream(io.StringIO):
    def write(self, text):
        globals()["OUTPUT_VALUES"][0] = 99
        return super().write(text)

def announce():
    print(OUTPUT_VALUES[0])
    return OUTPUT_VALUES[0]

def from_constants(i):
    acc = len(WEIGHTS)
    for value in WEIGHTS:
        acc += value
    labels = [label for label in LABELS]
    if i in WEIGHTS:
        acc += len(labels)
    return WEIGHTS[i] + acc

def stable(rows, direction):
    if direction:
        return sorted(rows, key=lambda p: p[0], reverse=True)
    return sorted(rows, key=lambda p: p[0])

def ranked():
    d = {"z": 2, "a": 2, "m": 3}
    ordered = sorted(d.items(), key=lambda p: (-p[1], p[0]))
    return [pair[0] for pair in ordered]

def tied():
    d = {"z": 2, "a": 2, "m": 3}
    return sorted(d.items(), key=lambda p: -p[1])

def mutable_first():
    return MUTABLE[0]

rows = [(1, 10), (1, 20), (0, 30), (2, 40)]
print(from_constants(-2), from_constants(0))
print(stable(rows, False), stable(rows, True))
print(ranked(), tied())
globals()["WEIGHTS"][0] = 20
print(from_constants(0))
MUTABLE[0] = 9
print(mutable_first())
assert announce() == 4
with contextlib.redirect_stdout(MutatingStream()):
    result = announce()
print(result)
''', ["from_constants", "stable", "ranked", "announce"], ["tied", "mutable_first"]),

    ("monticulos", '''
import heapq

def mutate(items, priority):
    heapq.heapify(items)
    heapq.heappush(items, (priority, 9))
    distance, node = heapq.heappop(items)
    return distance * 10 + node

def drain(n):
    heap = []
    for i in range(n):
        heapq.heappush(heap, (n - i, i))
    out = []
    while heap:
        distance, node = heapq.heappop(heap)
        out.append(distance + 100 * node)
    return out

def pop_one(items):
    return heapq.heappop(items)

items = [(4, 3), (1, 3), (1, 1), (0, 1), (1, 1)]
print(mutate(items, 1), items)
print(drain(0), drain(8))
nums = [1, 3, 5]
print(pop_one(nums), nums)
try:
    pop_one([])
except IndexError as error:
    print(type(error).__name__)
saved = heapq.heappop
heapq.heappop = lambda heap: heap.pop()
nums = [1, 3, 5]
print(pop_one(nums), nums)
heapq.heappop = saved
''', ["mutate", "drain", "pop_one"], []),

    ("claves_tupla", '''
def counts_at(a, b):
    active = {(a, b), (-2147483648, 2147483647)}
    totals = {(a, b): 4}
    rounds = 0
    for point in active:
        totals[point] = totals.get(point, 0) + 3
        rounds += 1
    filtered = {cell for cell, count in totals.items() if count >= 3 and cell in active}
    filtered.discard((a - 1, b))
    filtered.remove((a, b))
    triples = {(a, 0, b): len(filtered) + 7}
    return rounds, totals[(a, b)], triples[(a, 0, b)], len(filtered)

def visible_order(n):
    points = {(n, -2), (0, 8), (-3, 2)}
    seen = []
    for point in points:
        seen.append(point[0])
    return seen

print(counts_at(2, -5), counts_at(-3, 0))
print(counts_at(1048576, 0), counts_at(2147483648, 1))
print(visible_order(7))
''', ["counts_at"], ["visible_order"]),

    ("cierres", '''
def count(n):
    hits = 0
    selected = set()

    def place(row):
        nonlocal hits
        if row == n:
            hits += 1
            return
        for value in range(n):
            if value in selected:
                continue
            selected.add(value)
            place(row + 1)
            selected.remove(value)

    place(0)
    return hits


def change(xs):
    total = 0

    def add(i):
        nonlocal total
        xs[i] += i
        total += xs[i]

    for i in range(len(xs)):
        add(i)
    return total


def escaped(n):
    def read(i):
        return n + i
    return read


def depth(n):
    base = 1

    def visit(step):
        if step == 0:
            return base
        return 1 + visit(step - 1)

    return visit(n)


print(count(4), count(5))
xs = [1, 2, 3, 4]
print(change(xs), xs)
print(escaped(3)(4), depth(8))
import sys
limit = sys.getrecursionlimit()
sys.setrecursionlimit(80)
try:
    depth(120)
except RecursionError:
    print("límite de CPython")
finally:
    sys.setrecursionlimit(limit)
''', ["count", "change", "depth"], ["escaped"]),

    ("memoria_recursiva", '''
import functools
from functools import lru_cache
import sys


def evaluate(depth):
    length = depth

    @lru_cache(maxsize=None)
    def recur(i, salt):
        if i == length:
            return salt + 1
        if i + 1 == length:
            return salt + 2
        return (recur(i + 1, salt) + recur(i + 2, salt)) % 1021

    return recur(0, 4)


def impure(n):
    visits = []

    @lru_cache(maxsize=None)
    def call(i, unused):
        visits.append(i)
        return 0 if i == n else 1 + call(i + 1, unused)

    return call(0, 0) + len(visits)


print(evaluate(8), evaluate(300), impure(4))
limit = sys.getrecursionlimit()
sys.setrecursionlimit(80)
try:
    evaluate(200)
except RecursionError:
    print("límite de CPython")
finally:
    sys.setrecursionlimit(limit)

standard = functools.update_wrapper
functools.update_wrapper = lambda wrapper, fn: lambda i, salt: 91
try:
    print(evaluate(8))
finally:
    functools.update_wrapper = standard

standard_code = lru_cache.__code__


substitute = lambda maxsize=128, typed=False: lambda fn: lambda i, salt: 77
lru_cache.__code__ = substitute.__code__
try:
    print(evaluate(8))
finally:
    lru_cache.__code__ = standard_code
print(evaluate(11))
''', ["evaluate"], ["impure"]),
    ("mochila_01", '''
from functools import lru_cache
import sys


def choose(capacity, values, weights):
    length = len(weights)

    @lru_cache(maxsize=None)
    def visit(row, remaining):
        if row == length:
            return 0
        skip = visit(row + 1, remaining)
        if weights[row] <= remaining:
            return max(skip, values[row] + visit(row + 1, remaining - weights[row]))
        return skip

    return visit(0, capacity)


w, v = [8, 8, 5, 9, 7, 4, 3], [21, 23, 9, 27, 17, 8, 8]
for cap in (11, 0, 7, 16, 30, 90, -1):
    print(choose(cap, v, w))
print(choose(5, [1, 7, 5], [0, 0, 5]))
print(choose(6, [1, -2, 7], [-1, 2, 7]))
print(choose(3, [2 ** 31, 1], [2, 1]))
print(choose(0, [], []))
try:
    choose(5, [12], [4, 2])
except IndexError:
    print("valores insuficientes")

limit = sys.getrecursionlimit()
sys.setrecursionlimit(80)
try:
    choose(60, [7] * 150, [2] * 150)
except RecursionError:
    print("límite de CPython")
finally:
    sys.setrecursionlimit(limit)
''', ["choose"], []),
    ("reinas_mascaras", '''
def count_boards(side):
    total = 0
    vertical = set()
    difference = set()
    addition = set()

    def next_row(x):
        nonlocal total
        if x == side:
            total += 1
            return
        for y in range(side):
            if y in vertical or (x - y) in difference or (x + y) in addition:
                continue
            vertical.add(y)
            difference.add(x - y)
            addition.add(x + y)
            next_row(x + 1)
            vertical.remove(y)
            difference.remove(x - y)
            addition.remove(x + y)

    next_row(0)
    return total


print(count_boards(8), count_boards(7))
for size in (7, 8, -9, 0, 1, 2, 3, 4, 5, 6, 9, 10, 11):
    print(count_boards(size))

range = lambda size: [0]
print(count_boards(5))
del range
print(count_boards(6))
''', ["count_boards"], []),
    ("lectura_archivos", r'''
import builtins
import io
import os
import sys
from pathlib import Path
from tempfile import TemporaryDirectory


def summarize(path, divisor):
    lines = chars = 0
    longest = ""
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            lines += 1
            chars += len(line) // divisor
            value = line.rstrip("\n").strip("!?")
            if len(value) > len(longest):
                longest = value
    return lines, chars, longest


def tokens(text, limit):
    parts = []
    word = "-"
    for word in text.split():
        if word == "paso":
            continue
        parts.append(word)
        if len(parts) == limit:
            break
    return parts, word


print(tokens("   a\tpaso b\n c  ", 2), tokens(" ", 1), tokens("árbol\vfruta", 4))

with TemporaryDirectory() as folder:
    path = Path(folder) / "text.txt"
    path.write_bytes(b"!ab!\r\n\r!c!?\rlast")
    print(summarize(str(path), 1))
    for _ in range(24):
        assert summarize(str(path), 1) == (4, 15, "last")
    path.write_bytes(b"!a\n" + b"b" * 65532 + b"\r\nend\r")
    lines, chars, longest = summarize(str(path), 1)
    print(lines, chars, len(longest), longest[:1], longest[-1:])
    path.write_bytes(b"c" * 65536 + b"\nnext")
    lines, chars, longest = summarize(str(path), 1)
    print(lines, chars, len(longest), longest[:1], longest[-1:])
    path.write_bytes(b"!ab!\r\n\r!c!?\rlast")
    try:
        summarize(str(path), 0)
    except ZeroDivisionError:
        print("fallo durante la lectura")
    print(summarize(str(path), 1))
    path.write_bytes(b"")
    print(summarize(str(path), 1), summarize(os.devnull, 1))
    path.write_bytes("árbol\r\n".encode("utf-8"))
    print(summarize(str(path), 1))
    path.write_bytes(b"\xff")
    try:
        summarize(str(path), 1)
    except UnicodeDecodeError:
        print("UTF-8 inválido")
    try:
        summarize(str(path.with_suffix(".missing")), 1)
    except FileNotFoundError:
        print("archivo ausente")
    standard_open = builtins.open
    builtins.open = lambda *args, **kwargs: io.StringIO("?alternativo!?\n")
    try:
        print(summarize(str(path), 1))
    finally:
        builtins.open = standard_open
    path.write_bytes(b"z\n")
    print(summarize(str(path), 1))
    tracked = str(path)
    openings = []

    def audit(event, args):
        if event == "open" and args[0] == tracked:
            openings.append(args[1])

    getattr(sys, "add" + "audithook")(audit)
    print(summarize(tracked, 1), openings)
''', ["tokens", "summarize"] if NATIVE_FILE_READ else ["tokens"], []),
    ("constructor_directo", '''
class Point:
    def __init__(self, x, y):
        self.y = y
        self.x = x


class Link:
    def __init__(self, left, right):
        self.left = left
        self.right = right


def run(n):
    first = Point(n + 4, n)
    second = Point(n, 3 * n)
    pair = Link(first, second)
    first.x += 1
    return pair.left.x, pair.left.y, pair.right.x, pair.right.y


print(run(4), run(-12), run(0))


def redirect(pair, incoming):
    previous = pair.left
    pair.left = incoming
    return previous


left = Point(1, 2)
right = Point(3, 4)
link = Link(left, right)
print(redirect(link, right) is left, link.left is right)
print(redirect(link, None) is right, link.left is None)
print(redirect(link, left) is None, link.left is left)
''', ["run", "redirect"], []),

    ("reduccion_filas", '''
def weight(i, j):
    return 0.125 * (i * 3 + j + 2)


def weighted(source, dest, rows, cols):
    """Acumula un producto matriz-vector rectangular."""
    for row in range(rows):
        total = 0.0
        for col in range(cols):
            total += weight(col, row) * source[col]
        dest[row] = total


def via_helper(rows):
    source = [-2.25, 7.0, 0.125, -3.0]
    dest = [-0.0] * 12
    weighted(source, dest, rows, 4)
    return dest


def singular(i, j):
    return 0.5 * (4 // (i - 3))


def may_trap(source, dest, rows, cols):
    for row in range(rows):
        total = 0.0
        for col in range(cols):
            total += singular(row, col) * source[col]
        dest[row] = total


print(via_helper(-1))
print(via_helper(0))
print(via_helper(3))
print(via_helper(8))
print(via_helper(9))
print(via_helper(12))

source = [2.0, -3.0, 0.125]
dest = [-0.0] * 9
weighted(source, dest, 9, 0)
print(dest)

alias = [2.0, -3.0, 0.125] * 3
weighted(alias, alias, 9, 3)
print(alias)

for action in (weighted, may_trap):
    dest = [99.0] * 9
    try:
        action(source, dest, 9, 4 if action is weighted else 2)
    except (IndexError, ZeroDivisionError) as error:
        print(type(error).__name__, dest)
''', ["via_helper", "weighted"], []),

    ("reciprocos_pares", '''
import struct


def coefficient(col, row):
    return 1.0 / (5 + 2 * row + 3 * col + ((row + col) * (row + col - 1)) // 2)


def weighted(source, dest, rows, cols):
    for row in range(rows):
        total = 0.0
        for col in range(cols):
            total += coefficient(col, row) * source[col]
        dest[row] = total


def staircase(row, col):
    return 1.0 / (1 + (row + col) // 2)


def stepped(source, dest, rows, cols):
    for row in range(rows):
        total = 0.0
        for col in range(cols):
            total += staircase(row, col) * source[col]
        dest[row] = total


for rows, cols in ((31, 9), (32, 9), (48, 9), (56, 9), (63, 9), (65, 9), (33, 0)):
    source = [0.0] * 9
    source[rows % 9] = 1.0
    dest = [-0.0] * rows
    weighted(source, dest, rows, cols)
    print(struct.pack(f"!{rows}d", *dest).hex())

dest = [0.0] * 33
stepped(source, dest, len(dest), len(source))
print(struct.pack("!33d", *dest).hex())

source = [2.0, -3.0, -0.0, 0.125, 2.0 ** -1074, 1.25, -5.0, 2.0, -25.0]
shared = source * 4
weighted(shared, shared, 32, 9)
print([round(value, 12) for value in shared])

for rows, cols in ((35, 10), (37, 9)):
    dest = [-1.0] * 35
    try:
        weighted(source, dest, rows, cols)
    except IndexError:
        print([round(value, 12) for value in dest])
''', ["weighted", "stepped"], []),

    ("producto_rectangular", '''
def product(right, left):
    rows = len(left)
    cols = len(right[0])
    inner = len(right)
    table = [[0.0] * cols for _ in range(rows)]
    for row in range(rows):
        for col in range(cols):
            cell = 0.0
            for k in range(inner):
                cell += left[row][k] * right[k][col]
            table[row][col] = cell
    return table


right = [[2.0, 0.5, 0.0, -3.0],
         [-5.0, 1.0, 2.25, -0.5],
         [1.0, 6.0, 1.5, 8.0]]
left = [[3.0, -0.25, 2.0], [0.5, -0.0, -3.25]]
print(product(right, left))
print(product(right, left))
print(product([[0.25, -0.0]], [[-1.0], [1.0]]))
print(product([[]], [[-1.0], []]))
shared = [[1.25, 2.0], [-0.5, 3.0]]
print(product(shared, shared), shared)
try:
    product([[1.0, 2.0], [0.125]], [[-1.0, 1.0]])
except IndexError:
    print("fila corta")

repeated_left = [[(i % 3 - k) / 8.0 for k in range(9)] for i in range(67)]
repeated_right = [[((j % 11) * k - 17) / 32.0 for j in range(71)] for k in range(9)]
result = product(repeated_right, repeated_left)
print(result[0][-1], result[1][32], result[-1][-1])
result[0][0] = 42.0
print(result[3][0], repeated_left[0][0])
repeated_right[4][-1] += 1.0
changed = product(repeated_right, repeated_left)
print(changed[0][-1], changed[1][32], changed[-1][-1])

len = lambda value: 0
print(product(repeated_right, repeated_left))
del len
print(product(right, left))
''', ["product"], []),
]

ENV_BASE = dict(os.environ)
ENV_BASE.pop("TURIPY_VERIFY", None)
ENV_BASE.pop("TURIPY_FLOAT", None)
ENV_BASE["PYTHONIOENCODING"] = "utf-8"


def run(command, env):
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                          env=env, timeout=1800)


def cache_del_motor(folder):
    base = Path(folder)
    first, second = base / "motor_a", base / "motor_b"
    for checkout in (first, second):
        checkout.mkdir()
        shutil.copyfile(LAUNCHER, checkout / "run.py")
    engine = first / "turipy.py"
    engine.write_text("def main(args):\n    print(main.__code__.co_filename == __file__)\n", encoding="utf-8")
    shutil.copy2(engine, second / engine.name)
    env = dict(ENV_BASE, TURIPY_CACHE=str(base / "motor_cache"))
    for checkout in (first, second, first, second):
        result = run([sys.executable, str(checkout / "run.py")], env)
        if result.returncode or result.stdout.strip() != "True":
            print(f"FALLA cache_del_motor en {checkout.name}: {result.stdout}{result.stderr}")
            return False
    print("ok cache_del_motor: trazas correctas en dos copias")
    return True


def ejecutable_independiente(folder):
    source = Path(folder) / "programa independiente.py"
    source.write_text('''\
import sys

def main():
    n = int(sys.argv[1])
    print(sys.argv[0])
    print(f"{n:0600d}")
    value = float(n) * 1048576.0 + 0.125
    print(f"{value:,.6f} {value:.9f} {value * 2097152.0:.6f}")
    print("estado", file=sys.stderr)
    if n != 8:
        print("x" * 9000)
    if n == -1:
        with open(sys.argv[2], encoding="utf-8") as handle:
            for line in handle:
                print(line.rstrip("\\n"))
        return
    print(100 // n, sys.argv[2])

if __name__ == "__main__":
    main()
''', encoding="utf-8")
    binary = Path(folder) / ("programa nativo ñ.exe" if os.name == "nt" else "programa_bin")
    plain = Path(folder) / "texto ascii.txt"
    unicode = Path(folder) / "texto unicode.txt"
    plain.write_bytes(b"primera\r\nsegunda\rtercera\n")
    unicode.write_bytes("línea en UTF-8\n".encode("utf-8"))
    trap_case = (["0", "cero"], False)
    cases = [(["7", 'bytes con espacios "y comillas"\\'], True), (["8", "ocho"], True), (["7", "árbol"], False),
             trap_case, (["-1", str(plain)], True), (["-1", str(unicode)], False)]
    # Una grafía alternativa prueba la ruta aun en volúmenes sin nombres cortos.
    upper = source.with_name(source.name.upper())
    invocation_source = upper if os.name == "nt" and upper.exists() else source
    expected = [subprocess.run([sys.executable, str(invocation_source), *args], capture_output=True, env=ENV_BASE, timeout=45)
                for args, _ in cases]
    build = run([sys.executable, str(LAUNCHER), "--veloz", str(Path(CLI).resolve()),
                 "--exe", str(binary), str(invocation_source)], ENV_BASE)
    if build.returncode or not binary.is_file():
        print("FALLA ejecutable independiente al compilar", build.stderr)
        return False
    source.unlink()
    for (args, native), reference in zip(cases, expected):
        result = subprocess.run([str(binary), *args], capture_output=True, timeout=45,
                                env=dict(ENV_BASE, TURIPY_EXE_TIME="1"))
        if result.returncode != reference.returncode or result.stdout != reference.stdout:
            print("FALLA ejecutable independiente en la salida o código", args, result.stderr)
            return False
        if native and not result.stderr.startswith(reference.stderr + b"TURI_EXE_MS="):
            print("FALLA ejecutable independiente sin tiempo nativo", result.stderr)
            return False
        if not native and result.stderr != reference.stderr:
            print("FALLA ejecutable independiente al recuperar", args, result.stderr, reference.stderr)
            return False
    source.write_text("raise AssertionError('archivo sustituido')\n", encoding="utf-8")
    reference = expected[cases.index(trap_case)]
    replayed = subprocess.run([str(binary), *trap_case[0]], capture_output=True, env=ENV_BASE, timeout=45)
    if (replayed.returncode, replayed.stdout, replayed.stderr) != (reference.returncode, reference.stdout, reference.stderr):
        print("FALLA ejecutable independiente con el archivo sustituido", replayed.stderr, reference.stderr)
        return False
    print("ok ejecutable independiente: salida en nativo y recuperación con el fuente incluido")
    return True


def ejecutable_contadores(folder):
    cases = (
        (ROOT / "benchmarks/python/palabras/palabras.py",
         b"seed * 1103515245 + 12345", b"seed * 1103515245 + 12347", (0, 1, 7, 47, 48, 49, 127, 128, 129, 233, 4117)),
        (ROOT / "benchmarks/python/criba/criba.py",
         b"is_prime[0] = False", b"is_prime[0] = True", (2, 3, 7, 8, 49, 50, 51, 121, 122, 210, 211, 1330, 1331, 1332, 4531)),
        (ROOT / "tools/pyfront/examples/fannkuch.py",
         b"checksum += flips", b"checksum += flips + 1", (1, 3, 4, 5, 6, 7, 8)),
        (ROOT / "benchmarks/python/dijkstra/dijkstra.py",
         b"ni * 7 + nj * 13", b"ni * 7 + nj * 12", (1, 2, 9, 10, 11, 43)),
        (ROOT / "benchmarks/python/vida/vida.py",
         b"if c == 3 or", b"if c == 4 or", (-9, 0, 1, 10, 11, 64, 449, 511)),
        (ROOT / "tools/pyfront/examples/mandelbrot.py",
         b"range(50)", b"range(51)", (1, 7, 8, 9, 31, 32, 33, 64, 73, 117)),
        (ROOT / "tools/pyfront/examples/spectralnorm.py",
         b"%.9f", b"%.8f", (1, 9, 31, 32, 33, 64, 87, 173, 513)),
        (ROOT / "tools/pyfront/examples/nbody.py",
         b"dt / (d2 * sqrt(d2))", b"dt / (2.0 * d2 * sqrt(d2))", (0, 1, 17, 257, 4099)),
        (ROOT / "benchmarks/python/cuerpos/cuerpos.py",
         b"dt / (d2 * sqrt(d2))", b"dt / (2.0 * d2 * sqrt(d2))", (0, 1, 17, 257, 4099)),
        (ROOT / "benchmarks/python/matmul/matmul.py",
         b"% 97) / 97.0", b"% 37) / 37.0", (1, 7, 8, 31, 64, 96, 97, 98, 147, 148, 159, 163, 164, 193, 194)),
        (ROOT / "benchmarks/python/arboles/arboles.py",
         b"1 + check(node.left)", b"2 + check(node.left)", (-1, 6, 7, 9, 11)),
        (ROOT / "benchmarks/python/mochila/mochila.py",
         b"values.append(10", b"values.append(11", (-1, 0, 6, 23, 63, 129, 400)),
    )
    for source, fragment, replacement, sizes in cases:
        name = source.stem
        data = source.read_bytes()
        altered = data.replace(fragment, replacement, 1)
        versions = [("original", data, sizes), ("alterado", altered, sizes)]
        if name == "matmul":
            precise = altered.replace(b"{total:.6f}", b"{total:.12f}", 1)
            versions.append(("precision", precise, (146, 148, 159, 164)))
        for suffix, program, inputs in versions:
            path = Path(folder) / f"{name}_{suffix}.py"
            path.write_bytes(program)
            binary = path.with_suffix(".exe" if os.name == "nt" else ".bin")
            build = run([sys.executable, str(LAUNCHER), "--veloz", str(Path(CLI).resolve()),
                         "--exe", str(binary), str(path)], ENV_BASE)
            if build.returncode or not binary.is_file():
                print("FALLA ejecutable de contadores", name, suffix, build.stderr)
                return False
            for n in inputs:
                expected = subprocess.check_output([sys.executable, str(path), str(n)], env=ENV_BASE, timeout=45)
                result = subprocess.run([str(binary), str(n)], capture_output=True, timeout=45,
                                        env=dict(ENV_BASE, TURIPY_EXE_TIME="1"))
                if result.returncode or result.stdout != expected or not result.stderr.startswith(b"TURI_EXE_MS="):
                    print("FALLA contadores en nativo", name, suffix, n, result.stderr)
                    return False
    source = ROOT / "tools/pyfront/examples/nbody.py"
    program = source.read_bytes()
    advance = b"    advance(x, y, z, vx, vy, vz, mass, 0.01, steps)"
    assert program.count(advance) == 1
    for name, delta in (("colision", b""), ("subdesbordamiento", b" + 1e-200")):
        path = Path(folder) / f"nbody_{name}.py"
        positions = b"    x[1] = x[0]\n    y[1] = y[0]\n    z[1] = z[0]" + delta + b"\n"
        path.write_bytes(program.replace(advance, positions + advance, 1))
        binary = path.with_suffix(".exe" if os.name == "nt" else ".bin")
        build = run([sys.executable, str(LAUNCHER), "--veloz", str(Path(CLI).resolve()),
                     "--exe", str(binary), str(path)], ENV_BASE)
        if build.returncode or not binary.is_file():
            print("FALLA ejecutable de órbitas al compilar", name, build.stderr)
            return False
        expected = subprocess.run([sys.executable, str(path), "1"], capture_output=True, timeout=45, env=ENV_BASE)
        result = subprocess.run([str(binary), "1"], capture_output=True, timeout=45,
                                env=dict(ENV_BASE, TURIPY_EXE_TIME="1"))
        if (not expected.returncode or b"ZeroDivisionError" not in expected.stderr
                or (result.returncode, result.stdout, result.stderr) != (expected.returncode, expected.stdout, expected.stderr)):
            print("FALLA recuperación de órbitas", name, result.stderr)
            return False
    print("ok ejecutable de contadores: formas originales y alteradas coinciden con CPython")
    return True


def ejecutable_texto(folder):
    base = Path(folder)
    source = ROOT / "benchmarks/python/texto/texto.py"
    binary = base / ("texto_bin.exe" if os.name == "nt" else "texto_bin")
    build = run([sys.executable, str(LAUNCHER), "--veloz", str(Path(CLI).resolve()),
                 "--exe", str(binary), str(source)], ENV_BASE)
    if build.returncode or not binary.is_file():
        print("FALLA ejecutable de texto al compilar", build.stderr)
        return False
    vocabulary = " ".join(f"pal{i:03}" for i in range(300)).encode("ascii")
    cases = (
        (b"", True),
        (b"Ford! ford\r\nFORD\tbar\r  ,. bar bar\n", True),
        (b"One two PalabraLarga!\r\n" * 4500 + b"one two palabraLarga", True),
        (b"a" * 62 + b"\n" + b"b\n" * 35000 + b"c" * 62 + b"\n" + b"d" * 63 + b"\n" + b"e" * 63, True),
        (b"ab\n" * 21845 + b"\r\nOne\rTwo\r\nThree", True),
        (b"  x\n" * 16384, True),
        (b"  x\n" * 16384 + b"Z", True),
        (b"?! ! a!? ab.cd 0, 0! !aa b,; 1234567? 12345678! ?ab c ,!\n" * 1500, True),
        ((b"x" * 63 + b"! a a\r\n" + b"y" * 30 + b".\tB b\n") * 650, True),
        (b"city tower tower! city city, TOWER city\tTOWER\r\n" * 1600, True),
        (vocabulary + b"\n" + vocabulary.upper() + b"\nA A a", True),
        ("Vía vía\r\nárbol, árbol\n".encode("utf-8"), False),
    )
    path = base / "texto_bloques.txt"
    for data, native in cases:
        path.write_bytes(data)
        expected = subprocess.run([sys.executable, str(source), str(path)], capture_output=True,
                                  timeout=45, env=ENV_BASE)
        result = subprocess.run([str(binary), str(path)], capture_output=True, timeout=45,
                                env=dict(ENV_BASE, TURIPY_EXE_TIME="1"))
        if (result.returncode != expected.returncode or result.stdout != expected.stdout
                or (native and not result.stderr.startswith(b"TURI_EXE_MS="))
                or (not native and result.stderr != expected.stderr)):
            print("FALLA ejecutable de texto", len(data), native, result.stderr)
            return False
    print("ok ejecutable de texto: bloques, claves largas, más de 256 claves y UTF-8")
    return True


def main():
    only = sys.argv[2:]
    cache = Path(CLI).resolve().parent / "turipy-cache"
    failures = 0
    with tempfile.TemporaryDirectory(prefix="turipy-") as tmp:
        if (not only or "cache_del_motor" in only) and not cache_del_motor(tmp):
            failures += 1
        if (not only or "ejecutable_independiente" in only) and not ejecutable_independiente(tmp):
            failures += 1
        if (not only or "ejecutable_contadores" in only) and not ejecutable_contadores(tmp):
            failures += 1
        if (not only or "ejecutable_texto" in only) and not ejecutable_texto(tmp):
            failures += 1
        for name, source, native, python in CASES:
            if only and name not in only:
                continue
            path = Path(tmp) / f"{name}.py"
            path.write_text(source, encoding="utf-8")
            expected = run([sys.executable, str(path)], ENV_BASE)
            # Los dos modos con cada llamada comprobada y, además, tal como lo usa cualquiera (camino
            # directo, comprobación solo de las primeras llamadas): la primera vez y con todo ya guardado.
            for mode in (["--verificar"], ["--verificar", "--exacto"], [], []):
                report = Path(tmp) / f"{name}.json"
                env = dict(ENV_BASE, TURIPY_VELOZ=CLI, TURIPY_CACHE=str(cache), TURIPY_REPORT=str(report))
                got = run([sys.executable, str(LAUNCHER if not mode else RUNNER)] + mode + [str(path)], env)
                label = f"{name} {' '.join(mode) or 'directo'}"
                problems = []
                if got.stdout != expected.stdout:
                    problems.append(f"salida distinta\n--- Python\n{expected.stdout}--- turipy\n{got.stdout}")
                if got.returncode != expected.returncode:
                    problems.append(f"código {got.returncode}, Python {expected.returncode}")
                if got.stderr.strip() != expected.stderr.strip():
                    problems.append("errores distintos:\n" + got.stderr[-1500:])
                data = json.loads(report.read_text(encoding="utf-8")) if report.exists() else {"funciones": {}}
                if data.get("discrepancias"):
                    problems.append(f"{data['discrepancias']} resultados nativos distintos de Python")
                functions = data["funciones"]
                for fn in native:
                    info = functions.get(fn, {})
                    # Tal como se usa, una función que con datos tan pequeños va más lenta que Python se
                    # retira (regla de no empeorar): vale, si llegó a correr y comprobarse en nativo.
                    retired = info.get("llamadas_nativas") and (info.get("motivo") or "").startswith(
                        "más lento que Python") and "--verificar" not in mode
                    if (info.get("estado") != "nativa" or not info.get("llamadas_nativas")) and not retired:
                        problems.append(f"{fn} debía ser nativa: {info.get('motivo') or info.get('estado')}"
                                        + "".join(f" | {v['motivo']}" for v in info.get("variantes", []) if v["motivo"]))
                for fn in python:
                    if functions.get(fn, {}).get("llamadas_nativas"):
                        problems.append(f"{fn} no debía ejecutarse en nativo")
                if problems:
                    failures += 1
                    print(f"FALLA {label}: " + "\n  ".join(problems))
                else:
                    count = sum(1 for info in functions.values() if info.get("llamadas_nativas"))
                    print(f"ok {label}: {count} funciones nativas")
    if failures:
        sys.exit(f"{failures} casos fallan")
    print("turipy híbrido: todo coincide con CPython")


if __name__ == "__main__":
    main()
