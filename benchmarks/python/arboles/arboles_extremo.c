/* Árboles completos en un almacén por niveles. Se escriben todos los nodos y
   el recorrido sigue ambas referencias. Cada nodo guarda dos índices de 32 bits. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __AVX512F__
#error Este banco requiere AVX-512F
#endif

#if defined(TZ_ARBOLES_BITS)
#include "arboles_bits.h"
#elif defined(TZ_ARBOLES_BYTES)
#include "arboles_bytes.h"
#elif defined(TZ_ARBOLES_COMPACTOS)
typedef uint32_t NodeWord;

static void build(uint32_t *nodes, int depth) {
    size_t leaves = (size_t)1 << depth;
#ifdef TZ_ARBOLES_BLOQUES
    __m512i positions = _mm512_setr_epi32(0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30);
    const __m512i advance = _mm512_set1_epi32(32);
    const __m512i zero = _mm512_setzero_si512();
    for (size_t i = 0; i < leaves; i += 16) {
        _mm512_storeu_si512(nodes + i, positions);
        _mm512_storeu_si512(nodes + leaves + i, zero);
        positions = _mm512_add_epi32(positions, advance);
    }
#else
    __m512i positions = _mm512_setr_epi32(2,4,6,8,10,12,14,16,18,20,22,24,26,28,30,32);
    const __m512i advance = _mm512_set1_epi32(32);
    size_t i = 1;
    for (; i + 16 <= leaves; i += 16) {
        _mm512_storeu_si512(nodes + i, positions);
        positions = _mm512_add_epi32(positions, advance);
    }
    for (; i < leaves; i++) nodes[i] = (uint32_t)(2 * i);
    memset(nodes + leaves, 0, leaves * sizeof(*nodes));
#endif
}

#ifdef TZ_ARBOLES_LINEAL
static uint64_t check(const uint32_t *nodes, size_t end) {
#ifdef TZ_ARBOLES_BLOQUES
    const __m512i zero = _mm512_setzero_si512();
#ifdef TZ_ARBOLES_POPCNT
    unsigned total = 0;
    for (size_t at = 0; at < end; at += 16) {
        __m512i words = _mm512_loadu_si512(nodes + at);
        total += (unsigned)__builtin_popcount(_mm512_cmpneq_epi32_mask(words, zero));
    }
    return 2 * total + 1;
#else
    const __m512i one = _mm512_set1_epi32(1);
    __m512i total = zero;
    for (size_t at = 0; at < end; at += 16) {
        __m512i words = _mm512_loadu_si512(nodes + at);
        total = _mm512_mask_add_epi32(total, _mm512_cmpneq_epi32_mask(words, zero), total, one);
    }
    return 2 * (uint32_t)_mm512_reduce_add_epi32(total) + 1;
#endif
#else
    size_t at = 1;
    const __m512i zero = _mm512_setzero_si512();
    __m512i total = zero;
    for (; at + 16 <= end; at += 16) {
        __m512i words = _mm512_loadu_si512(nodes + at);
        total = _mm512_mask_add_epi32(total, _mm512_cmpneq_epi32_mask(words, zero),
                                      total, _mm512_set1_epi32(1));
    }
    uint64_t internal = (uint32_t)_mm512_reduce_add_epi32(total);
    for (; at < end; at++) internal += nodes[at] != 0;
    return 2 * internal + 1;
#endif
}
#else
static uint64_t check(const uint32_t *nodes, uint32_t index) {
    uint32_t left = nodes[index];
    if (!left) return 1;
    return 1 + check(nodes, left) + check(nodes, left + 1);
}
#endif
#else
typedef uint64_t NodeWord;

static uint64_t children(uint64_t index) {
    return (2 * index) | ((2 * index + 1) << 32);
}

static void build(uint64_t *nodes, int depth) {
    size_t first_leaf = (size_t)1 << depth;
    __m512i pair = _mm512_setr_epi64(children(1), children(2), children(3), children(4),
                                   children(5), children(6), children(7), children(8));
    const __m512i advance = _mm512_set1_epi64(8 * INT64_C(0x200000002));
    size_t i = 1;
    for (; i + 8 <= first_leaf; i += 8) {
        _mm512_storeu_si512(nodes + i, pair);
        pair = _mm512_add_epi64(pair, advance);
    }
    for (; i < first_leaf; i++) nodes[i] = children(i);
    memset(nodes + first_leaf, 0, first_leaf * sizeof(*nodes));
}

static uint64_t check(const uint64_t *nodes, uint32_t index) {
    uint64_t node = nodes[index];
    uint32_t left = (uint32_t)node;
    if (!left) return 1;
    return 1 + check(nodes, left) + check(nodes, (uint32_t)(node >> 32));
}
#endif

#if !defined(TZ_ARBOLES_BYTES) && !defined(TZ_ARBOLES_BITS)
static uint64_t count_tree(const NodeWord *nodes, int depth) {
#ifdef TZ_ARBOLES_LINEAL
    return check(nodes, (size_t)1 << (depth + 1));
#else
    return check(nodes, 1);
#endif
}
#endif

static size_t storage_size(int depth) {
    size_t nodes = (size_t)1 << (depth + 1);
#ifdef TZ_ARBOLES_BITS
    return (nodes + 63) / 64;
#else
    return nodes;
#endif
}

int main(int argc, char **argv) {
    long requested = argc > 1 ? strtol(argv[1], NULL, 10) : 10;
    if (requested > 25) return EXIT_FAILURE;
    int max_depth = requested < 6 ? 6 : (int)requested;
    double start = c_extremo_inicio();
    NodeWord *scratch = malloc(sizeof(*scratch) * storage_size(max_depth + 1));
    NodeWord *long_tree = malloc(sizeof(*long_tree) * storage_size(max_depth));
    if (!scratch || !long_tree) {
        free(scratch);
        free(long_tree);
        return EXIT_FAILURE;
    }
    build(scratch, max_depth + 1);
    printf("%llu\n", (unsigned long long)count_tree(scratch, max_depth + 1));
    build(long_tree, max_depth);
    for (int depth = 4; depth <= max_depth; depth += 2) {
        uint64_t iterations = UINT64_C(1) << (max_depth - depth + 4);
        uint64_t total = 0;
        for (uint64_t i = 0; i < iterations; i++) {
            build(scratch, depth);
            total += count_tree(scratch, depth);
        }
        printf("%llu\n", (unsigned long long)total);
    }
    printf("%llu\n", (unsigned long long)count_tree(long_tree, max_depth));
    free(long_tree);
    free(scratch);
    c_extremo_fin(start);
    return EXIT_SUCCESS;
}
