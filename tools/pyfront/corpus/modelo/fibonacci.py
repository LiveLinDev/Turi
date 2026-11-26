def fibonacci(n: int) -> int:
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a

import copy as _copy
for _args in ((10,), (0,), (1,), (2,), (20,), (15,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(fibonacci(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
