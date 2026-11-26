def count_words(s: str) -> int:
    return len(s.split())

import copy as _copy
for _args in (('a b  c',), ('',), ('   ',), ('word',), (' two words ',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(count_words(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
