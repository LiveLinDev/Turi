def max_of_list(nums: list[int]) -> int:
    return max(nums)

import copy as _copy
for _args in (([3, 9, 2],), ([-5],), ([-3, -1, -7],), ([4, 4, 4],), ([1, 2, 3, 100, 5],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(max_of_list(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
