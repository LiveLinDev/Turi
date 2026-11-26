def longest_run(nums: list[int]) -> int:
    if not nums:
        return 0
    best = 1
    cur = 1
    for i in range(1, len(nums)):
        if nums[i] == nums[i - 1]:
            cur += 1
            best = max(best, cur)
        else:
            cur = 1
    return best

import copy as _copy
for _args in (([1, 1, 2, 2, 2, 3],), ([],), ([5],), ([1, 2, 3],), ([4, 4, 4, 4],), ([1, 1, 2, 1, 1, 1],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(longest_run(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
