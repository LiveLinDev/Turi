def leap_year(year: int) -> bool:
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)

import copy as _copy
for _args in ((2024,), (1900,), (2000,), (2023,), (2400,), (2100,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(leap_year(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
