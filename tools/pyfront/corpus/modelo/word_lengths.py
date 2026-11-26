def word_lengths(s: str) -> list[int]:
    return [len(w) for w in s.split()]

import copy as _copy
for _args in (('the quick fox',), ('',), ('a',), ('two  spaces',), ('  lead and trail  ',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(word_lengths(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
