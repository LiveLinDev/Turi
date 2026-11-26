/* Dijkstra en cuadrícula, C experto: montículo binario propio de (distancia, celda) con entradas
   repetidas que se descartan al salir (el mismo algoritmo que heapq), distancias en un arreglo plano. */
#include <stdio.h>
#include <stdlib.h>

typedef struct { long long d; int cell; } Item;
static Item *heap;
static size_t size, cap;

static void push(long long d, int cell) {
    if (size == cap) {
        cap = cap ? cap * 2 : 1024;
        heap = realloc(heap, cap * sizeof(Item));
    }
    size_t i = size++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (heap[parent].d <= d) break;
        heap[i] = heap[parent];
        i = parent;
    }
    heap[i].d = d;
    heap[i].cell = cell;
}

static Item pop(void) {
    Item top = heap[0], last = heap[--size];
    size_t i = 0;
    for (;;) {
        size_t child = 2 * i + 1;
        if (child >= size) break;
        if (child + 1 < size && heap[child + 1].d < heap[child].d) child++;
        if (last.d <= heap[child].d) break;
        heap[i] = heap[child];
        i = child;
    }
    if (size) heap[i] = last;
    return top;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 100;
    long long *dist = malloc(sizeof(long long) * (size_t)n * (size_t)n);
    for (long i = 0; i < (long)n * n; i++) dist[i] = 1000000000000000000LL;
    dist[0] = 0;
    push(0, 0);
    static const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
    while (size) {
        Item it = pop();
        int i = it.cell / n, j = it.cell % n;
        if (it.d > dist[it.cell]) continue;
        for (int k = 0; k < 4; k++) {
            int ni = i + di[k], nj = j + dj[k];
            if (ni < 0 || ni >= n || nj < 0 || nj >= n) continue;
            long long nd = it.d + (ni * 7 + nj * 13) % 10 + 1;
            int cell = ni * n + nj;
            if (nd < dist[cell]) {
                dist[cell] = nd;
                push(nd, cell);
            }
        }
    }
    long long total = 0;
    for (long i = 0; i < (long)n * n; i++) total += dist[i];
    printf("%lld %lld\n", dist[(long)n * n - 1], total);
    free(dist);
    free(heap);
    return 0;
}
