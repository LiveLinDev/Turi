#include <cstdint>
#include <iostream>
#include <vector>

using Integer = std::int64_t;
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    Integer seed;
    if (!(std::cin >> seed) || seed <= 0) return 1;
    Integer total = 0;
#if MODE == 1
    total = seed;
    for (Integer i = 0; i < 8000000; ++i)
        total = (total * 48271 + 1) % 2147483647;
#elif MODE == 2
    constexpr Integer n = 262144;
    std::vector<Integer> values(n, seed);
    for (Integer i = 0; i < n; ++i) values[i] = (i + seed) % 1009;
    for (Integer p = 0; p < 32; ++p)
        for (auto& value : values) value = (value + p + 1) % 1009;
    for (auto value : values) total += value;
#elif MODE == 3
    for (Integer r = 0; r < 20000; ++r) {
        auto n = 32 + (r + seed) % 2017;
        std::vector<Integer> values(n, r % 251);
        for (Integer i = 0; i < n; ++i) values[i] = (values[i] + i + seed) % 251;
        total += values[(r + seed) % n];
    }
#else
#error select MODE 1, 2 or 3
#endif
    std::cout << total << '\n';
}
