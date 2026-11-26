def last_index_of(nums: list[int], target: int) -> int:
    for i in range(len(nums) - 1, -1, -1):
        if nums[i] == target:
            return i
    return -1

import copy as _copy
for _args in (([1, 2, 1, 3], 1), ([], 4), ([5], 5), ([1, 2, 3], 9), ([7, 7, 7], 7)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(last_index_of(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
