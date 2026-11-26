def sum_even(nums: list[int]) -> int:
    return sum(n for n in nums if n % 2 == 0)

import copy as _copy
for _args in (([1, 2, 3, 4],), ([],), ([7, 9],), ([-2, -4, 5],), ([10],), ([0, 0, 1],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(sum_even(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
