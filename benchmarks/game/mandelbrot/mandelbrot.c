/* Mandelbrot, expert portable C: 8 pixels per step, branch-free inner loops the compiler vectorizes. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    long long n = argc > 1 ? atoll(argv[1]) : 200;
    double inv = 2.0 / (double)n;
    double cr[8], zr[8], zi[8];
    long long inside = 0, checksum = 0;
    for (long long y = 0; y < n; y++) {
        double ci = (double)y * inv - 1.0;
        for (long long x = 0; x < n; x += 8) {
            for (int k = 0; k < 8; k++) {
                cr[k] = (double)(x + k) * inv - 1.5;
                zr[k] = 0.0;
                zi[k] = 0.0;
            }
            for (int iter = 0; iter < 50; iter++) {
                for (int k = 0; k < 8; k++) {
                    double r = zr[k], i = zi[k];
                    zr[k] = r * r - i * i + cr[k];
                    zi[k] = 2.0 * r * i + ci;
                }
            }
            long long byte = 0;
            for (int k = 0; k < 8; k++) {
                byte *= 2;
                if (x + k < n && zr[k] * zr[k] + zi[k] * zi[k] <= 4.0) {
                    byte += 1;
                    inside += 1;
                }
            }
            checksum = (checksum * 31 + byte) % 1000000007;
        }
    }
    printf("%lld\n%lld\n", inside, checksum);
    return 0;
}
