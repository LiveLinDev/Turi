"""Language-neutral task set for the multi-language AI benchmark.

Every task has a typed signature, an English description, a reference solution in Python and
test inputs. Expected outputs are computed from the reference, never written by hand.
The first two tests of each task are public (shown in the prompt); the rest are hidden.

Types: int, float, bool, str, list[int], list[str].
"""
from dataclasses import dataclass, field
from typing import Callable


@dataclass(frozen=True)
class Task:
    name: str                      # snake_case words, adapted per language
    doc: str
    params: tuple                  # ((name, type), ...)
    ret: str
    ref: Callable
    inputs: tuple                  # tuple of argument tuples
    tests: tuple = field(default=(), compare=False)


def _mk(name, doc, params, ret, ref, inputs):
    tests = tuple((args, ref(*args)) for args in inputs)
    return Task(name, doc, tuple(params), ret, ref, tuple(inputs), tests)


def _is_prime(n):
    if n < 2:
        return False
    i = 2
    while i * i <= n:
        if n % i == 0:
            return False
        i += 1
    return True


def _collatz(n):
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps


def _rle(s):
    out, i = "", 0
    while i < len(s):
        j = i
        while j < len(s) and s[j] == s[i]:
            j += 1
        out += s[i] + str(j - i)
        i = j
    return out


def _balanced(s):
    depth = 0
    for c in s:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def _longest_run(nums):
    best = cur = 0
    prev = None
    for x in nums:
        cur = cur + 1 if x == prev else 1
        prev = x
        best = max(best, cur)
    return best


def _merge_sorted(a, b):
    i = j = 0
    out = []
    while i < len(a) and j < len(b):
        if a[i] <= b[j]:
            out.append(a[i]); i += 1
        else:
            out.append(b[j]); j += 1
    return out + a[i:] + b[j:]


def _dedupe(nums):
    seen, out = set(), []
    for x in nums:
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


def _caesar(s, k):
    out = ""
    for c in s:
        if "a" <= c <= "z":
            out += chr((ord(c) - 97 + k) % 26 + 97)
        else:
            out += c
    return out


def _binary(n):
    if n == 0:
        return "0"
    out = ""
    while n > 0:
        out = str(n % 2) + out
        n //= 2
    return out


def _digits_sum(n):
    return sum(int(c) for c in str(n))


def _two_sum(nums, target):
    for i in range(len(nums)):
        for j in range(i + 1, len(nums)):
            if nums[i] + nums[j] == target:
                return [i, j]
    return [-1, -1]


def _max_subarray(nums):
    best = cur = nums[0]
    for x in nums[1:]:
        cur = max(x, cur + x)
        best = max(best, cur)
    return best


def _prime_factors(n):
    out, d = [], 2
    while d * d <= n:
        while n % d == 0:
            out.append(d)
            n //= d
        d += 1
    if n > 1:
        out.append(n)
    return out


def _word_lengths(s):
    return [len(w) for w in s.split(" ") if w != ""]


def _title(s):
    return " ".join(w[:1].upper() + w[1:] for w in s.split(" "))


def _lcp(words):
    if not words:
        return ""
    p = words[0]
    for w in words[1:]:
        while not w.startswith(p):
            p = p[:-1]
    return p


def _rotate(nums, k):
    if not nums:
        return []
    k %= len(nums)
    return nums[k:] + nums[:k]


def _fizz(n):
    out = []
    for i in range(1, n + 1):
        out.append("FizzBuzz" if i % 15 == 0 else "Fizz" if i % 3 == 0 else "Buzz" if i % 5 == 0 else str(i))
    return out


def _is_anagram(a, b):
    return sorted(a) == sorted(b)


def _second_largest(nums):
    u = sorted(set(nums))
    return u[-2] if len(u) >= 2 else -1


def _pascal_row(n):
    row = [1]
    for _ in range(n):
        row = [1] + [row[i] + row[i + 1] for i in range(len(row) - 1)] + [1]
    return row


def _gcd(a, b):
    while b:
        a, b = b, a % b
    return a


def _bubble(nums):
    return sorted(nums)


def _mode(nums):
    best, best_n = nums[0], 0
    for x in sorted(set(nums)):
        c = nums.count(x)
        if c > best_n:
            best, best_n = x, c
    return best


def _compress_spaces(s):
    return " ".join(w for w in s.split(" ") if w != "")


def _running_max(nums):
    out, m = [], None
    for x in nums:
        m = x if m is None or x > m else m
        out.append(m)
    return out


def _count_inversions(nums):
    return sum(1 for i in range(len(nums)) for j in range(i + 1, len(nums)) if nums[i] > nums[j])


def _hex(n):
    return format(n, "x")


def _vowel_words(words):
    return [w for w in words if w[:1] in ("a", "e", "i", "o", "u")]


def _int_sqrt(n):
    r = 0
    while (r + 1) * (r + 1) <= n:
        r += 1
    return r


def _interleave(a, b):
    out = []
    for i in range(max(len(a), len(b))):
        if i < len(a):
            out.append(a[i])
        if i < len(b):
            out.append(b[i])
    return out


def _ascii_sum(s):
    return sum(ord(c) for c in s)


def _join_upper(words, sep):
    return sep.join(w.upper() for w in words)


def _stock(prices):
    best, lo = 0, prices[0]
    for p in prices:
        lo = min(lo, p)
        best = max(best, p - lo)
    return best


def _missing(nums):
    n = len(nums)
    return n * (n + 1) // 2 - sum(nums)


def _roman(n):
    vals = [(1000, "M"), (900, "CM"), (500, "D"), (400, "CD"), (100, "C"), (90, "XC"),
            (50, "L"), (40, "XL"), (10, "X"), (9, "IX"), (5, "V"), (4, "IV"), (1, "I")]
    out = ""
    for v, s in vals:
        while n >= v:
            out += s
            n -= v
    return out


TASKS = [
    _mk("sum_even", "Return the sum of the even numbers in nums (0 if there are none).",
        [("nums", "list[int]")], "int", lambda nums: sum(x for x in nums if x % 2 == 0),
        [([1, 2, 3, 4],), ([],), ([7, 9],), ([-2, -4, 5],), ([10],), ([0, 0, 1],)]),
    _mk("count_vowels", "Return how many characters of s are lowercase vowels (a, e, i, o, u).",
        [("s", "str")], "int", lambda s: sum(1 for c in s if c in "aeiou"),
        [("hello",), ("",), ("xyz",), ("education",), ("AEIOU aeiou",), ("rhythm and blues",)]),
    _mk("is_palindrome", "Return true if s reads the same forwards and backwards (case-sensitive, every character counts).",
        [("s", "str")], "bool", lambda s: s == s[::-1],
        [("level",), ("hello",), ("",), ("a",), ("abba",), ("abcda",), ("Aa",)]),
    _mk("reverse_words", "s contains words separated by single spaces. Return the words in reverse order, separated by single spaces.",
        [("s", "str")], "str", lambda s: " ".join(s.split(" ")[::-1]),
        [("one two three",), ("solo",), ("a b",), ("the quick brown fox",), ("x y z w",)]),
    _mk("factorial", "Return n! for 0 <= n <= 20.",
        [("n", "int")], "int", lambda n: 1 if n == 0 else __import__("math").factorial(n),
        [(5,), (0,), (1,), (10,), (20,), (3,)]),
    _mk("is_prime", "Return true if n is a prime number.",
        [("n", "int")], "bool", _is_prime,
        [(7,), (8,), (1,), (2,), (97,), (91,), (0,), (7919,)]),
    _mk("fibonacci", "Return the n-th Fibonacci number, where fibonacci(0) = 0 and fibonacci(1) = 1.",
        [("n", "int")], "int", lambda n: (lambda f: f(f, n))(lambda f, k: k if k < 2 else f(f, k - 1) + f(f, k - 2)) if n < 25 else 0,
        [(10,), (0,), (1,), (2,), (20,), (15,)]),
    _mk("max_of_list", "Return the largest value in nums. nums is never empty.",
        [("nums", "list[int]")], "int", lambda nums: max(nums),
        [([3, 9, 2],), ([-5],), ([-3, -1, -7],), ([4, 4, 4],), ([1, 2, 3, 100, 5],)]),
    _mk("filter_positive", "Return a list with the strictly positive numbers of nums, in the same order.",
        [("nums", "list[int]")], "list[int]", lambda nums: [x for x in nums if x > 0],
        [([1, -2, 3, 0],), ([],), ([-1, -2],), ([5, 6],), ([0, 0, 7, -7, 8],)]),
    _mk("collatz_steps", "Return how many steps the Collatz process needs to reach 1 from n (n >= 1): if n is even divide it by 2, otherwise replace it with 3n + 1.",
        [("n", "int")], "int", _collatz,
        [(6,), (1,), (7,), (27,), (2,), (97,)]),
    _mk("run_length_encode", "Run-length encode s: each run of equal characters becomes the character followed by the run length. Example: aaab becomes a3b1.",
        [("s", "str")], "str", _rle,
        [("aaab",), ("",), ("abc",), ("zzzzzzzzzzzz",), ("aabbaa",), ("x",)]),
    _mk("balanced_parens", "Return true if the parentheses in s are balanced. s may contain other characters, which are ignored.",
        [("s", "str")], "bool", _balanced,
        [("(a(b)c)",), (")(",), ("",), ("((())",), ("()()",), ("(()))(",), ("no parens",)]),
    _mk("longest_run", "Return the length of the longest run of equal consecutive values in nums (0 for an empty list).",
        [("nums", "list[int]")], "int", _longest_run,
        [([1, 1, 2, 2, 2, 3],), ([],), ([5],), ([1, 2, 3],), ([4, 4, 4, 4],), ([1, 1, 2, 1, 1, 1],)]),
    _mk("merge_sorted", "a and b are sorted in ascending order. Return one sorted list with all the elements of both.",
        [("a", "list[int]"), ("b", "list[int]")], "list[int]", _merge_sorted,
        [([1, 4, 9], [2, 3, 10]), ([], [1]), ([1], []), ([], []), ([1, 1], [1, 1]), ([5, 6, 7], [1, 2])]),
    _mk("remove_duplicates", "Return nums without repeated values, keeping the first occurrence of each and the original order.",
        [("nums", "list[int]")], "list[int]", _dedupe,
        [([3, 1, 3, 2, 1],), ([],), ([7, 7, 7],), ([1, 2, 3],), ([-1, 0, -1, 0, 5],)]),
    _mk("caesar_shift", "Shift every lowercase letter of s forward by k positions in the alphabet, wrapping from z to a (0 <= k <= 25). Other characters stay unchanged.",
        [("s", "str"), ("k", "int")], "str", _caesar,
        [("abc", 1), ("xyz", 3), ("hello, world", 13), ("", 5), ("Zebra-zoo", 25), ("same", 0)]),
    _mk("to_binary", "Return the binary representation of n (n >= 0) as a string without leading zeros (\"0\" for 0).",
        [("n", "int")], "str", _binary,
        [(5,), (0,), (1,), (255,), (1024,), (37,)]),
    _mk("digit_sum", "Return the sum of the decimal digits of n (n >= 0).",
        [("n", "int")], "int", _digits_sum,
        [(1234,), (0,), (9,), (99999,), (1000001,), (808,)]),
    _mk("two_sum", "Return the indices [i, j] with i < j of the first pair (smallest i, then smallest j) such that nums[i] + nums[j] == target, or [-1, -1] if there is none.",
        [("nums", "list[int]"), ("target", "int")], "list[int]", _two_sum,
        [([2, 7, 11, 15], 9), ([1, 2, 3], 7), ([3, 3], 6), ([], 0), ([1, 4, 5, 4], 8), ([0, -1, 1], 0)]),
    _mk("max_subarray_sum", "Return the largest sum of a non-empty contiguous subarray of nums. nums is never empty.",
        [("nums", "list[int]")], "int", _max_subarray,
        [([-2, 1, -3, 4, -1, 2, 1, -5, 4],), ([-3],), ([1, 2, 3],), ([-1, -2, -3],), ([5, -9, 6, -2, 3],)]),
    _mk("prime_factors", "Return the prime factors of n (n >= 2) in ascending order, repeated according to multiplicity. Example: 12 gives [2, 2, 3].",
        [("n", "int")], "list[int]", _prime_factors,
        [(12,), (7,), (2,), (360,), (97,), (1001,)]),
    _mk("word_lengths", "s contains words separated by spaces (possibly several). Return the length of each word, in order.",
        [("s", "str")], "list[int]", _word_lengths,
        [("the quick fox",), ("",), ("a",), ("two  spaces",), ("  lead and trail  ",)]),
    _mk("capitalize_words", "s contains words separated by single spaces. Return s with the first letter of every word converted to uppercase (letters are a-z or A-Z).",
        [("s", "str")], "str", _title,
        [("hello world",), ("a",), ("already Done",), ("x y z",), ("mixed cASE here",)]),
    _mk("longest_common_prefix", "Return the longest string that is a prefix of every word in words (\"\" if words is empty or there is no common prefix).",
        [("words", "list[str]")], "str", _lcp,
        [(["flower", "flow", "flight"],), (["dog", "car"],), ([],), (["same"],), (["ab", "abc", "abcd"],), (["", "a"],)]),
    _mk("rotate_left", "Return nums rotated to the left by k positions (k >= 0, may be larger than the length). An empty list stays empty.",
        [("nums", "list[int]"), ("k", "int")], "list[int]", _rotate,
        [([1, 2, 3, 4, 5], 2), ([], 3), ([1], 10), ([1, 2, 3], 3), ([1, 2, 3], 4), ([9, 8], 1)]),
    _mk("fizz_buzz", "Return a list with the values for 1..n: \"FizzBuzz\" for multiples of 15, \"Fizz\" for other multiples of 3, \"Buzz\" for other multiples of 5, otherwise the number as a string.",
        [("n", "int")], "list[str]", _fizz,
        [(5,), (0,), (1,), (15,), (3,)]),
    _mk("is_anagram", "Return true if a and b contain exactly the same characters with the same counts (case-sensitive).",
        [("a", "str"), ("b", "str")], "bool", _is_anagram,
        [("listen", "silent"), ("abc", "abd"), ("", ""), ("aab", "aba"), ("aab", "abb"), ("a", "aa")]),
    _mk("second_largest", "Return the second largest distinct value in nums, or -1 if there are fewer than two distinct values.",
        [("nums", "list[int]")], "int", _second_largest,
        [([3, 9, 2, 9],), ([5],), ([],), ([4, 4],), ([-1, -2, -3],), ([10, 20, 20, 15],)]),
    _mk("pascal_row", "Return row n of Pascal's triangle (row 0 is [1], row 1 is [1, 1]).",
        [("n", "int")], "list[int]", _pascal_row,
        [(4,), (0,), (1,), (6,), (10,)]),
    _mk("greatest_common_divisor", "Return the greatest common divisor of a and b (both > 0).",
        [("a", "int"), ("b", "int")], "int", _gcd,
        [(12, 18), (7, 13), (100, 10), (1, 1), (270, 192), (17, 51)]),
    _mk("sort_ascending", "Return the elements of nums sorted in ascending order.",
        [("nums", "list[int]")], "list[int]", _bubble,
        [([3, 1, 2],), ([],), ([5, 5, 1],), ([-1, -3, 2, 0],), ([9, 8, 7, 6, 5, 4],)]),
    _mk("most_frequent", "Return the value that appears most often in nums (never empty). If several values tie, return the smallest of them.",
        [("nums", "list[int]")], "int", _mode,
        [([1, 3, 3, 2],), ([7],), ([2, 2, 1, 1],), ([5, 4, 5, 4, 3],), ([-1, -1, 0],)]),
    _mk("collapse_spaces", "Return s with leading and trailing spaces removed and every run of spaces replaced by a single space.",
        [("s", "str")], "str", _compress_spaces,
        [("  a   b  ",), ("",), ("one",), ("a b",), ("   ",), ("x    y z",)]),
    _mk("running_maximum", "Return a list where element i is the maximum of nums[0..i].",
        [("nums", "list[int]")], "list[int]", _running_max,
        [([1, 3, 2, 5, 4],), ([],), ([7],), ([-3, -5, -1],), ([2, 2, 2],)]),
    _mk("count_inversions", "Return the number of pairs (i, j) with i < j and nums[i] > nums[j].",
        [("nums", "list[int]")], "int", _count_inversions,
        [([3, 1, 2],), ([],), ([1, 2, 3],), ([3, 2, 1],), ([5, 5],), ([2, 4, 1, 3, 5],)]),
    _mk("to_hex", "Return the lowercase hexadecimal representation of n (n >= 0) without prefix (\"0\" for 0).",
        [("n", "int")], "str", _hex,
        [(255,), (0,), (16,), (4095,), (48879,), (10,)]),
    _mk("words_starting_with_vowel", "Return the words that start with a lowercase vowel (a, e, i, o, u), in the same order.",
        [("words", "list[str]")], "list[str]", _vowel_words,
        [(["apple", "pear", "orange"],), ([],), (["kiwi"],), (["egg", "ice", "", "Under"],), (["umbrella", "tree"],)]),
    _mk("integer_sqrt", "Return the largest integer r such that r * r <= n (n >= 0).",
        [("n", "int")], "int", _int_sqrt,
        [(17,), (0,), (1,), (16,), (99,), (1000000,)]),
    _mk("interleave", "Return a list that alternates elements of a and b (a first). When one list runs out, append the rest of the other.",
        [("a", "list[int]"), ("b", "list[int]")], "list[int]", _interleave,
        [([1, 2, 3], [10, 20]), ([], []), ([1], [2, 3, 4]), ([], [5]), ([7, 8], [])]),
    _mk("char_code_sum", "Return the sum of the ASCII codes of the characters of s (s is ASCII).",
        [("s", "str")], "int", _ascii_sum,
        [("abc",), ("",), ("A",), ("hello world",), ("zzz",)]),
    _mk("join_uppercase", "Convert every word to uppercase (words contain only a-z) and join them with sep between consecutive words.",
        [("words", "list[str]"), ("sep", "str")], "str", _join_upper,
        [(["a", "bc"], "-"), ([], ","), (["solo"], ", "), (["x", "y", "z"], ""), (["ab", "cd"], " and ")]),
    _mk("best_profit", "prices[i] is the price on day i (never empty). Return the best profit from buying on one day and selling on a later day, or 0 if no profit is possible.",
        [("prices", "list[int]")], "int", _stock,
        [([7, 1, 5, 3, 6, 4],), ([7, 6, 4],), ([5],), ([1, 2, 3, 4],), ([3, 8, 1, 2],)]),
    _mk("missing_number", "nums contains every integer from 0 to len(nums) except one, in any order. Return the missing integer.",
        [("nums", "list[int]")], "int", _missing,
        [([3, 0, 1],), ([0],), ([1],), ([0, 1, 2, 4],), ([5, 4, 3, 2, 1],)]),
    _mk("to_roman", "Return n (1 <= n <= 3999) as a Roman numeral.",
        [("n", "int")], "str", _roman,
        [(4,), (1994,), (1,), (3999,), (58,), (444,)]),
    _mk("average", "Return the arithmetic mean of nums as a float (nums is never empty).",
        [("nums", "list[int]")], "float", lambda nums: sum(nums) / len(nums),
        [([1, 2, 3, 4],), ([5],), ([-1, 1],), ([1, 2],), ([10, 20, 30, 45],)]),
    _mk("celsius_to_fahrenheit", "Convert a temperature from Celsius to Fahrenheit.",
        [("c", "float")], "float", lambda c: c * 9.0 / 5.0 + 32.0,
        [(100.0,), (0.0,), (-40.0,), (36.6,), (21.5,)]),
    _mk("count_words", "Return how many words s contains. Words are maximal runs of non-space characters.",
        [("s", "str")], "int", lambda s: len([w for w in s.split(" ") if w]),
        [("a b  c",), ("",), ("   ",), ("word",), (" two words ",)]),
    _mk("all_unique", "Return true if no value appears more than once in nums.",
        [("nums", "list[int]")], "bool", lambda nums: len(set(nums)) == len(nums),
        [([1, 2, 3],), ([1, 2, 1],), ([],), ([0],), ([5, -5, 5],)]),
    _mk("clamp_values", "Return nums with every value below lo replaced by lo and every value above hi replaced by hi (lo <= hi).",
        [("nums", "list[int]"), ("lo", "int"), ("hi", "int")], "list[int]",
        lambda nums, lo, hi: [min(max(x, lo), hi) for x in nums],
        [([1, 5, 10], 3, 7), ([], 0, 1), ([-5, 0, 5], 0, 0), ([4], 1, 9), ([100, -100], -10, 10)]),
    _mk("string_repeat", "Return s repeated n times (n >= 0).",
        [("s", "str"), ("n", "int")], "str", lambda s, n: s * n,
        [("ab", 3), ("x", 0), ("", 5), ("hey", 1), ("-", 6)]),
    _mk("power_mod", "Return (base raised to exp) modulo m, for base >= 0, exp >= 0 and 1 <= m <= 1000000. The result must be exact even when the power is huge.",
        [("base", "int"), ("exp", "int"), ("m", "int")], "int", lambda b, e, m: pow(b, e, m),
        [(2, 10, 1000), (3, 0, 7), (7, 222, 13), (10, 18, 1000000), (5, 3, 1), (123456, 789, 999983)]),
    _mk("last_index_of", "Return the largest index i such that nums[i] == target, or -1 if target does not appear.",
        [("nums", "list[int]"), ("target", "int")], "int",
        lambda nums, t: max([i for i, x in enumerate(nums) if x == t], default=-1),
        [([1, 2, 1, 3], 1), ([], 4), ([5], 5), ([1, 2, 3], 9), ([7, 7, 7], 7)]),
    _mk("strip_digits", "Return s with every decimal digit removed.",
        [("s", "str")], "str", lambda s: "".join(c for c in s if not c.isdigit()),
        [("a1b22c",), ("",), ("123",), ("no digits",), ("9 lives, 2 go",)]),
    _mk("pairwise_differences", "Return a list with nums[i + 1] - nums[i] for every valid i (empty if nums has fewer than two elements).",
        [("nums", "list[int]")], "list[int]", lambda nums: [nums[i + 1] - nums[i] for i in range(len(nums) - 1)],
        [([1, 4, 9, 16],), ([],), ([5],), ([3, 3],), ([10, -10, 10],)]),
    _mk("leap_year", "Return true if year is a leap year in the Gregorian calendar.",
        [("year", "int")], "bool", lambda y: y % 4 == 0 and (y % 100 != 0 or y % 400 == 0),
        [(2024,), (1900,), (2000,), (2023,), (2400,), (2100,)]),
    _mk("longest_word", "Return the longest word in words (never empty). If several words tie, return the first of them.",
        [("words", "list[str]")], "str", lambda words: max(words, key=len),
        [(["a", "abc", "ab"],), (["same", "size"],), (["x"],), (["", "b"],), (["hello", "hi", "howdy!"],)]),
    _mk("count_char", "Return how many times the one-character string ch appears in s.",
        [("s", "str"), ("ch", "str")], "int", lambda s, ch: s.count(ch),
        [("banana", "a"), ("", "x"), ("xyz", "q"), ("aaaa", "a"), ("a b c", " ")]),
    _mk("triangle_kind", "Given three positive side lengths, return \"invalid\" if they cannot form a triangle (the sum of any two sides must be greater than the third), otherwise \"equilateral\", \"isosceles\" or \"scalene\".",
        [("a", "int"), ("b", "int"), ("c", "int")], "str",
        lambda a, b, c: "invalid" if a + b <= c or a + c <= b or b + c <= a else "equilateral" if a == b == c else "isosceles" if a == b or b == c or a == c else "scalene",
        [(3, 3, 3), (3, 4, 5), (1, 1, 2), (5, 5, 8), (2, 3, 2), (10, 1, 1)]),
    _mk("dot_product", "Return the dot product of a and b (same length).",
        [("a", "list[int]"), ("b", "list[int]")], "int", lambda a, b: sum(x * y for x, y in zip(a, b)),
        [([1, 2, 3], [4, 5, 6]), ([], []), ([2], [-3]), ([1, 0, 1], [0, 9, 0]), ([7, 7], [1, 1])]),
    _mk("swap_case", "Return s with lowercase ASCII letters turned to uppercase and uppercase ASCII letters turned to lowercase. Other characters stay unchanged.",
        [("s", "str")], "str", lambda s: s.swapcase(),
        [("Hello World",), ("",), ("abc",), ("ABC",), ("a1B2-c3",)]),
]

PUBLIC = 2  # tests shown in the prompt


def by_name(name):
    for t in TASKS:
        if t.name == name:
            return t
    raise KeyError(name)
