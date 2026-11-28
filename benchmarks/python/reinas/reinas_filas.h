/* Las claves son las cuatro columnas restantes relativas a la primera. */
static const uint16_t shared_gap[1024] = {
    [23] = 1, [27] = 5, [29] = 25, [30] = 125, [39] = 2, [43] = 6,
    [45] = 26, [46] = 126, [51] = 10, [53] = 30, [54] = 130, [57] = 50,
    [58] = 150, [60] = 250, [71] = 3, [75] = 7, [77] = 27, [78] = 127,
    [83] = 11, [85] = 31, [86] = 131, [89] = 51, [90] = 151, [92] = 251,
    [99] = 15, [101] = 35, [102] = 135, [105] = 55, [106] = 155, [108] = 255,
    [113] = 75, [114] = 175, [116] = 275, [120] = 375, [135] = 4, [139] = 8,
    [141] = 28, [142] = 128, [147] = 12, [149] = 32, [150] = 132, [153] = 52,
    [154] = 152, [156] = 252, [163] = 16, [165] = 36, [166] = 136, [169] = 56,
    [170] = 156, [172] = 256, [177] = 76, [178] = 176, [180] = 276, [184] = 376,
    [195] = 20, [197] = 40, [198] = 140, [201] = 60, [202] = 160, [204] = 260,
    [209] = 80, [210] = 180, [212] = 280, [216] = 380, [225] = 100, [226] = 200,
    [228] = 300, [232] = 400, [240] = 500, [263] = 4, [267] = 9, [269] = 29,
    [270] = 129, [275] = 13, [277] = 33, [278] = 133, [281] = 53, [282] = 153,
    [284] = 253, [291] = 17, [293] = 37, [294] = 137, [297] = 57, [298] = 157,
    [300] = 257, [305] = 77, [306] = 177, [308] = 277, [312] = 377, [323] = 21,
    [325] = 41, [326] = 141, [329] = 61, [330] = 161, [332] = 261, [337] = 81,
    [338] = 181, [340] = 281, [344] = 381, [353] = 101, [354] = 201, [356] = 301,
    [360] = 401, [368] = 501, [387] = 20, [389] = 45, [390] = 145, [393] = 65,
    [394] = 165, [396] = 265, [401] = 85, [402] = 185, [404] = 285, [408] = 385,
    [417] = 105, [418] = 205, [420] = 305, [424] = 405, [432] = 505, [449] = 100,
    [450] = 225, [452] = 325, [456] = 425, [464] = 525, [480] = 500, [519] = 4,
    [523] = 9, [525] = 29, [526] = 129, [531] = 14, [533] = 34, [534] = 134,
    [537] = 54, [538] = 154, [540] = 254, [547] = 18, [549] = 38, [550] = 138,
    [553] = 58, [554] = 158, [556] = 258, [561] = 78, [562] = 178, [564] = 278,
    [568] = 378, [579] = 22, [581] = 42, [582] = 142, [585] = 62, [586] = 162,
    [588] = 262, [593] = 82, [594] = 182, [596] = 282, [600] = 382, [609] = 102,
    [610] = 202, [612] = 302, [616] = 402, [624] = 502, [643] = 21, [645] = 46,
    [646] = 146, [649] = 66, [650] = 166, [652] = 266, [657] = 86, [658] = 186,
    [660] = 286, [664] = 386, [673] = 106, [674] = 206, [676] = 306, [680] = 406,
    [688] = 506, [705] = 101, [706] = 226, [708] = 326, [712] = 426, [720] = 526,
    [736] = 501, [771] = 20, [773] = 45, [774] = 145, [777] = 70, [778] = 170,
    [780] = 270, [785] = 90, [786] = 190, [788] = 290, [792] = 390, [801] = 110,
    [802] = 210, [804] = 310, [808] = 410, [816] = 510, [833] = 105, [834] = 230,
    [836] = 330, [840] = 430, [848] = 530, [864] = 505, [897] = 100, [898] = 225,
    [900] = 350, [904] = 450, [912] = 550, [928] = 525, [960] = 500,
};

#define TZ_REINAS_COLUMNAS UINT64_C(0x10008004002001)

static inline uint64_t available_five(uint32_t cols, uint32_t left, uint32_t right) {
    uint64_t vertical = cols * TZ_REINAS_COLUMNAS;
    uint64_t rising = ((uint64_t)(left & 2047) * UINT64_C(0x100040010004001)) & UINT64_C(0x7f03fc1ff0ffc7ff);
    uint64_t falling = ((uint64_t)right * UINT64_C(0x1001001001001)) & UINT64_C(0x7f07f87fc7fe7ff);
    return vertical & ~(rising | falling);
}

static unsigned count_five_available(uint32_t cols, uint64_t available) {
    const uint64_t limits = UINT64_C(0x8004002001000800);
    if ((((available | limits) - TZ_REINAS_COLUMNAS) & limits) != limits) return 0;
    unsigned gap = shared_gap[(cols >> __builtin_ctz(cols)) >> 1];
    uint64_t low = UINT64_MAX, high = UINT64_MAX;
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
    uint64_t grid = _pext_u64(available, cols * TZ_REINAS_COLUMNAS);
#ifdef TZ_REINAS_FILTRO
    uint64_t covered = grid | (grid >> 5) | (grid >> 10) | (grid >> 15) | (grid >> 20);
    if ((covered & 31) != 31) return 0;
#endif
#endif
    for (unsigned col = 0; col < 5; ++col) {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
        unsigned choice = (unsigned)((((grid & (UINT64_C(0x108421) << col)) * UINT64_C(0x111110)) >>
                                      (20 + col)) & 31);
#else
        unsigned first = (unsigned)__builtin_ctz(cols);
        uint64_t selected = (available >> first) & TZ_REINAS_COLUMNAS;
        unsigned choice = (unsigned)((selected * UINT64_C(0x1001001001001) >> 48) & 31);
        cols &= cols - 1;
#endif
        low &= five[2 * (32 * col + choice)];
        high &= five[2 * (32 * col + choice) + 1];
    }
    return (unsigned)(__builtin_popcountll(low & five[320 + 2 * gap]) +
                      __builtin_popcountll(high & five[321 + 2 * gap]));
}

static uint64_t count_six_shared(uint32_t cols, uint32_t left, uint32_t right) {
    static const uint64_t omit[] = {
        UINT64_C(0x21008802400a003), UINT64_C(0x420110048014007), UINT64_C(0x84022009002a00e),
        UINT64_C(0x108044012405401c), UINT64_C(0x21008882480a8038), UINT64_C(0x4211110490150070),
        UINT64_C(0x4222209202a00e0), UINT64_C(0x8404412405401c0), UINT64_C(0x1080880480a80380),
        UINT64_C(0x2101100900500700), UINT64_C(0x4202201200a00600)
    };
    uint32_t pending = cols & ~(left | right);
    if (!pending) return 0;
    uint64_t suffix = available_five(cols, left << 1, right >> 1);
    uint64_t total = 0;
    while (pending) {
        uint32_t queen = pending & (0u - pending);
        pending &= pending - 1;
        total += count_five_available(cols ^ queen, suffix & ~omit[__builtin_ctz(queen)]);
    }
    return total;
}

#ifdef TZ_REINAS_NIVELES
#define TZ_REINAS_NIVEL(name, child) \
    static uint64_t name(uint32_t cols, uint32_t left, uint32_t right) { \
        uint32_t pending = cols & ~(left | right); \
        uint64_t total = 0; \
        while (pending) { \
            uint32_t queen = pending & (0u - pending); \
            pending &= pending - 1; \
            total += child(cols ^ queen, (left | queen) << 1, (right | queen) >> 1); \
        } \
        return total; \
    }

TZ_REINAS_NIVEL(count_seven_shared, count_six_shared)
TZ_REINAS_NIVEL(count_eight_shared, count_seven_shared)
TZ_REINAS_NIVEL(count_nine_shared, count_eight_shared)
TZ_REINAS_NIVEL(count_ten_shared, count_nine_shared)
#undef TZ_REINAS_NIVEL
#endif
#undef TZ_REINAS_COLUMNAS
