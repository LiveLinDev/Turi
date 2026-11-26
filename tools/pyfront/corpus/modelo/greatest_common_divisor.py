def greatest_common_divisor(a: int, b: int) -> int:
    while b:
        a, b = b, a % b
    return a

import copy as _copy
for _args in ((12, 18), (7, 13), (100, 10), (1, 1), (270, 192), (17, 51)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(greatest_common_divisor(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
