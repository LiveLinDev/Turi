def collatz_steps(n: int) -> int:
    steps = 0
    while n != 1:
        if n % 2 == 0:
            n //= 2
        else:
            n = 3 * n + 1
        steps += 1
    return steps

import copy as _copy
for _args in ((6,), (1,), (7,), (27,), (2,), (97,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(collatz_steps(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
