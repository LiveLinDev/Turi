def best_profit(prices: list[int]) -> int:
    min_price = prices[0]
    profit = 0
    for p in prices[1:]:
        profit = max(profit, p - min_price)
        min_price = min(min_price, p)
    return profit

import copy as _copy
for _args in (([7, 1, 5, 3, 6, 4],), ([7, 6, 4],), ([5],), ([1, 2, 3, 4],), ([3, 8, 1, 2],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(best_profit(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
