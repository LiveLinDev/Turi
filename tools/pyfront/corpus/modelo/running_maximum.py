def running_maximum(nums: list[int]) -> list[int]:
    result = []
    current = None
    for n in nums:
        current = n if current is None or n > current else current
        result.append(current)
    return result

import copy as _copy
for _args in (([1, 3, 2, 5, 4],), ([],), ([7],), ([-3, -5, -1],), ([2, 2, 2],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(running_maximum(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
