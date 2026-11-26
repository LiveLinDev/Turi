def to_roman(n: int) -> str:
    vals = [(1000, "M"), (900, "CM"), (500, "D"), (400, "CD"),
            (100, "C"), (90, "XC"), (50, "L"), (40, "XL"),
            (10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")]
    result = []
    for v, s in vals:
        while n >= v:
            result.append(s)
            n -= v
    return "".join(result)

import copy as _copy
for _args in ((4,), (1994,), (1,), (3999,), (58,), (444,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(to_roman(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
