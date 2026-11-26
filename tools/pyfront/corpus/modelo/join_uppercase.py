def join_uppercase(words: list[str], sep: str) -> str:
    return sep.join(w.upper() for w in words)

import copy as _copy
for _args in ((['a', 'bc'], '-'), ([], ','), (['solo'], ', '), (['x', 'y', 'z'], ''), (['ab', 'cd'], ' and ')):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(join_uppercase(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
