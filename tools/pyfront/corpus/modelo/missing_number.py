def missing_number(nums: list[int]) -> int:
    n = len(nums)
    return n * (n + 1) // 2 - sum(nums)

import copy as _copy
for _args in (([3, 0, 1],), ([0],), ([1],), ([0, 1, 2, 4],), ([5, 4, 3, 2, 1],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(missing_number(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
