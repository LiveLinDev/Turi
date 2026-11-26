def longest_common_prefix(words: list[str]) -> str:
    if not words:
        return ""
    prefix = words[0]
    for word in words[1:]:
        while not word.startswith(prefix):
            prefix = prefix[:-1]
            if not prefix:
                return ""
    return prefix

import copy as _copy
for _args in ((['flower', 'flow', 'flight'],), (['dog', 'car'],), ([],), (['same'],), (['ab', 'abc', 'abcd'],), (['', 'a'],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(longest_common_prefix(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
