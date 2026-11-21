"""Executable mathematical specification. All indices and values are bounded in the registered domains."""
from dataclasses import dataclass


@dataclass(frozen=True)
class Case:
    family: str
    size: int
    function: object


CASES = []


def work(family, size):
    def register(f):
        CASES.append(Case(family, size, f))
        return f
    return register


def val(i, s):
    x = (i * 73 + s * 101) % 1009
    return (x * x + 17 * x + 19) % 1009


def low(x, y):
    if x < y:
        return x
    return y


def high(x, y):
    if x > y:
        return x
    return y


def dist(x, y):
    if x >= y:
        return x - y
    return y - x


def arr(n, value):
    return [value] * n


def data(n, s):
    return [val(i, s) for i in range(n)]


def copyv(a):
    return a.copy()


@work("aritmetica", 2048)
def euclidBatch(n, s):
    total = 0
    for i in range(n):
        a = val(i, s) + 1
        b = i + 19
        while b > 0:
            r = a % b
            a = b
            b = r
        total += a
    return total


@work("aritmetica", 1200)
def primeTrial(n, s):
    total = 0
    for i in range(n):
        a = i + s + 2
        d = 2
        while d * d <= a and a % d != 0:
            d += 1
        if d * d > a:
            total += 1
    return total


@work("aritmetica", 768)
def modularPower(n, s):
    total = 0
    for i in range(n):
        e = val(i, s)
        b = i + 3
        v = 1
        while e > 0:
            if e % 2 == 1:
                v = v * b % 65521
            b = b * b % 65521
            e = e // 2
        total += v
    return total


@work("aritmetica", 1200)
def newtonRoot(n, s):
    total = 0
    for i in range(n):
        a = (i + s) * 97 + 1
        x = a
        y = (x + 1) // 2
        while y < x:
            x = y
            y = (x + a // x) // 2
        total += x
    return total


@work("aritmetica", 2400)
def digitSum(n, s):
    total = 0
    for i in range(n):
        a = (i + 1) * (s + 1)
        while a > 0:
            total += a % 10
            a = a // 10
    return total


@work("aritmetica", 1600)
def binomialRows(n, s):
    total = 0
    for i in range(n):
        a = (i + s) % 27 + 1
        v = 1
        for j in range(1, a + 1):
            v = v * (a + 1 - j) // j
            total += v
    return total


@work("aritmetica", 800)
def factorMultiplicity(n, s):
    total = 0
    for i in range(n):
        x = val(i, s) + 1
        d = 2
        while d * d <= x:
            while x % d == 0:
                total += 1
                x = x // d
            d += 1
        if x > 1:
            total += 1
    return total


@work("aritmetica", 1400)
def binaryOnes(n, s):
    total = 0
    for i in range(n):
        x = val(i, s) * 177 + i
        while x > 0:
            total += x % 2
            x = x // 2
    return total


@work("aritmetica", 1000)
def totientBatch(n, s):
    total = 0
    for i in range(n):
        x = val(i, s) + 1
        phi = x
        d = 2
        while d * d <= x:
            if x % d == 0:
                phi -= phi // d
                while x % d == 0:
                    x = x // d
            d += 1
        if x > 1:
            phi -= phi // x
        total += phi
    return total


@work("aritmetica", 1200)
def divisorsPaired(n, s):
    total = 0
    for i in range(n):
        x = val(i, s) + 1
        d = 1
        while d * d <= x:
            if x % d == 0:
                total += d
                if d * d != x:
                    total += x // d
            d += 1
    return total


@work("flujos", 16384)
def affineStream(n, s):
    x = s
    for i in range(n):
        x = (x * 48271 + 1) % 2147483647
    return x


@work("flujos", 12288)
def fibonacciMod(n, s):
    a = 0
    b = s
    for i in range(n):
        c = (a + b) % 65521
        a = b
        b = c
    return b


@work("flujos", 8192)
def adlerStream(n, s):
    a = 1
    b = 0
    for i in range(n):
        a = (a + val(i, s) % 256) % 65521
        b = (b + a) % 65521
    return b * 65536 + a


@work("flujos", 8192)
def polynomialHash(n, s):
    h = 0
    for i in range(n):
        h = (h * 257 + val(i, s)) % 1000003
    return h


@work("flujos", 16384)
def exponentialFilter(n, s):
    state = 0
    for i in range(n):
        state = (state * 7 + val(i, s) * 8) // 8
    return state


@work("flujos", 12288)
def hysteresis(n, s):
    state = 0
    transitions = 0
    for i in range(n):
        v = val(i, s)
        if state == 0 and v > 765:
            state = 1
            transitions += 1
        if state == 1 and v < 201:
            state = 0
    return transitions


@work("flujos", 12288)
def longestStreak(n, s):
    streak = 0
    best = 0
    for i in range(n):
        if val(i, s) % 3 != 0:
            streak += 1
            best = high(best, streak)
        else:
            streak = 0
    return best


@work("flujos", 12288)
def majorityVote(n, s):
    balance = 0
    candidate = 0
    for i in range(n):
        v = val(i, s) % 7
        if balance == 0:
            candidate = v
        if candidate == v:
            balance += 1
        else:
            balance -= 1
    return candidate * 100 + balance


@work("flujos", 12288)
def crossingCount(n, s):
    prev = s % 1009
    count = 0
    for i in range(n):
        x = val(i, s)
        if prev <= 500 and x > 500:
            count += 1
        prev = x
    return count


@work("flujos", 12288)
def cumulativeDrawdown(n, s):
    price = 1000
    peak = price
    draw = 0
    for i in range(n):
        price += val(i, s) % 31 - 15
        peak = high(peak, price)
        draw = high(draw, peak - price)
    return draw


@work("busqueda_orden", 180)
def insertionSort(n, s):
    a = data(n, s)
    for i in range(1, n):
        v = a[i]
        j = i
        while j > 0 and a[j - 1] > v:
            a[j] = a[j - 1]
            j -= 1
        a[j] = v
    return a[n // 3] + a[n - 1]


@work("busqueda_orden", 144)
def selectionSort(n, s):
    a = data(n, s)
    for i in range(n):
        k = i
        for j in range(i + 1, n):
            if a[j] < a[k]:
                k = j
        v = a[k]
        a[k] = a[i]
        a[i] = v
    return a[n // 2] + a[0]


@work("busqueda_orden", 160)
def bubbleEarly(n, s):
    a = data(n, s)
    edge = n
    while edge > 1:
        last = 0
        for j in range(1, edge):
            if a[j - 1] > a[j]:
                v = a[j]
                a[j] = a[j - 1]
                a[j - 1] = v
                last = j
        edge = last
    return a[n // 4] + a[n - 1]


@work("busqueda_orden", 768)
def shellSort(n, s):
    a = data(n, s)
    gap = n // 2
    while gap > 0:
        for i in range(gap, n):
            v = a[i]
            j = i
            while j >= gap and a[j - gap] > v:
                a[j] = a[j - gap]
                j -= gap
            a[j] = v
        gap = gap // 2
    return a[n // 2]


@work("busqueda_orden", 1536)
def countingRank(n, s):
    a = data(n, s)
    h = arr(1009, 0)
    for i in range(n):
        h[a[i]] += 1
    rank = n // 2
    v = 0
    while rank >= h[v]:
        rank -= h[v]
        v += 1
    return v


@work("busqueda_orden", 4096)
def lowerBound(n, s):
    a = arr(n, 0)
    for i in range(n):
        a[i] = i * 3 + i // 5
    t = 0
    for j in range(n // 4):
        x = (val(j, s) * 31) % (n * 4)
        lo = 0
        hi = n
        while lo < hi:
            mid = (lo + hi) // 2
            if a[mid] < x:
                lo = mid + 1
            else:
                hi = mid
        t += lo
    return t


@work("busqueda_orden", 768)
def sentinelFind(n, s):
    a = data(n + 1, s)
    t = 0
    for q in range(40):
        x = val(q + n, s)
        a[n] = x
        j = 0
        while a[j] != x:
            j += 1
        t += j
    return t


@work("busqueda_orden", 2048)
def partitionPivot(n, s):
    a = data(n, s)
    pivot = a[n // 2]
    left = 0
    for i in range(n):
        if a[i] < pivot:
            v = a[left]
            a[left] = a[i]
            a[i] = v
            left += 1
    return left + a[n // 2]


@work("busqueda_orden", 512)
def mergeRuns(n, s):
    a = arr(n, 0)
    b = arr(n, 0)
    x = 0
    y = 0
    t = 0
    for i in range(n):
        x += val(i, s) % 7
        y += val(i + 3, s) % 11
        a[i] = x
        b[i] = y
    l = 0
    r = 0
    for k in range(n * 2):
        if r == n or (l < n and a[l] <= b[r]):
            t += a[l] * (k % 7)
            l += 1
        else:
            t += b[r] * (k % 7)
            r += 1
    return t


@work("busqueda_orden", 768)
def heapBuild(n, s):
    a = data(n, s)
    i = n // 2
    while i > 0:
        i -= 1
        root = i
        child = 2 * root + 1
        while child < n:
            if child + 1 < n and a[child + 1] > a[child]:
                child += 1
            if a[root] >= a[child]:
                child = n
            else:
                v = a[root]
                a[root] = a[child]
                a[child] = v
                root = child
                child = 2 * root + 1
    return a[0] + a[n // 3]
