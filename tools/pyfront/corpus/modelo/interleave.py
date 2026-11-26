def interleave(a: list[int], b: list[int]) -> list[int]:
    result = []
    i = j = 0
    while i < len(a) and j < len(b):
        result.append(a[i])
        result.append(b[j])
        i += 1
        j += 1
    result.extend(a[i:])
    result.extend(b[j:])
    return result

import copy as _copy
for _args in (([1, 2, 3], [10, 20]), ([], []), ([1], [2, 3, 4]), ([], [5]), ([7, 8], [])):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(interleave(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
