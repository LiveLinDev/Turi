def reverse_words(s: str) -> str:
    return ' '.join(reversed(s.split(' ')))

import copy as _copy
for _args in (('one two three',), ('solo',), ('a b',), ('the quick brown fox',), ('x y z w',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(reverse_words(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
