def all_unique(nums: list[int]) -> bool:
    return len(nums) == len(set(nums))

import copy as _copy
for _args in (([1, 2, 3],), ([1, 2, 1],), ([],), ([0],), ([5, -5, 5],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(all_unique(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
