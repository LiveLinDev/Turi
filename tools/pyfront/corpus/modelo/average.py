def average(nums: list[int]) -> float:
    return sum(nums) / len(nums)

import copy as _copy
for _args in (([1, 2, 3, 4],), ([5],), ([-1, 1],), ([1, 2],), ([10, 20, 30, 45],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(average(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
