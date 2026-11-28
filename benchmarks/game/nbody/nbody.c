/* n-body, expert C baseline: struct-of-arrays not needed for n=5; this is the
   classic fastest scalar formulation (precomputed pair deltas, restrict, const n). */
#include "nbody_model.h"
#include <stdio.h>
#include <stdlib.h>

#define PAIRS 10

static void advance(int steps, double dt) {
    for (int s = 0; s < steps; s++) {
        double d[PAIRS][3], mag[PAIRS];
        int k = 0;
        for (int i = 0; i < BODY_COUNT - 1; i++)
            for (int j = i + 1; j < BODY_COUNT; j++, k++)
                for (int m = 0; m < 3; m++) d[k][m] = b[i].x[m] - b[j].x[m];
        for (k = 0; k < PAIRS; k++) {
            double d2 = d[k][0] * d[k][0] + d[k][1] * d[k][1] + d[k][2] * d[k][2];
            mag[k] = dt / (d2 * sqrt(d2));
        }
        k = 0;
        for (int i = 0; i < BODY_COUNT - 1; i++)
            for (int j = i + 1; j < BODY_COUNT; j++, k++)
                for (int m = 0; m < 3; m++) {
                    b[i].v[m] -= d[k][m] * b[j].mass * mag[k];
                    b[j].v[m] += d[k][m] * b[i].mass * mag[k];
                }
        for (int i = 0; i < BODY_COUNT; i++)
            for (int m = 0; m < 3; m++) b[i].x[m] += dt * b[i].v[m];
    }
}

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    offset_momentum();
    printf("%.9f\n", energy());
    advance(n, 0.01);
    printf("%.9f\n", energy());
    return 0;
}
