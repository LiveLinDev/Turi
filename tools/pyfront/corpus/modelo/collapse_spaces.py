def collapse_spaces(s: str) -> str:
    s = s.strip()
    result = []
    i = 0
    n = len(s)
    in_space = False
    while i < n:
        ch = s[i]
        if ch == ' ':
            if not in_space:
                result.append(' ')
                in_space = True
        else:
            result.append(ch)
            in_space = False
        i = i + 1
    return ''.join(result)

import copy as _copy
for _args in (('  a   b  ',), ('',), ('one',), ('a b',), ('   ',), ('x    y z',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(collapse_spaces(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
