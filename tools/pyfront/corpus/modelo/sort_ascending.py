def sort_ascending(nums: list[int]) -> list[int]:
    return sorted(nums)

import copy as _copy
for _args in (([3, 1, 2],), ([],), ([5, 5, 1],), ([-1, -3, 2, 0],), ([9, 8, 7, 6, 5, 4],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(sort_ascending(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
