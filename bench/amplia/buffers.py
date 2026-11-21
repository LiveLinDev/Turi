from corpus import arr, copyv, data, dist, high, low, val, work


@work("memoria", 32768)
def reverseBuffer(n, s):
    a = data(n, s)
    for i in range(n // 2):
        j = n - 1 - i
        t = a[i]
        a[i] = a[j]
        a[j] = t
    total = 0
    for k in range(n):
        total += a[k] * (k % 11)
    return total


@work("memoria", 16384)
def rotateCycles(n, s):
    a = data(n, s)
    t = a[n - 1]
    for i in range(1, n):
        a[n - i] = a[n - i - 1]
    a[0] = t
    total = 0
    for j in range(n):
        total += a[j] * (j % 17)
    return total


@work("memoria", 32768)
def strideGather(n, s):
    a = data(n, s)
    t = 0
    pos = 0
    for i in range(n):
        t += a[pos]
        pos = (pos + 61) % n
    return t


@work("memoria", 16384)
def permutedScatter(n, s):
    a = arr(n, 0)
    j = s % n
    for i in range(n):
        a[j] = i + 1
        j = (j + 31) % n
    t = 0
    for k in range(n):
        t += a[k] * (k % 5)
    return t


@work("memoria", 16384)
def chainedLoads(n, s):
    a = data(n, s)
    for i in range(n):
        a[i] = (a[i] + i * 59) % n
    p = s % n
    t = 0
    for j in range(n):
        p = a[p]
        t += p
    return t


@work("memoria", 8192)
def copyIsolation(n, s):
    a = data(n, s)
    b = copyv(a)
    for i in range(n):
        if i % 3 == 0:
            b[i] = a[i] + i
    t = 0
    for j in range(n):
        t += b[j] * 3 - a[j]
    return t


@work("memoria", 380)
def scopeChurn(n, s):
    t = 0
    for i in range(n):
        width = val(i, s) % 377 + 4
        a = arr(width, i)
        for j in range(width):
            a[j] = (a[j] + j * 17) % 173
        t += a[(i + s) % width]
    return t


@work("memoria", 16384)
def compactStable(n, s):
    a = data(n, s)
    write = 0
    for i in range(n):
        if a[i] % 3 == 0:
            a[write] = a[i]
            write += 1
    t = 0
    for j in range(write):
        t += a[j] * (j % 5)
    return t


@work("memoria", 16384)
def interleaveBuffers(n, s):
    a = data(n, s)
    b = data(n, s + 1)
    c = arr(n * 2, 0)
    for i in range(n):
        c[2 * i] = a[i]
        c[2 * i + 1] = b[i]
    t = 0
    for j in range(1, n * 2):
        t += dist(c[j], c[j - 1])
    return t


@work("memoria", 32768)
def sparsePages(n, s):
    a = arr(n, s)
    for i in range((n + 511) // 512):
        a[i * 512] = val(i, s)
    t = 0
    for k in range(n):
        t += a[k]
    return t


@work("estadistica", 8192)
def dotProduct(n, s):
    a = data(n, s)
    b = data(n, s + 1)
    t = 0
    for i in range(n):
        t += a[i] * b[i]
    return t


@work("estadistica", 8192)
def integerVariance(n, s):
    a = data(n, s)
    t = 0
    for i in range(n):
        t += a[i]
    mean = t // n
    v = 0
    for j in range(n):
        d = a[j] - mean
        v += d * d
    return v


@work("estadistica", 12288)
def frequencyMode(n, s):
    a = arr(257, 0)
    for i in range(n):
        a[val(i, s) % 257] += 1
    best = 0
    for k in range(1, 257):
        if a[k] > a[best]:
            best = k
    return best


@work("estadistica", 8192)
def slidingWindow(n, s):
    a = data(n, s)
    window = 0
    best = 0
    for i in range(n):
        window += a[i]
        if i >= 23:
            window -= a[i - 23]
        best = high(best, window)
    return best


@work("estadistica", 8192)
def prefixQueries(n, s):
    a = arr(n + 1, 0)
    for i in range(n):
        a[i + 1] = a[i] + val(i, s)
    t = 0
    for j in range(n // 4):
        lo = val(j, s) % n
        hi = lo + val(j + 1, s) % (n - lo)
        t += a[hi + 1] - a[lo]
    return t


@work("estadistica", 8192)
def minmaxSpan(n, s):
    a = data(n, s)
    mn = a[0]
    mx = a[0]
    for i in range(1, n):
        mn = low(mn, a[i])
        mx = high(mx, a[i])
    return mx - mn


@work("estadistica", 6144)
def normalizedCov(n, s):
    a = data(n, s)
    b = data(n, s + 2)
    x = 0
    y = 0
    xy = 0
    for i in range(n):
        x += a[i]
        y += b[i]
        xy += a[i] * b[i]
    return n * xy - x * y


@work("estadistica", 8192)
def lowpassMedian(n, s):
    a = data(n, s)
    total = 0
    for i in range(2, n):
        x = low(a[i], a[i - 1])
        y = high(a[i], a[i - 1])
        total += high(x, low(a[i - 2], y))
    return total


@work("estadistica", 8192)
def lagCorrelation(n, s):
    a = data(n, s)
    t = 0
    lag = s % 23 + 1
    for i in range(lag, n):
        t += (a[i] - 500) * (a[i - lag] - 500)
    return t


@work("estadistica", 8192)
def trimmedThreshold(n, s):
    a = data(n, s)
    t = 0
    count = 0
    for i in range(n):
        if a[i] >= 100 and a[i] < 900:
            t += a[i]
            count += 1
    return t // (count + 1)


@work("matrices", 38)
def matrixMultiply(n, s):
    a = data(n * n, s)
    b = data(n * n, s + 1)
    t = 0
    for i in range(n):
        for j in range(n):
            v = 0
            for k in range(n):
                v += a[i * n + k] * b[k * n + j]
            t += v % 101
    return t


@work("matrices", 92)
def matrixTranspose(n, s):
    a = data(n * n, s)
    b = arr(n * n, 0)
    for i in range(n):
        for j in range(n):
            b[j * n + i] = a[i * n + j]
    t = 0
    for k in range(n * n):
        t += b[k] * (k % 13)
    return t


@work("matrices", 68)
def fivePoint(n, s):
    a = data(n * n, s)
    t = 0
    for i in range(1, n - 1):
        for j in range(1, n - 1):
            p = i * n + j
            t += (a[p] + a[p - 1] + a[p + 1] + a[p - n] + a[p + n]) // 5
    return t


@work("matrices", 76)
def sobelHorizontal(n, s):
    a = data(n * n, s)
    t = 0
    for i in range(1, n - 1):
        for j in range(1, n - 1):
            p = i * n + j
            l = a[p - n - 1] + a[p - 1] * 2 + a[p + n - 1]
            r = a[p - n + 1] + a[p + 1] * 2 + a[p + n + 1]
            t += dist(l, r)
    return t


@work("matrices", 72)
def summedArea(n, s):
    p = arr((n + 1) * (n + 1), 0)
    w = n + 1
    for i in range(1, w):
        row = 0
        for j in range(1, w):
            row += val(i * w + j, s)
            p[i * w + j] = row + p[(i - 1) * w + j]
    return p[n * w + n] + p[(n // 2) * w + n // 3]


@work("matrices", 64)
def lifeOneStep(n, s):
    a = data(n * n, s)
    alive = 0
    for i in range(1, n - 1):
        for j in range(1, n - 1):
            p = i * n + j
            count = 0
            for y in range(3):
                for x in range(3):
                    if y != 1 or x != 1:
                        count += a[p + (y - 1) * n + x - 1] % 2
            if count == 3 or (count == 2 and a[p] % 2 == 1):
                alive += 1
    return alive


@work("matrices", 100)
def diagonalEnergy(n, s):
    a = data(n * n, s)
    t = 0
    for d in range(n):
        v = 0
        for i in range(n - d):
            v += a[i * n + i + d]
        t += v * v
    return t


@work("matrices", 80)
def triangularSolve(n, s):
    x = arr(n, 0)
    for i in range(n):
        v = val(i, s)
        for j in range(i):
            v += (val(i + j, s) % 5) * x[j]
        x[i] = v % 997
    return x[n - 1]


@work("matrices", 96)
def rowNormalize(n, s):
    a = data(n * n, s)
    t = 0
    for i in range(n):
        mx = 1
        for j in range(n):
            mx = high(mx, a[i * n + j])
        for k in range(n):
            t += a[i * n + k] * 255 // mx
    return t


@work("matrices", 80)
def maxPool(n, s):
    a = data(n * n, s)
    t = 0
    for i in range(n // 2):
        for j in range(n // 2):
            p = 2 * i * n + 2 * j
            t += high(high(a[p], a[p + 1]), high(a[p + n], a[p + n + 1]))
    return t
