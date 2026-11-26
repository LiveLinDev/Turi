def digit_sum(n: int) -> int:
    return sum(int(d) for d in str(n))

import copy as _copy
for _args in ((1234,), (0,), (9,), (99999,), (1000001,), (808,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(digit_sum(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
