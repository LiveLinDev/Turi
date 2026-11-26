def count_char(s: str, ch: str) -> int:
    return s.count(ch)

import copy as _copy
for _args in (('banana', 'a'), ('', 'x'), ('xyz', 'q'), ('aaaa', 'a'), ('a b c', ' ')):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(count_char(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
