def char_code_sum(s: str) -> int:
    return sum(ord(c) for c in s)

import copy as _copy
for _args in (('abc',), ('',), ('A',), ('hello world',), ('zzz',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(char_code_sum(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
