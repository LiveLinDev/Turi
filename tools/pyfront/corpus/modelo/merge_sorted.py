def merge_sorted(a: list[int], b: list[int]) -> list[int]:
    result = []
    i = j = 0
    while i < len(a) and j < len(b):
        if a[i] <= b[j]:
            result.append(a[i])
            i += 1
        else:
            result.append(b[j])
            j += 1
    result.extend(a[i:])
    result.extend(b[j:])
    return result

import copy as _copy
for _args in (([1, 4, 9], [2, 3, 10]), ([], [1]), ([1], []), ([], []), ([1, 1], [1, 1]), ([5, 6, 7], [1, 2])):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(merge_sorted(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
