def most_frequent(nums: list[int]) -> int:
    counts = {}
    for n in nums:
        counts[n] = counts.get(n, 0) + 1
    return min(counts, key=lambda x: (-counts[x], x))

import copy as _copy
for _args in (([1, 3, 3, 2],), ([7],), ([2, 2, 1, 1],), ([5, 4, 5, 4, 3],), ([-1, -1, 0],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(most_frequent(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
