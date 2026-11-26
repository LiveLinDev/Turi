def is_palindrome(s: str) -> bool:
    return s == s[::-1]

import copy as _copy
for _args in (('level',), ('hello',), ('',), ('a',), ('abba',), ('abcda',), ('Aa',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(is_palindrome(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
