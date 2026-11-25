/* fannkuch-redux, expert scalar C: same algorithm, 32-bit elements, restrict-free locals on the stack. */
#include <stdio.h>
#include <stdlib.h>

static int fannkuch(int n, long long *checksum_out) {
    int perm[32], perm1[33], count[33];
    for (int i = 0; i < n; i++) perm1[i] = i;
    int max_flips = 0, r = n;
    long long checksum = 0, perm_count = 0;
    for (;;) {
        for (; r != 1; r--) count[r - 1] = r;
        for (int i = 0; i < n; i++) perm[i] = perm1[i];
        int flips = 0, k;
        while ((k = perm[0]) != 0) {
            for (int i = 0, j = k; i < j; i++, j--) {
                int t = perm[i];
                perm[i] = perm[j];
                perm[j] = t;
            }
            flips++;
        }
        if (flips > max_flips) max_flips = flips;
        checksum += (perm_count % 2 == 0) ? flips : -flips;
        for (;;) {
            if (r == n) {
                *checksum_out = checksum;
                return max_flips;
            }
            int first = perm1[0];
            for (int i = 0; i < r; i++) perm1[i] = perm1[i + 1];
            perm1[r] = first;
            if (--count[r] > 0) break;
            r++;
        }
        perm_count++;
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 7;
    long long checksum = 0;
    int flips = fannkuch(n, &checksum);
    printf("%lld\n%d\n", checksum, flips);
    return 0;
}
