def to_hex(n: int) -> str:
    return format(n, "x")

import copy as _copy
for _args in ((255,), (0,), (16,), (4095,), (48879,), (10,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(to_hex(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
