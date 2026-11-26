def pascal_row(n: int) -> list[int]:
    row = [1]
    for _ in range(n):
        row = [1] + [row[i] + row[i + 1] for i in range(len(row) - 1)] + [1]
    return row

import copy as _copy
for _args in ((4,), (0,), (1,), (6,), (10,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(pascal_row(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
