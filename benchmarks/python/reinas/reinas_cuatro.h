#include <immintrin.h>

static const uint32_t allowed[128] = {
    0, 63, 798912, 798975, 5326080, 5326143, 6124992, 6125055,
    10652160, 10652223, 11451072, 11451135, 15978240, 15978303, 16777152, 16777215,
    0, 4032, 3194883, 3198915, 8785940, 8789972, 11980823, 11984855,
    4792360, 4796392, 7987243, 7991275, 13578300, 13582332, 16773183, 16777215,
    0, 258048, 12583692, 12841740, 2623585, 2881633, 15207277, 15465325,
    1311890, 1569938, 13895582, 14153630, 3935475, 4193523, 16519167, 16777215,
    0, 16515072, 199728, 16714800, 41610, 16556682, 241338, 16756410,
    20805, 16535877, 220533, 16735605, 62415, 16577487, 262143, 16777215,
    9216, 2056, 11272, 11272, 2106372, 544, 2631460, 2631460,
    2654484, 2600, 2657084, 2657084, 2663700, 2600, 2666300, 2666300,
    1052672, 1120392, 4262088, 5314760, 278528, 8733345, 8454817, 8733345,
    1331200, 14047851, 12716651, 14047851, 1331200, 14113515, 12782315, 14113515,
    1061888, 1249410, 4400322, 5452994, 2417684, 8732801, 11117973, 11396501,
    3985684, 14045251, 15368535, 16699735, 3994900, 14110915, 15443415, 16774615,
    1061888, 1251466, 4402378, 5455050, 2417684, 8733345, 11118517, 11397045,
    3985684, 14047851, 15371135, 16702335, 3994900, 14113515, 15446015, 16777215
};

static unsigned bits(uint64_t availability, unsigned column) {
    const uint64_t keep = UINT64_C(0x0001000100010001);
#ifdef TZ_REINAS_BMI2
    return (unsigned)_pext_u64(availability, keep << column);
#else
    return (unsigned)((((availability >> column) & keep) * UINT64_C(0x0001000200040008)) >> 48) & 15;
#endif
}

static unsigned closed_four(uint32_t c, uint32_t l, uint32_t r) {
    uint32_t first = c & ~(l | r);
    if (!first) return 0;
    uint32_t second = c & ~((l << 1) | (r >> 1));
    uint32_t third = c & ~((l << 2) | (r >> 2));
    uint32_t last = c & ~((l << 3) | (r >> 3));
    uint64_t availability = first | ((uint64_t)second << 16) | ((uint64_t)third << 32) | ((uint64_t)last << 48);
    unsigned a = __builtin_ctz(c); c &= c - 1;
    unsigned b = __builtin_ctz(c); c &= c - 1;
    unsigned x = __builtin_ctz(c); c &= c - 1;
    unsigned d = __builtin_ctz(c);
    unsigned ba = b - a, xb = x - b, dx = d - x;
    unsigned gap = ((ba < 4 ? ba : 4) - 1) * 16 + ((xb < 4 ? xb : 4) - 1) * 4 + ((dx < 4 ? dx : 4) - 1);
    return __builtin_popcount(allowed[bits(availability, a)] & allowed[16 + bits(availability, b)] &
                              allowed[32 + bits(availability, x)] & allowed[48 + bits(availability, d)] & allowed[64 + gap]);
}
