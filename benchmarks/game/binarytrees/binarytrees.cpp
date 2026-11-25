// binary-trees, idiomatic C++: std::unique_ptr ownership.
#include <cstdio>
#include <cstdlib>
#include <memory>

struct Node { std::unique_ptr<Node> left, right; };

static std::unique_ptr<Node> build(int depth) {
    auto n = std::make_unique<Node>();
    if (depth > 0) {
        n->left = build(depth - 1);
        n->right = build(depth - 1);
    }
    return n;
}

static long long check(const Node& n) { return n.left ? 1 + check(*n.left) + check(*n.right) : 1; }

int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 10;
    int min_depth = 4, max_depth = min_depth + 2 > n ? min_depth + 2 : n, stretch = max_depth + 1;
    std::printf("%lld\n", check(*build(stretch)));
    auto long_tree = build(max_depth);
    for (int depth = min_depth; depth <= max_depth; depth += 2) {
        long long iterations = 1LL << (max_depth - depth + min_depth), total = 0;
        for (long long i = 0; i < iterations; i++) total += check(*build(depth));
        std::printf("%lld\n", total);
    }
    std::printf("%lld\n", check(*long_tree));
    return 0;
}
