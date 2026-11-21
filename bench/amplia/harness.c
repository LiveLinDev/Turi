#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
static double ns(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e9 / f.QuadPart;
}
static uint64_t rss(void) {
    PROCESS_MEMORY_COUNTERS mem = {sizeof(mem)};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem))) abort();
    return mem.PeakWorkingSetSize;
}
#else
#include <time.h>
static double ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + t.tv_nsec;
}
static uint64_t rss(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256]; unsigned long kb;
    while (f && fgets(line,sizeof(line),f)) {
        if (sscanf(line,"VmHWM: %lu kB",&kb) == 1) { fclose(f); return (uint64_t)kb*1024; }
    }
    abort();
}
#endif

static uint64_t live, peak, allocs, requested;
typedef union { max_align_t alignment; size_t size; } Header;
void *audit_malloc(size_t size) {
    Header *p = malloc(sizeof(*p) + size);
    if (!p) return NULL;
    p->size = size; ++allocs; requested += size; live += size;
    if (live > peak) peak = live;
    return p + 1;
}
void audit_free(void *ptr) {
    if (ptr) { Header *base = (Header *)ptr-1; live -= base->size; free(base); }
}

extern int64_t bench_case(int64_t, int64_t, int64_t);
#ifdef GOOSE
extern void bench_init(int, char **);
#endif
int main(int argc, char **argv) {
    if (argc != 5) return 2;
    int64_t id = atoll(argv[1]), n = atoll(argv[2]), seed = atoll(argv[3]);
    int64_t rounds = atoll(argv[4]), sum = 0;
#ifdef GOOSE
    bench_init(argc,argv);
#endif
    double begin = ns();
    for (int64_t i=0; i<rounds; ++i)
        sum += bench_case(id, n, seed + (i % 4) * 7);
    double elapsed = ns() - begin;
    printf("%" PRId64 " %.0f %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
           sum, elapsed, rss(), allocs, requested, peak, live);
    return live ? 3 : 0;
}
