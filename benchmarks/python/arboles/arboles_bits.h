/* Un bit por nodo. La posición de cada rama determina sus dos enlaces. */
typedef uint64_t NodeWord;

#ifdef TZ_ARBOLES_RAMAS
enum { LEAF_BITS = 0 };
#else
enum { LEAF_BITS = 1 };
#endif

static void build(uint64_t *nodes, int depth) {
    size_t leaves = (size_t)1 << depth;
    if (leaves < 64) {
        nodes[0] = (UINT64_C(1) << leaves) - 2;
        return;
    }
    size_t words = leaves / 64;
    memset(nodes, 0xff, words * sizeof(*nodes));
    nodes[0] = ~UINT64_C(1);
    if (LEAF_BITS) memset(nodes + words, 0, words * sizeof(*nodes));
}

static uint64_t count_tree(const uint64_t *nodes, int depth) {
    size_t words = (((size_t)1 << (depth + LEAF_BITS)) + 63) / 64;
    uint64_t total = 0;
    for (size_t i = 0; i < words; i++) total += __builtin_popcountll(nodes[i]);
    return 2 * total + 1;
}

#ifdef TZ_ARBOLES_PLANOS
/* El nodo i del árbol j ocupa el bit i*trees+j. */
static void build_forest(uint64_t *nodes, int depth, size_t trees) {
    size_t leaves = (size_t)1 << depth;
    if (leaves < 64) {
        nodes[0] = (UINT64_C(1) << leaves) - (UINT64_C(1) << trees);
        return;
    }
    size_t words = leaves / 64, sentinels = trees / 64;
    memset(nodes, 0, sentinels * sizeof(*nodes));
    memset(nodes + sentinels, 0xff, (words - sentinels) * sizeof(*nodes));
    if (LEAF_BITS) memset(nodes + words, 0, words * sizeof(*nodes));
    if (!sentinels) nodes[0] = UINT64_MAX << trees;
}
#endif
