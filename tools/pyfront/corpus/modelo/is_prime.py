def is_prime(n: int) -> bool:
    if n < 2:
        return False
    if n < 4:
        return True
    if n % 2 == 0:
        return False
    i = 3
    while i * i <= n:
        if n % i == 0:
            return False
        i += 2
    return True

import copy as _copy
for _args in ((7,), (8,), (1,), (2,), (97,), (91,), (0,), (7919,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(is_prime(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
