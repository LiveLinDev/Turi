def triangle_kind(a: int, b: int, c: int) -> str:
    if a + b <= c or a + c <= b or b + c <= a:
        return "invalid"
    if a == b == c:
        return "equilateral"
    if a == b or b == c or a == c:
        return "isosceles"
    return "scalene"

import copy as _copy
for _args in ((3, 3, 3), (3, 4, 5), (1, 1, 2), (5, 5, 8), (2, 3, 2), (10, 1, 1)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(triangle_kind(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
