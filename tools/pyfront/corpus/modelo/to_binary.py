def to_binary(n: int) -> str:
    return bin(n)[2:]

import copy as _copy
for _args in ((5,), (0,), (1,), (255,), (1024,), (37,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(to_binary(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
