#pragma once

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

static double c_extremo_ahora(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) return 0.0;
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec clock;
    if (clock_gettime(CLOCK_MONOTONIC, &clock) != 0) return 0.0;
    return (double)clock.tv_sec + (double)clock.tv_nsec * 1e-9;
#endif
}

static double c_extremo_inicio(void) {
#ifdef _WIN32
    if (GetEnvironmentVariableA("TURIPY_C_TIME", NULL, 0) == 0) return 0.0;
#else
    if (!getenv("TURIPY_C_TIME")) return 0.0;
#endif
    return c_extremo_ahora();
}

static void c_extremo_fin(double start) {
    if (start > 0.0) fprintf(stderr, "C_EXTREMO_MS=%.6f\n", (c_extremo_ahora() - start) * 1000.0);
}
