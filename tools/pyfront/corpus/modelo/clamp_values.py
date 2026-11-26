def clamp_values(nums: list[int], lo: int, hi: int) -> list[int]:
    return [max(lo, min(hi, n)) for n in nums]

import copy as _copy
for _args in (([1, 5, 10], 3, 7), ([], 0, 1), ([-5, 0, 5], 0, 0), ([4], 1, 9), ([100, -100], -10, 10)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(clamp_values(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
