from corpus import arr, data, dist, high, low, val, work


@work("dinamica", 160)
def editDistance(n, s):
    a = data(n, s)
    b = data(n, s + 1)
    d = arr(n + 1, 0)
    for j in range(n + 1):
        d[j] = j
    for i in range(1, n + 1):
        diag = d[0]
        d[0] = i
        for k in range(1, n + 1):
            old = d[k]
            cost = 0
            if a[i - 1] % 7 != b[k - 1] % 7:
                cost = 1
            d[k] = low(low(d[k - 1], old) + 1, diag + cost)
            diag = old
    return d[n]


@work("dinamica", 160)
def commonSubsequence(n, s):
    a = data(n, s)
    b = data(n, s + 2)
    d = arr(n + 1, 0)
    for i in range(n):
        diag = 0
        for j in range(1, n + 1):
            old = d[j]
            if a[i] % 11 == b[j - 1] % 11:
                d[j] = diag + 1
            else:
                d[j] = high(d[j], d[j - 1])
            diag = old
    return d[n]


@work("dinamica", 700)
def knapsack(n, s):
    d = arr(n + 1, 0)
    for i in range(48):
        w = val(i, s) % 37 + 1
        price = val(i + 1, s) % 89
        j = n
        while j >= w:
            d[j] = high(d[j], d[j - w] + price)
            j -= 1
    return d[n]


@work("dinamica", 1600)
def coinMinimum(n, s):
    d = arr(n + 1, n + 1)
    d[0] = 0
    for i in range(1, n + 1):
        d[i] = d[i - 1] + 1
        for j in range(5):
            c = val(j, s) % 33 + 1
            if i >= c:
                d[i] = low(d[i], d[i - c] + 1)
    return d[n]


@work("dinamica", 180)
def increasingSubseq(n, s):
    a = data(n, s)
    d = arr(n, 1)
    best = 0
    for i in range(n):
        for j in range(i):
            if a[j] < a[i]:
                d[i] = high(d[i], d[j] + 1)
        best = high(best, d[i])
    return best


@work("dinamica", 90)
def gridMinPath(n, s):
    d = arr(n, 0)
    for i in range(n):
        for j in range(n):
            v = val(i * n + j, s)
            if j == 0:
                d[j] += v
            else:
                if i == 0:
                    d[j] = d[j - 1] + v
                else:
                    d[j] = low(d[j], d[j - 1]) + v
    return d[n - 1]


@work("dinamica", 8192)
def maxNonAdjacent(n, s):
    prev = 0
    best = 0
    for i in range(n):
        take = prev + val(i, s)
        prev = best
        best = high(best, take)
    return best


@work("dinamica", 170)
def rodCut(n, s):
    d = arr(n + 1, 0)
    price = data(n + 1, s)
    for i in range(1, n + 1):
        for j in range(1, i + 1):
            d[i] = high(d[i], price[j] + d[i - j])
    return d[n]


@work("dinamica", 8192)
def segmentGain(n, s):
    best = 0
    current = 0
    for i in range(n):
        current = high(0, current + val(i, s) - 503)
        best = high(best, current)
    return best


@work("dinamica", 1024)
def subsetReach(n, s):
    d = arr(n + 1, 0)
    d[0] = 1
    for i in range(30):
        w = val(i, s) % 70 + 1
        j = n
        while j >= w:
            if d[j - w] == 1:
                d[j] = 1
            j -= 1
    total = 0
    for k in range(n + 1):
        total += d[k]
    return total


@work("estructuras", 2048)
def unionFind(n, s):
    p = arr(n, 0)
    for i in range(n):
        p[i] = i
    joined = 0
    for j in range(n):
        a = (j * 37 + s) % n
        b = (j * 19 + val(j, s)) % n
        while p[a] != a:
            p[a] = p[p[a]]
            a = p[a]
        while p[b] != b:
            b = p[b]
        if a != b:
            p[a] = b
            joined += 1
    return joined


@work("estructuras", 2048)
def fenwickUpdateQuery(n, s):
    tree = arr(n + 1, 0)
    t = 0
    for i in range(n):
        k = i + 1
        bit = 1
        while k % (bit * 2) == 0:
            bit *= 2
        tree[k] += val(i, s)
        parent = k + bit
        if parent <= n:
            tree[parent] += tree[k]
        if k % 17 == 0:
            t += tree[k]
    return t + tree[n]


@work("estructuras", 2048)
def ringQueue(n, s):
    q = arr(127, 0)
    head = 0
    tail = 0
    count = 0
    t = 0
    for i in range(n):
        x = val(i, s)
        if count == 0 or (count < 127 and x % 3 != 0):
            q[tail] = x
            tail = (tail + 1) % 127
            count += 1
        else:
            t += q[head]
            head = (head + 1) % 127
            count -= 1
    return t + count


@work("estructuras", 3072)
def monotoneStack(n, s):
    st = arr(n, 0)
    size = 0
    t = 0
    for i in range(n):
        x = val(i, s)
        while size > 0 and st[size - 1] <= x:
            size -= 1
        if size > 0:
            t += st[size - 1]
        st[size] = x
        size += 1
    return t


@work("estructuras", 1024)
def openAddress(n, s):
    cap = 2 * n + 1
    slots = arr(cap, 0)
    probes = 0
    for i in range(n):
        key = i * 231 + val(i, s) + 1
        p = key % cap
        while slots[p] != 0:
            probes += 1
            p = (p + 1) % cap
        slots[p] = key
    return probes + slots[s % cap]


@work("estructuras", 32)
def floydDistances(n, s):
    d = data(n * n, s)
    for k in range(n):
        for i in range(n):
            for j in range(n):
                p = i * n + j
                d[p] = low(d[p], d[i * n + k] + d[k * n + j])
    return d[s % n] + d[n * n - 1]


@work("estructuras", 1200)
def breadthFirst(n, s):
    seen = arr(n, 0)
    q = arr(n, 0)
    q[0] = s % n
    seen[q[0]] = 1
    head = 0
    tail = 1
    t = 0
    while head < tail:
        v = q[head]
        head += 1
        for j in range(2):
            w = (v * (j + 2) + j + 1) % n
            if seen[w] == 0:
                seen[w] = seen[v] + 1
                q[tail] = w
                tail += 1
                t += seen[w]
    return t


@work("estructuras", 2048)
def treeFold(n, s):
    a = data(n, s)
    i = n - 1
    while i > 0:
        a[(i - 1) // 2] += a[i]
        i -= 1
    return a[0] + a[n // 7]


@work("estructuras", 3072)
def sparseEdges(n, s):
    target = arr(n, 0)
    for i in range(n):
        for j in range(3):
            dst = (i * 17 + j * 29 + s) % n
            target[dst] += val(i + j, s) % 19
    t = 0
    for k in range(n):
        t += target[k] * (k % 11)
    return t


@work("estructuras", 2048)
def intervalSweep(n, s):
    hits = arr(n + 1, 0)
    for i in range(n):
        start = (val(i, s) + i) % n
        end = low(n, start + val(i + 1, s) % 41 + 1)
        hits[start] += 1
        hits[end] -= 1
    active = 0
    best = 0
    for k in range(n):
        active += hits[k]
        best = high(active, best)
    return best


@work("codificacion", 8192)
def luhnDigits(n, s):
    total = 0
    for i in range(n):
        d = val(i, s) % 10
        if i % 2 == 1:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return total % 10


@work("codificacion", 8192)
def runLengths(n, s):
    prev = -1
    size = 0
    t = 0
    for i in range(n):
        v = val(i, s) % 3
        if v == prev:
            size += 1
        else:
            t += size * size
            size = 1
            prev = v
    return t + size * size


@work("codificacion", 4096)
def varintLength(n, s):
    t = 0
    for i in range(n):
        x = (i + val(i, s)) * (s + 1)
        t += 1
        while x >= 128:
            t += 1
            x = x // 128
    return t


@work("codificacion", 6144)
def decimalFields(n, s):
    value = 0
    total = 0
    digits = 0
    for i in range(n):
        token = val(i, s) % 13
        if token < 10 and digits < 8:
            value = value * 10 + token
            digits += 1
        else:
            total += value
            value = 0
            digits = 0
    return total + value


@work("codificacion", 8192)
def csvQuoted(n, s):
    quoted = 0
    fields = 1
    for i in range(n):
        c = val(i, s) % 17
        if c == 0:
            quoted = 1 - quoted
        if c == 1 and quoted == 0:
            fields += 1
    return fields


@work("codificacion", 8192)
def bracketDepth(n, s):
    depth = 0
    errors = 0
    for i in range(n):
        k = val(i, s) % 5
        if k == 0:
            depth += 1
        if k == 1:
            if depth == 0:
                errors += 1
            else:
                depth -= 1
    return errors + depth


@work("codificacion", 8192)
def utf8Widths(n, s):
    total = 0
    for i in range(n):
        cp = (val(i, s) * 1237 + i) % 1114112
        width = 1
        if cp >= 128:
            width += 1
        if cp >= 2048:
            width += 1
        if cp >= 65536:
            width += 1
        total += width
    return total


@work("codificacion", 8192)
def patternAutomaton(n, s):
    state = 0
    hits = 0
    for i in range(n):
        x = val(i, s) % 3
        if x == 0:
            if state == 2:
                hits += 1
            state = 1
        else:
            if x == 1 and state == 1:
                state = 2
            else:
                state = 0
    return hits


@work("codificacion", 8192)
def zigzagDelta(n, s):
    last = 0
    total = 0
    for i in range(n):
        x = val(i, s)
        d = x - last
        if d < 0:
            total += (0 - d) * 2 - 1
        else:
            total += d * 2
        last = x
    return total


@work("codificacion", 8192)
def packedNibbles(n, s):
    a = data(n, s)
    t = 0
    for i in range(n // 2):
        byte = (a[2 * i] % 16) * 16 + a[2 * i + 1] % 16
        t += byte * ((i % 7) + 1)
    return t


@work("aplicaciones", 8192)
def tokenBucket(n, s):
    credit = 65
    ok = 0
    for i in range(n):
        credit = low(100, credit + 5)
        request = val(i, s) % 16
        if credit >= request:
            credit -= request
            ok += 1
    return ok


@work("aplicaciones", 8192)
def orderBookLevel(n, s):
    bids = arr(128, 0)
    t = 0
    for i in range(n):
        x = val(i, s)
        index = x % 128
        if x % 3 == 0:
            used = low(bids[index], x % 19)
            bids[index] -= used
            t += used
        else:
            bids[index] += x % 23
    return t


@work("aplicaciones", 8192)
def roundRobin(n, s):
    budget = arr(17, 0)
    for i in range(17):
        budget[i] = val(i, s) + 1
    served = 0
    p = 0
    for j in range(n):
        take = low(13, budget[p])
        served += take
        budget[p] -= take
        if budget[p] == 0:
            budget[p] = val(j, s) + 1
        p = (p + 1) % 17
    return served


@work("aplicaciones", 4096)
def inventoryReorder(n, s):
    stock = arr(61, 20)
    orders = 0
    for i in range(n):
        k = val(i, s) % 61
        stock[k] -= i % 5
        if stock[k] < 5:
            stock[k] += 24
            orders += 1
    return orders


@work("aplicaciones", 8192)
def thermostat(n, s):
    temp = 190
    power = 0
    consumed = 0
    for i in range(n):
        if temp < 184:
            power = 1
        if temp > 211:
            power = 0
        temp += power * 5 - val(i, s) % 6
        consumed += power
    return consumed


@work("aplicaciones", 4096)
def resampleLinear(n, s):
    a = data(n, s)
    t = 0
    for j in range(n * 3 - 3):
        p = j // 3
        fraction = j % 3
        t += (a[p] * (3 - fraction) + a[p + 1] * fraction) // 3
    return t


@work("aplicaciones", 8192)
def envelopeFollower(n, s):
    env = 0
    t = 0
    for i in range(n):
        x = val(i, s)
        if x > env:
            env += (x - env + 1) // 2
        else:
            env -= (env - x) // 8
        t += env
    return t


@work("aplicaciones", 8192)
def telemetryBuckets(n, s):
    b = arr(11, 0)
    for i in range(n):
        v = val(i, s) + 1
        level = 0
        while v > 1:
            v = v // 2
            level += 1
        b[level] += 1
    target = n * 9 // 10
    k = 0
    while target > b[k]:
        target -= b[k]
        k += 1
    return k


@work("aplicaciones", 8192)
def backoffSimulation(n, s):
    delay = 1
    elapsed = 0
    for i in range(n):
        if val(i, s) % 4 != 0:
            delay = 1
        else:
            delay = low(128, delay * 2)
        elapsed += delay
    return elapsed


@work("aplicaciones", 8192)
def packetJitter(n, s):
    previous = 0
    jitter = 0
    for i in range(n):
        transit = val(i, s) % 127
        delta = dist(transit, previous)
        jitter = (15 * jitter + 16 * delta) // 16
        previous = transit
    return jitter
