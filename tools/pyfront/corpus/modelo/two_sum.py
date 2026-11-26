def two_sum(nums: list[int], target: int) -> list[int]:
    seen = {}
    for i, x in enumerate(nums):
        if target - x in seen:
            return [seen[target - x], i]
        seen.setdefault(x, i)
    return [-1, -1]

import copy as _copy
for _args in (([2, 7, 11, 15], 9), ([1, 2, 3], 7), ([3, 3], 6), ([], 0), ([1, 4, 5, 4], 8), ([0, -1, 1], 0)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(two_sum(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
