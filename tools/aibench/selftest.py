"""Checks every language adapter (signature, driver, build, run, comparison) with hand-written
solutions for a few tasks that cover all parameter and return types. No model is involved."""
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from langs import all_langs, run_candidate  # noqa: E402
from tasks import by_name  # noqa: E402

SOL = {
    "sum_even": {
        "turi": "fn sumEven(nums: [int]) -> int {\n    var t = 0\n    for x in nums { if x % 2 == 0 { t += x } }\n    return t\n}",
        "python": "def sum_even(nums):\n    return sum(x for x in nums if x % 2 == 0)",
        "c": "long long sum_even(const long long *nums, int nums_len) {\n    long long t = 0;\n    for (int i = 0; i < nums_len; i++) if (nums[i] % 2 == 0) t += nums[i];\n    return t;\n}",
        "cpp": "#include <vector>\nlong long sum_even(const std::vector<long long>& nums) {\n    long long t = 0;\n    for (auto x : nums) if (x % 2 == 0) t += x;\n    return t;\n}",
        "rust": "fn sum_even(nums: &[i64]) -> i64 {\n    nums.iter().filter(|x| *x % 2 == 0).sum()\n}",
        "go": "package main\n\nfunc sumEven(nums []int) int {\n\tt := 0\n\tfor _, x := range nums {\n\t\tif x%2 == 0 {\n\t\t\tt += x\n\t\t}\n\t}\n\treturn t\n}",
    },
    "reverse_words": {
        "turi": "fn reverseWords(s: str) -> str {\n    var out = \"\"\n    var end = s.len()\n    var i = s.len() - 1\n    while i >= 0 {\n        if s[i] == ' ' {\n            out = out + s[i + 1..end] + \" \"\n            end = i\n        }\n        i -= 1\n    }\n    return out + s[0..end]\n}",
        "python": "def reverse_words(s):\n    return ' '.join(s.split(' ')[::-1])",
        "c": "#include <stdlib.h>\n#include <string.h>\nchar *reverse_words(const char *s) {\n    size_t n = strlen(s);\n    char *out = malloc(n + 1);\n    size_t o = 0; long end = (long)n;\n    for (long i = (long)n - 1; i >= -1; i--) {\n        if (i < 0 || s[i] == ' ') {\n            memcpy(out + o, s + i + 1, end - i - 1); o += end - i - 1;\n            if (i >= 0) out[o++] = ' ';\n            end = i;\n        }\n    }\n    out[o] = 0;\n    return out;\n}",
        "cpp": "#include <string>\n#include <vector>\nstd::string reverse_words(const std::string& s) {\n    std::vector<std::string> w; std::string cur;\n    for (char c : s) { if (c == ' ') { w.push_back(cur); cur.clear(); } else cur += c; }\n    w.push_back(cur);\n    std::string out;\n    for (size_t i = w.size(); i-- > 0;) { out += w[i]; if (i) out += ' '; }\n    return out;\n}",
        "rust": "fn reverse_words(s: &str) -> String {\n    s.split(' ').rev().collect::<Vec<_>>().join(\" \")\n}",
        "go": "package main\n\nimport \"strings\"\n\nfunc reverseWords(s string) string {\n\tw := strings.Split(s, \" \")\n\tfor i, j := 0, len(w)-1; i < j; i, j = i+1, j-1 {\n\t\tw[i], w[j] = w[j], w[i]\n\t}\n\treturn strings.Join(w, \" \")\n}",
    },
    "fizz_buzz": {
        "turi": "fn fizzBuzz(n: int) -> [str] {\n    var out: [str] = []\n    for i in 1..=n {\n        if i % 15 == 0 { out.push(\"FizzBuzz\") } else if i % 3 == 0 { out.push(\"Fizz\") } else if i % 5 == 0 { out.push(\"Buzz\") } else { out.push(str(i)) }\n    }\n    return out\n}",
        "python": "def fizz_buzz(n):\n    return ['FizzBuzz' if i % 15 == 0 else 'Fizz' if i % 3 == 0 else 'Buzz' if i % 5 == 0 else str(i) for i in range(1, n + 1)]",
        "c": "#include <stdlib.h>\n#include <stdio.h>\n#include <string.h>\nchar **fizz_buzz(long long n, int *out_len) {\n    char **r = malloc(sizeof(char *) * (n > 0 ? n : 1));\n    for (long long i = 1; i <= n; i++) {\n        char buf[32];\n        if (i % 15 == 0) strcpy(buf, \"FizzBuzz\"); else if (i % 3 == 0) strcpy(buf, \"Fizz\"); else if (i % 5 == 0) strcpy(buf, \"Buzz\"); else sprintf(buf, \"%lld\", i);\n        r[i - 1] = malloc(strlen(buf) + 1); strcpy(r[i - 1], buf);\n    }\n    *out_len = (int)n;\n    return r;\n}",
        "cpp": "#include <string>\n#include <vector>\nstd::vector<std::string> fizz_buzz(long long n) {\n    std::vector<std::string> r;\n    for (long long i = 1; i <= n; i++) r.push_back(i % 15 == 0 ? \"FizzBuzz\" : i % 3 == 0 ? \"Fizz\" : i % 5 == 0 ? \"Buzz\" : std::to_string(i));\n    return r;\n}",
        "rust": "fn fizz_buzz(n: i64) -> Vec<String> {\n    (1..=n).map(|i| if i % 15 == 0 { \"FizzBuzz\".to_string() } else if i % 3 == 0 { \"Fizz\".to_string() } else if i % 5 == 0 { \"Buzz\".to_string() } else { i.to_string() }).collect()\n}",
        "go": "package main\n\nimport \"strconv\"\n\nfunc fizzBuzz(n int) []string {\n\tout := []string{}\n\tfor i := 1; i <= n; i++ {\n\t\tswitch {\n\t\tcase i%15 == 0:\n\t\t\tout = append(out, \"FizzBuzz\")\n\t\tcase i%3 == 0:\n\t\t\tout = append(out, \"Fizz\")\n\t\tcase i%5 == 0:\n\t\t\tout = append(out, \"Buzz\")\n\t\tdefault:\n\t\t\tout = append(out, strconv.Itoa(i))\n\t\t}\n\t}\n\treturn out\n}",
    },
    "average": {
        "turi": "fn average(nums: [int]) -> float {\n    var t = 0\n    for x in nums { t += x }\n    return float(t) / float(nums.len())\n}",
        "python": "def average(nums):\n    return sum(nums) / len(nums)",
        "c": "double average(const long long *nums, int nums_len) {\n    long long t = 0;\n    for (int i = 0; i < nums_len; i++) t += nums[i];\n    return (double)t / nums_len;\n}",
        "cpp": "#include <vector>\ndouble average(const std::vector<long long>& nums) {\n    long long t = 0;\n    for (auto x : nums) t += x;\n    return (double)t / nums.size();\n}",
        "rust": "fn average(nums: &[i64]) -> f64 {\n    nums.iter().sum::<i64>() as f64 / nums.len() as f64\n}",
        "go": "package main\n\nfunc average(nums []int) float64 {\n\tt := 0\n\tfor _, x := range nums {\n\t\tt += x\n\t}\n\treturn float64(t) / float64(len(nums))\n}",
    },
    "words_starting_with_vowel": {
        "turi": "fn wordsStartingWithVowel(words: [str]) -> [str] {\n    var out: [str] = []\n    for w in words {\n        if w.len() > 0 {\n            let c = w[0]\n            if c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u' { out.push(w) }\n        }\n    }\n    return out\n}",
        "python": "def words_starting_with_vowel(words):\n    return [w for w in words if w[:1] in ('a','e','i','o','u')]",
        "c": "#include <stdlib.h>\n#include <string.h>\nchar **words_starting_with_vowel(const char **words, int words_len, int *out_len) {\n    char **r = malloc(sizeof(char *) * (words_len > 0 ? words_len : 1));\n    int n = 0;\n    for (int i = 0; i < words_len; i++) {\n        char c = words[i][0];\n        if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') { r[n] = malloc(strlen(words[i]) + 1); strcpy(r[n], words[i]); n++; }\n    }\n    *out_len = n;\n    return r;\n}",
        "cpp": "#include <string>\n#include <vector>\nstd::vector<std::string> words_starting_with_vowel(const std::vector<std::string>& words) {\n    std::vector<std::string> r;\n    for (auto& w : words) if (!w.empty() && std::string(\"aeiou\").find(w[0]) != std::string::npos) r.push_back(w);\n    return r;\n}",
        "rust": "fn words_starting_with_vowel(words: &[String]) -> Vec<String> {\n    words.iter().filter(|w| matches!(w.chars().next(), Some('a' | 'e' | 'i' | 'o' | 'u'))).cloned().collect()\n}",
        "go": "package main\n\nfunc wordsStartingWithVowel(words []string) []string {\n\tout := []string{}\n\tfor _, w := range words {\n\t\tif len(w) > 0 && (w[0] == 'a' || w[0] == 'e' || w[0] == 'i' || w[0] == 'o' || w[0] == 'u') {\n\t\t\tout = append(out, w)\n\t\t}\n\t}\n\treturn out\n}",
    },
    "is_palindrome": {
        "turi": "fn isPalindrome(s: str) -> bool {\n    let n = s.len()\n    for i in 0..n / 2 {\n        if s[i] != s[n - 1 - i] { return false }\n    }\n    return true\n}",
        "python": "def is_palindrome(s):\n    return s == s[::-1]",
        "c": "#include <stdbool.h>\n#include <string.h>\nbool is_palindrome(const char *s) {\n    size_t n = strlen(s);\n    for (size_t i = 0; i < n / 2; i++) if (s[i] != s[n - 1 - i]) return false;\n    return true;\n}",
        "cpp": "#include <string>\nbool is_palindrome(const std::string& s) {\n    return std::string(s.rbegin(), s.rend()) == s;\n}",
        "rust": "fn is_palindrome(s: &str) -> bool {\n    s.chars().eq(s.chars().rev())\n}",
        "go": "package main\n\nfunc isPalindrome(s string) bool {\n\tfor i, j := 0, len(s)-1; i < j; i, j = i+1, j-1 {\n\t\tif s[i] != s[j] {\n\t\t\treturn false\n\t\t}\n\t}\n\treturn true\n}",
    },
}


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = Path(tempfile.mkdtemp(prefix="aibench-self-"))
    bad = 0
    try:
        for lang in all_langs(compiler):
            if not lang.available():
                print(f"{lang.name:7s} toolchain missing")
                bad += 1
                continue
            for tname, sols in SOL.items():
                task = by_name(tname)
                r = run_candidate(lang, task, sols[lang.name], tmp / f"{tname}-{lang.name}")
                bad += not r["all_ok"]
                print(f"{lang.name:7s} {tname:28s} {'OK' if r['all_ok'] else 'FAIL ' + r['stage'] + ' ' + r['detail'][:400]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("failures:", bad)
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
