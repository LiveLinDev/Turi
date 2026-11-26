def rotate_left(nums: list[int], k: int) -> list[int]:
    if not nums:
        return []
    k %= len(nums)
    return nums[k:] + nums[:k]

import copy as _copy
for _args in (([1, 2, 3, 4, 5], 2), ([], 3), ([1], 10), ([1, 2, 3], 3), ([1, 2, 3], 4), ([9, 8], 1)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(rotate_left(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
