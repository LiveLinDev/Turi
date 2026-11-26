def run_length_encode(s: str) -> str:
    result = []
    i = 0
    while i < len(s):
        j = i
        while j < len(s) and s[j] == s[i]:
            j += 1
        result.append(s[i] + str(j - i))
        i = j
    return ''.join(result)

import copy as _copy
for _args in (('aaab',), ('',), ('abc',), ('zzzzzzzzzzzz',), ('aabbaa',), ('x',)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(run_length_encode(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
