def swap_case(s: str) -> str:
    return s.swapcase()

import copy as _copy
for _args in (('Hello World',), ('',), ('abc',), ('ABC',), ('a1B2-c3',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(swap_case(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
