def fizz_buzz(n: int) -> list[str]:
    result = []
    for i in range(1, n + 1):
        if i % 15 == 0:
            result.append("FizzBuzz")
        elif i % 3 == 0:
            result.append("Fizz")
        elif i % 5 == 0:
            result.append("Buzz")
        else:
            result.append(str(i))
    return result

import copy as _copy
for _args in ((5,), (0,), (1,), (15,), (3,)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(fizz_buzz(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
