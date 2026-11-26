def longest_word(words: list[str]) -> str:
    return max(words, key=len)

import copy as _copy
for _args in ((['a', 'abc', 'ab'],), (['same', 'size'],), (['x'],), (['', 'b'],), (['hello', 'hi', 'howdy!'],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(longest_word(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
