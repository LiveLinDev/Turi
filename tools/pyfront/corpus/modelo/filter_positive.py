def filter_positive(nums: list[int]) -> list[int]:
    return [n for n in nums if n > 0]

import copy as _copy
for _args in (([1, -2, 3, 0],), ([],), ([-1, -2],), ([5, 6],), ([0, 0, 7, -7, 8],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(filter_positive(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
