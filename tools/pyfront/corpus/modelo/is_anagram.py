def is_anagram(a: str, b: str) -> bool:
    return sorted(a) == sorted(b)

import copy as _copy
for _args in (('listen', 'silent'), ('abc', 'abd'), ('', ''), ('aab', 'aba'), ('aab', 'abb'), ('a', 'aa')):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(is_anagram(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
