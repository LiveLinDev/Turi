def factorial(n: int) -> int:
    result = 1
    for i in range(2, n + 1):
        result *= i
    return result

import copy as _copy
for _args in ((5,), (0,), (1,), (10,), (20,), (3,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(factorial(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
