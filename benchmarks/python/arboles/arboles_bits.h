/* Un bit por nodo. La posición de cada rama determina sus dos enlaces. */
typedef uint64_t NodeWord;

static void build(uint64_t *nodes, int depth) {
    size_t leaves = (size_t)1 << depth;
    if (leaves < 64) {
        nodes[0] = (UINT64_C(1) << leaves) - 2;
        return;
    }
    size_t words = leaves / 64;
    memset(nodes, 0xff, words * sizeof(*nodes));
    nodes[0] = ~UINT64_C(1);
    memset(nodes + words, 0, words * sizeof(*nodes));
}

static uint64_t count_tree(const uint64_t *nodes, int depth) {
    size_t words = (((size_t)1 << (depth + 1)) + 63) / 64;
    uint64_t total = 0;
    for (size_t i = 0; i < words; i++) total += __builtin_popcountll(nodes[i]);
    return 2 * total + 1;
}
