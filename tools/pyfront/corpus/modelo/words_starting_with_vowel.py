def words_starting_with_vowel(words: list[str]) -> list[str]:
    return [w for w in words if w and w[0] in 'aeiou']

import copy as _copy
for _args in ((['apple', 'pear', 'orange'],), ([],), (['kiwi'],), (['egg', 'ice', '', 'Under'],), (['umbrella', 'tree'],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(words_starting_with_vowel(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
