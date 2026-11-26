def caesar_shift(s: str, k: int) -> str:
    result = []
    for c in s:
        if 'a' <= c <= 'z':
            result.append(chr((ord(c) - ord('a') + k) % 26 + ord('a')))
        else:
            result.append(c)
    return ''.join(result)

import copy as _copy
for _args in (('abc', 1), ('xyz', 3), ('hello, world', 13), ('', 5), ('Zebra-zoo', 25), ('same', 0)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(caesar_shift(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
