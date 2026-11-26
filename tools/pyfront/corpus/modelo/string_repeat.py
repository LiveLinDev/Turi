def string_repeat(s: str, n: int) -> str:
    return s * n

import copy as _copy
for _args in (('ab', 3), ('x', 0), ('', 5), ('hey', 1), ('-', 6)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(string_repeat(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
