def remove_duplicates(nums: list[int]) -> list[int]:
    seen = set()
    result = []
    for n in nums:
        if n not in seen:
            seen.add(n)
            result.append(n)
    return result

import copy as _copy
for _args in (([3, 1, 3, 2, 1],), ([],), ([7, 7, 7],), ([1, 2, 3],), ([-1, 0, -1, 0, 5],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(remove_duplicates(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
