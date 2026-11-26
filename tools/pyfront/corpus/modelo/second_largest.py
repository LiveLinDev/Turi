def second_largest(nums: list[int]) -> int:
    distinct = set(nums)
    if len(distinct) < 2:
        return -1
    distinct.remove(max(distinct))
    return max(distinct)

import copy as _copy
for _args in (([3, 9, 2, 9],), ([5],), ([],), ([4, 4],), ([-1, -2, -3],), ([10, 20, 20, 15],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(second_largest(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
