def pairwise_differences(nums: list[int]) -> list[int]:
    return [nums[i + 1] - nums[i] for i in range(len(nums) - 1)]

import copy as _copy
for _args in (([1, 4, 9, 16],), ([],), ([5],), ([3, 3],), ([10, -10, 10],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(pairwise_differences(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
