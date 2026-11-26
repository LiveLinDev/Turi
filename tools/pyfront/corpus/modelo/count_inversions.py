def count_inversions(nums: list[int]) -> int:
    def merge_count(arr: list[int]) -> tuple[list[int], int]:
        if len(arr) <= 1:
            return arr, 0
        mid = len(arr) // 2
        left, inv_l = merge_count(arr[:mid])
        right, inv_r = merge_count(arr[mid:])
        merged = []
        i = j = 0
        inv_split = 0
        while i < len(left) and j < len(right):
            if left[i] <= right[j]:
                merged.append(left[i])
                i += 1
            else:
                merged.append(right[j])
                j += 1
                inv_split += len(left) - i
        merged.extend(left[i:])
        merged.extend(right[j:])
        return merged, inv_l + inv_r + inv_split

    return merge_count(nums)[1]

import copy as _copy
for _args in (([3, 1, 2],), ([],), ([1, 2, 3],), ([3, 2, 1],), ([5, 5],), ([2, 4, 1, 3, 5],)):
    _mine = _copy.deepcopy(list(_args))
    try:
        print(repr(count_inversions(*_mine)), repr(_mine))
    except Exception as _error:
        print("error", type(_error).__name__)
