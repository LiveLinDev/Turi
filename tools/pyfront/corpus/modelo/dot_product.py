def dot_product(a: list[int], b: list[int]) -> int:
    return sum(x * y for x, y in zip(a, b))

import copy as _copy
for _args in (([1, 2, 3], [4, 5, 6]), ([], []), ([2], [-3]), ([1, 0, 1], [0, 9, 0]), ([7, 7], [1, 1])):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(dot_product(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
