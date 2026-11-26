def balanced_parens(s: str) -> bool:
    depth = 0
    for c in s:
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth < 0:
                return False
    return depth == 0

import copy as _copy
for _args in (('(a(b)c)',), (')(',), ('',), ('((())',), ('()()',), ('(()))(',), ('no parens',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(balanced_parens(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
