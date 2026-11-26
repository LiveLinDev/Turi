def prime_factors(n: int) -> list[int]:
    factors = []
    d = 2
    while d * d <= n:
        while n % d == 0:
            factors.append(d)
            n //= d
        d += 1 if d == 2 else 2
    if n > 1:
        factors.append(n)
    return factors

import copy as _copy
for _args in ((12,), (7,), (2,), (360,), (97,), (1001,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(prime_factors(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
