/* binary-trees, expert C: one bump arena per tree, released in a single free (the technique the
   fastest public entries use through memory pools). */
#include <stdio.h>
#include <stdlib.h>

typedef struct Node { struct Node *left, *right; } Node;
typedef struct { Node *nodes; size_t used; } Arena;

static Arena arena_new(int depth) {
    Arena a = { malloc(sizeof(Node) * (((size_t)2 << depth) - 1)), 0 };
    return a;
}

static Node *build(Arena *a, int depth) {
    Node *n = &a->nodes[a->used++];
    if (depth == 0) {
        n->left = n->right = NULL;
    } else {
        n->left = build(a, depth - 1);
        n->right = build(a, depth - 1);
    }
    return n;
}

static long long check(const Node *n) {
    return n->left ? 1 + check(n->left) + check(n->right) : 1;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 10;
    int min_depth = 4, max_depth = min_depth + 2 > n ? min_depth + 2 : n, stretch = max_depth + 1;
    Arena sa = arena_new(stretch);
    printf("%lld\n", check(build(&sa, stretch)));
    free(sa.nodes);
    Arena la = arena_new(max_depth);
    Node *long_tree = build(&la, max_depth);
    for (int depth = min_depth; depth <= max_depth; depth += 2) {
        long long iterations = 1LL << (max_depth - depth + min_depth), total = 0;
        Arena a = arena_new(depth);
        for (long long i = 0; i < iterations; i++) {
            a.used = 0;
            total += check(build(&a, depth));
        }
        free(a.nodes);
        printf("%lld\n", total);
    }
    printf("%lld\n", check(long_tree));
    free(la.nodes);
    return 0;
}
