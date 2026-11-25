/* spectral-norm, expert single-thread C: restrict, and the division vectorized by hand over two
   independent accumulators (the same reassociation the fastest public entries use). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static inline double eval_a(long i, long j) { return 1.0 / (double)((i + j) * (i + j + 1) / 2 + i + 1); }

static void times_a(const double* restrict u, double* restrict out, long n) {
    for (long i = 0; i < n; i++) {
        double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        long j = 0;
        for (; j + 4 <= n; j += 4) {
            a0 += eval_a(i, j) * u[j];
            a1 += eval_a(i, j + 1) * u[j + 1];
            a2 += eval_a(i, j + 2) * u[j + 2];
            a3 += eval_a(i, j + 3) * u[j + 3];
        }
        for (; j < n; j++) a0 += eval_a(i, j) * u[j];
        out[i] = (a0 + a1) + (a2 + a3);
    }
}

static void times_at(const double* restrict u, double* restrict out, long n) {
    for (long i = 0; i < n; i++) {
        double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        long j = 0;
        for (; j + 4 <= n; j += 4) {
            a0 += eval_a(j, i) * u[j];
            a1 += eval_a(j + 1, i) * u[j + 1];
            a2 += eval_a(j + 2, i) * u[j + 2];
            a3 += eval_a(j + 3, i) * u[j + 3];
        }
        for (; j < n; j++) a0 += eval_a(j, i) * u[j];
        out[i] = (a0 + a1) + (a2 + a3);
    }
}

int main(int argc, char** argv) {
    long n = argc > 1 ? atol(argv[1]) : 100;
    double* u = malloc(sizeof(double) * (size_t)n);
    double* v = malloc(sizeof(double) * (size_t)n);
    double* tmp = malloc(sizeof(double) * (size_t)n);
    for (long i = 0; i < n; i++) { u[i] = 1.0; v[i] = 0.0; }
    for (int step = 0; step < 10; step++) {
        times_a(u, tmp, n);
        times_at(tmp, v, n);
        times_a(v, tmp, n);
        times_at(tmp, u, n);
    }
    double vBv = 0.0, vv = 0.0;
    for (long i = 0; i < n; i++) { vBv += u[i] * v[i]; vv += v[i] * v[i]; }
    printf("%0.9f\n", sqrt(vBv / vv));
    return 0;
}
