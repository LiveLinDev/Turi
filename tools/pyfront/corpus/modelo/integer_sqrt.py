def integer_sqrt(n: int) -> int:
    if n < 2:
        return n
    lo, hi = 1, n
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if mid * mid <= n:
            lo = mid
        else:
            hi = mid - 1
    return lo

import copy as _copy
for _args in ((17,), (0,), (1,), (16,), (99,), (1000000,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(integer_sqrt(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
