def count_vowels(s: str) -> int:
    return sum(1 for c in s if c in 'aeiou')

import copy as _copy
for _args in (('hello',), ('',), ('xyz',), ('education',), ('AEIOU aeiou',), ('rhythm and blues',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(count_vowels(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
