#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef AUDIT
void *audit_malloc(size_t);
void audit_free(void *);
#define KALLOC audit_malloc
#define KFREE audit_free
#else
#define KALLOC malloc
#define KFREE free
#endif

typedef struct { int64_t *p; int64_t n; } Vec;

static Vec make_vec(int64_t n, int64_t value) {
    Vec out = { KALLOC(sizeof(int64_t) * n), n };
    if (!out.p && n) abort();
    for (int64_t i = 0; i < n; ++i) out.p[i] = value;
    return out;
}
static Vec clone_vec(Vec src) {
    Vec out = { KALLOC(sizeof(int64_t) * src.n), src.n };
    if (!out.p && src.n) abort();
    memcpy(out.p, src.p, src.n * sizeof(int64_t));
    return out;
}
static void dispose(Vec *vec) { KFREE(vec->p); }
