def max_subarray_sum(nums: list[int]) -> int:
    best = cur = nums[0]
    for x in nums[1:]:
        cur = max(x, cur + x)
        best = max(best, cur)
    return best

import copy as _copy
for _args in (([-2, 1, -3, 4, -1, 2, 1, -5, 4],), ([-3],), ([1, 2, 3],), ([-1, -2, -3],), ([5, -9, 6, -2, 3],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(max_subarray_sum(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
