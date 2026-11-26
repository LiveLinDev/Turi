/* Producto de matrices, C experto: memoria contigua, orden i-k-j (la fila de b se recorre seguida y
   el compilador vectoriza el bucle interior), punteros restrict. Mismo resultado que el orden i-j-k:
   cada c[i][j] suma sus productos en el mismo orden de k. */
#include <stdio.h>
#include <stdlib.h>

static double *make(int n, int seed) {
    double *m = malloc(sizeof(double) * (size_t)n * (size_t)n);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) m[(size_t)i * n + j] = ((i * 31 + j * 17 + seed) % 97) / 97.0;
    return m;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200;
    double *restrict a = make(n, 1), *restrict b = make(n, 2);
    double *restrict c = calloc((size_t)n * (size_t)n, sizeof(double));
    for (int i = 0; i < n; i++) {
        double *restrict ci = c + (size_t)i * n;
        for (int k = 0; k < n; k++) {
            double aik = a[(size_t)i * n + k];
            const double *restrict bk = b + (size_t)k * n;
            for (int j = 0; j < n; j++) ci[j] += aik * bk[j];
        }
    }
    double trace = 0.0, total = 0.0;
    for (int i = 0; i < n; i++) trace += c[(size_t)i * n + i];
    for (int i = 0; i < n; i++) {
        double row = 0.0;
        for (int j = 0; j < n; j++) row += c[(size_t)i * n + j];
        total += row;
    }
    printf("%.6f %.6f\n", trace, total);
    free(a);
    free(b);
    free(c);
    return 0;
}
