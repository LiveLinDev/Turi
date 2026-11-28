/* Los coeficientes de la sección fija proceden de section_tail_coefficients. */
static void fourth_tail(double left, double right, double *low, double *high) {
    double a = 1.0 / (left - 0.5), b = 1.0 / (right - 0.5), a3 = a * a * a;
    *high = (a3 - b * b * b) / 3.0;
    double error = (20.0 / 24.0) * (a3 * a3 + a3 * a * a * 0.2) + 1e-22;
    *low = fmax(0.0, *high - error);
}

static int finite_tail(long n, double *answer) {
    if (n < 1024 || n > 8192) return 0;
    const double scale = 3.788295591178222, shift = 4.3803453127855825;
    const double square = 1.4038422021934813, energy = 2.2793444334472777;
    double low, high, seed_low, seed_high;
    fourth_tail(1024.0, n, &low, &high);
    fourth_tail(1024.0 + shift, n + shift, &seed_low, &seed_high);
    double square_low = square + scale * scale * seed_low;
    double square_high = square + scale * scale * seed_high;
    double cross = 2 * scale * high;
    double energy_low = energy + 72.86659933815992 * low;
    double energy_high = energy + 74.79686378847606 * high + 1024.0 * cross * cross;
    double below = (energy_low / square_high) * (1.0 - 5e-13);
    double above = (energy_high / square_low) * (1.0 + 5e-13) + 8.220479893219967e-11 / (2.0 * below - 1.9);
    double roundoff = 176.0 * (n + 3) * 0x1p-53;
    low = sqrt(below * (1.0 - n * 0x1p-76) * (1.0 - roundoff));
    high = sqrt(above * (1.0 + roundoff));
    if ((long long)((low - 0x1p-47) * 1e9 + 0.5) != (long long)((high + 0x1p-47) * 1e9 + 0.5)) return 0;
    *answer = (low + high) * 0.5;
    return 1;
}
