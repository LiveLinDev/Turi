def capitalize_words(s: str) -> str:
    return ' '.join(w[:1].upper() + w[1:] for w in s.split(' '))

import copy as _copy
for _args in (('hello world',), ('a',), ('already Done',), ('x y z',), ('mixed cASE here',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(capitalize_words(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
