def celsius_to_fahrenheit(c: float) -> float:
    return c * 9.0 / 5.0 + 32.0

import copy as _copy
for _args in ((100.0,), (0.0,), (-40.0,), (36.6,), (21.5,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(celsius_to_fahrenheit(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
