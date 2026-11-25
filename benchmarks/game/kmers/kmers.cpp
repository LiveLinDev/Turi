// k-mer counting, idiomatic modern C++: unordered_map keyed by string_view into the sequence.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>

int main(int argc, char** argv) {
    long long n = argc > 1 ? std::atoll(argv[1]) : 10000;
    const int k = 8;
    std::string seq;
    seq.reserve(static_cast<size_t>(n));
    long long seed = 42;
    for (long long i = 0; i < n; i++) {
        seed = (seed * 1103515245 + 12345) % 2147483648LL;
        seq.push_back("ACGT"[(seed / 65536) % 4]);
    }
    std::unordered_map<std::string_view, long long> counts;
    std::string_view view(seq);
    for (long long i = 0; i + k <= n; i++) counts[view.substr(static_cast<size_t>(i), k)]++;
    long long best = 0, checksum = 0;
    for (const auto& [key, c] : counts) {
        if (c > best) best = c;
        checksum = (checksum + c * c) % 1000000007LL;
    }
    std::printf("%zu\n%lld\n%lld\n", counts.size(), best, checksum);
    return 0;
}
