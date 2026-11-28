#pragma once

#include <math.h>

enum { BODY_COUNT = 5 };
#define PI 3.141592653589793
#define SOLAR_MASS (4 * PI * PI)
#define DAYS 365.24

typedef struct { double x[3], v[3], mass; } body;

static body b[BODY_COUNT] = {
    {{0, 0, 0}, {0, 0, 0}, SOLAR_MASS},
    {{4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01},
     {1.66007664274403694e-03 * DAYS, 7.69901118419740425e-03 * DAYS, -6.90460016972063023e-05 * DAYS},
     9.54791938424326609e-04 * SOLAR_MASS},
    {{8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01},
     {-2.76742510726862411e-03 * DAYS, 4.99852801234917238e-03 * DAYS, 2.30417297573763929e-05 * DAYS},
     2.85885980666130812e-04 * SOLAR_MASS},
    {{1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01},
     {2.96460137564761618e-03 * DAYS, 2.37847173959480950e-03 * DAYS, -2.96589568540237556e-05 * DAYS},
     4.36624404335156298e-05 * SOLAR_MASS},
    {{1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01},
     {2.68067772490389322e-03 * DAYS, 1.62824170038242295e-03 * DAYS, -9.51592254519715870e-05 * DAYS},
     5.15138902046611451e-05 * SOLAR_MASS}};

static double energy(void) {
    double e = 0.0;
    for (int i = 0; i < BODY_COUNT; i++) {
        e += 0.5 * b[i].mass * (b[i].v[0] * b[i].v[0] + b[i].v[1] * b[i].v[1] + b[i].v[2] * b[i].v[2]);
        for (int j = i + 1; j < BODY_COUNT; j++) {
            double dx = b[i].x[0] - b[j].x[0], dy = b[i].x[1] - b[j].x[1], dz = b[i].x[2] - b[j].x[2];
            e -= b[i].mass * b[j].mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void offset_momentum(void) {
    double p[3] = {0, 0, 0};
    for (int i = 0; i < BODY_COUNT; i++)
        for (int m = 0; m < 3; m++) p[m] += b[i].v[m] * b[i].mass;
    for (int m = 0; m < 3; m++) b[0].v[m] = -p[m] / SOLAR_MASS;
}
