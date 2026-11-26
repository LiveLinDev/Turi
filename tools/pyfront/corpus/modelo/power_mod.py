def power_mod(base: int, exp: int, m: int) -> int:
    result = 1
    base %= m
    while exp > 0:
        if exp & 1:
            result = (result * base) % m
        base = (base * base) % m
        exp >>= 1
    return result

import copy as _copy
for _args in ((2, 10, 1000), (3, 0, 7), (7, 222, 13), (10, 18, 1000000), (5, 3, 1), (123456, 789, 999983)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(power_mod(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
