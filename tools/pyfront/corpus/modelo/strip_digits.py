def strip_digits(s: str) -> str:
    return ''.join(c for c in s if not c.isdigit())

import copy as _copy
for _args in (('a1b22c',), ('',), ('123',), ('no digits',), ('9 lives, 2 go',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(strip_digits(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
