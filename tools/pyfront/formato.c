/* Formato compartido por el motor híbrido y el ejecutable. */
#if defined(__AVX512F__) && defined(__FMA__)
#include <xmmintrin.h>
#endif
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#endif

int32_t tzx_queens_has_bmi2(void) {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
    return 1;
#else
    return 0;
#endif
}

uint64_t tzx_queens_grid(uint64_t data, uint64_t selected) {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
    return _pext_u64(data, selected);
#else
    uint64_t result = 0, place = 1;
    for (; selected; selected &= selected - 1, place <<= 1) {
        uint64_t picked = selected & (UINT64_C(0) - selected);
        if (data & picked) result |= place;
    }
    return result;
#endif
}

tz_i64 tzx_wide_simd(void) {
#if defined(__AVX512F__) && defined(__FMA__)
    /* La corrección de los recíprocos supone redondeo al más cercano. */
    return (_mm_getcsr() & _MM_ROUND_MASK) == _MM_ROUND_NEAREST;
#else
    return 0;
#endif
}

TZ_EXPORT tz_i64 tzx_fbuf(void) { return (tz_i64)tz_scratch(512); }
/* ---- Coma flotante como la escribe Python. Dígitos más cortos que vuelven a dar el mismo número
   (los de repr); entre los igual de cortos, el más cercano. */
static int tz_digits(double x, char* digits, int* decpt) {
    char tmp[64];
    for (int p = 0; p < 17; p++) {
        snprintf(tmp, sizeof tmp, "%.*e", p, x);
        if (strtod(tmp, 0) != x) {
            /* El más cercano con p+1 cifras no vale: puede valer uno de sus vecinos. */
            unsigned long long m = 0;
            int n = 0;
            const char* c = tmp;
            for (; *c && *c != 'e'; c++) if (*c >= '0' && *c <= '9') { m = m * 10 + (unsigned)(*c - '0'); n++; }
            int e = atoi(c + 1);
            unsigned long long top = 1;
            for (int i = 0; i < n; i++) top *= 10;
            int found = 0;
            for (int side = 0; side < 2 && !found; side++) {
                unsigned long long mm = m;
                int ee = e;
                if (side == 0) { mm = m + 1; if (mm == top) { mm = top / 10; ee = e + 1; } }
                else { if (m == top / 10) { mm = top - 1; ee = e - 1; } else mm = m - 1; }
                char cand[64], ds[32];
                snprintf(ds, sizeof ds, "%0*llu", n, mm);
                snprintf(cand, sizeof cand, "%c.%se%d", ds[0], ds + 1, ee);
                if (strtod(cand, 0) == x) {
                    memcpy(digits, ds, (size_t)n);
                    digits[n] = 0;
                    *decpt = ee + 1;
                    found = 1;
                }
            }
            if (found) goto trim;
            continue;
        }
        {
            int n = 0;
            const char* c = tmp;
            for (; *c && *c != 'e'; c++) if (*c >= '0' && *c <= '9') digits[n++] = *c;
            digits[n] = 0;
            *decpt = atoi(c + 1) + 1;
        }
    trim:
        {
            int n = (int)strlen(digits);
            while (n > 1 && digits[n - 1] == '0') digits[--n] = 0;
            return n;
        }
    }
    return 0;
}
static int tz_repr_into(double x, char* out) {
    char* o = out;
    if (x != x) { memcpy(out, "nan", 3); return 3; }
    if (x == __builtin_inf() || x == -__builtin_inf()) {
        if (x < 0) *o++ = '-';
        memcpy(o, "inf", 3);
        return (int)(o - out) + 3;
    }
    if (__builtin_signbit(x)) { *o++ = '-'; x = -x; }
    if (x == 0) { memcpy(o, "0.0", 3); return (int)(o - out) + 3; }
    char d[40];
    int decpt = 0;
    int n = tz_digits(x, d, &decpt);
    if (decpt <= -4 || decpt > 16) {
        *o++ = d[0];
        if (n > 1) { *o++ = '.'; memcpy(o, d + 1, (size_t)(n - 1)); o += n - 1; }
        int e = decpt - 1;
        o += sprintf(o, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (decpt <= 0) {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -decpt; i++) *o++ = '0';
        memcpy(o, d, (size_t)n); o += n;
    } else if (decpt < n) {
        memcpy(o, d, (size_t)decpt); o += decpt;
        *o++ = '.';
        memcpy(o, d + decpt, (size_t)(n - decpt)); o += n - decpt;
    } else {
        memcpy(o, d, (size_t)n); o += n;
        for (int i = n; i < decpt; i++) *o++ = '0';
        *o++ = '.'; *o++ = '0';
    }
    return (int)(o - out);
}
TZ_EXPORT tz_i64 tzx_frepr(double x) { return tz_repr_into(x, tz_scratch(64)); }
void tzx_out_float(double x, _Bool error) {
    char text[64];
    int n = tz_repr_into(x, text);
    turi_embed_write(text, n, error);
}
void tzx_out_int(tz_i64 v, _Bool error) {
    char text[24];
    int n = 0;
    unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
    do { text[23 - n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) text[23 - n++] = '-';
    turi_embed_write(text + 24 - n, n, error);
}
/* Formato de Python para números ([[relleno]alineación][signo][#][0][ancho][,|_][.precisión][tipo]).
   kind 0: entero; 1: flotante. Devuelve la longitud; el texto queda en tzx_fbuf(). */
static int tz_group(const char* digits, int n, int every, char sep, char* out) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (i && (n - i) % every == 0) out[k++] = sep;
        out[k++] = digits[i];
    }
    return k;
}

#ifdef __SIZEOF_INT128__
/* Redondea la mantisa multiplicada por 10^p sin un redondeo f64 intermedio.
   El llamador da un f64 positivo o cero, menor que 2^20, y p entre 0 y 9. */
static int tz_fixed_short(double x, int precision, int alternate, char *digits) {
    __extension__ typedef unsigned __int128 UInt128;
    static const uint32_t powers[] = {1, 10, 100, 1000, 10000, 100000,
                                     1000000, 10000000, 100000000, 1000000000};
    uint64_t bits;
    memcpy(&bits, &x, sizeof(bits));
    unsigned exponent = (unsigned)(bits >> 52);
    uint64_t mantissa = bits & UINT64_C(0xfffffffffffff);
    if (exponent) mantissa |= UINT64_C(1) << 52;
    unsigned shift = exponent ? 1075 - exponent : 1074;
    uint32_t scale = powers[precision];
    uint64_t rounded = 0;
    if (shift < 128) {
        UInt128 product = (UInt128)mantissa * scale;
        UInt128 half = (UInt128)1 << (shift - 1);
        UInt128 remainder = product & ((half << 1) - 1);
        rounded = (uint64_t)(product >> shift);
        rounded += remainder > half || (remainder == half && (rounded & 1));
    }
    uint64_t whole = rounded / scale, fraction = rounded % scale;
    unsigned chars = 0;
    do {
        digits[chars++] = (char)('0' + whole % 10);
        whole /= 10;
    } while (whole);
    for (unsigned i = 0; i < chars / 2; ++i) {
        char digit = digits[i];
        digits[i] = digits[chars - i - 1];
        digits[chars - i - 1] = digit;
    }
    if (precision || alternate) digits[chars++] = '.';
    for (int i = precision; i > 0; --i) {
        digits[chars + i - 1] = (char)('0' + fraction % 10);
        fraction /= 10;
    }
    return (int)chars + precision;
}
#endif

TZ_EXPORT tz_i64 tzx_fmt(tz_i64 kind, tz_i64 iv, double fv, tz_i64 fill, tz_i64 align, tz_i64 sign,
                          tz_i64 alt, tz_i64 width, tz_i64 group, tz_i64 prec, tz_i64 type) {
    char body[400], tmp[400], prefix[4] = {0};
    int blen = 0, negative = 0;
    if (kind == 0 && (type == 0 || type == 'd' || type == 'x' || type == 'X' || type == 'o' || type == 'b')) {
        unsigned long long u = iv < 0 ? 0ULL - (unsigned long long)iv : (unsigned long long)iv;
        negative = iv < 0;
        int base = type == 'x' || type == 'X' ? 16 : type == 'o' ? 8 : type == 'b' ? 2 : 10;
        const char* table = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        int n = 0;
        do { tmp[n++] = table[u % (unsigned)base]; u /= (unsigned)base; } while (u);
        for (int i = 0; i < n; i++) body[i] = tmp[n - 1 - i];
        blen = n;
        if (group) {
            memcpy(tmp, body, (size_t)blen);
            blen = tz_group(tmp, blen, base == 10 ? 3 : 4, group == 1 ? ',' : '_', body);
        }
        if (alt && base != 10) { prefix[0] = '0'; prefix[1] = (char)(type == 'X' ? 'X' : type == 'x' ? 'x' : type); }
    } else {
        double x = kind == 0 ? (double)iv : fv;
        negative = __builtin_signbit(x) && x == x;
        double a = negative ? -x : x;
        int t = (int)type;
        if (t == '%') a *= 100;
        if (a != a || a == __builtin_inf()) {
            const char* word = a != a ? "nan" : "inf";
            int upper = t == 'F' || t == 'E' || t == 'G';
            for (int i = 0; i < 3; i++) body[i] = upper ? (char)(word[i] - 32) : word[i];
            blen = 3;
            if (t == '%') body[blen++] = '%';
        } else if (t == 0 && prec < 0) {
            blen = tz_repr_into(a, body);
        } else if (t == 0) {
            /* Sin tipo y con precisión: como 'g', pero pasa a notación científica cuando el exponente
               llega a p - 1, y en notación fija lleva siempre una cifra decimal. */
            int p = prec == 0 ? 1 : (int)prec;
            blen = snprintf(body, sizeof body, alt ? "%#.*e" : "%.*e", p - 1, a);
            if (blen < 0 || blen >= (int)sizeof body) { turi_embed_abort(); return 0; }
            char* e = (char*)memchr(body, 'e', (size_t)blen);
            int exp10 = e ? atoi(e + 1) : 0;
            if (exp10 >= -4 && exp10 < p - 1) {
                blen = snprintf(body, sizeof body, alt ? "%#.*g" : "%.*g", p, a);
                if (!memchr(body, '.', (size_t)blen)) { body[blen++] = '.'; body[blen++] = '0'; }
            } else if (!alt && e) {
                int m = (int)(e - body);
                char tail[16];
                int tl = blen - m;
                memcpy(tail, e, (size_t)tl);
                if (memchr(body, '.', (size_t)m)) {
                    while (m > 0 && body[m - 1] == '0') m--;
                    if (m > 0 && body[m - 1] == '.') m--;
                }
                memcpy(body + m, tail, (size_t)tl);
                blen = m + tl;
            }
        } else {
            int p = prec < 0 ? 6 : (int)prec;
            int c = t == '%' ? 'f' : t;
#ifdef __SIZEOF_INT128__
            if ((c == 'f' || c == 'F') && p >= 0 && p <= 9 && a < 1048576.0) {
                blen = tz_fixed_short(a, p, alt != 0, body);
            } else
#endif
            {
                char spec[16];
                snprintf(spec, sizeof spec, alt ? "%%#.*%c" : "%%.*%c", c);
                blen = snprintf(body, sizeof body, spec, p, a);
                if (blen < 0 || blen >= (int)sizeof body) { turi_embed_abort(); return 0; }
            }
            if (t == '%') body[blen++] = '%';
        }
        if (group) {
            int lead = 0;
            while (lead < blen && body[lead] >= '0' && body[lead] <= '9') lead++;
            memcpy(tmp, body, (size_t)blen);
            int k = tz_group(tmp, lead, 3, group == 1 ? ',' : '_', body);
            memcpy(body + k, tmp + lead, (size_t)(blen - lead));
            blen = k + (blen - lead);
        }
    }
    char signch = negative ? '-' : sign == '+' ? '+' : sign == ' ' ? ' ' : 0;
    int plen = (int)strlen(prefix);
    tz_i64 content = (signch ? 1 : 0) + plen + blen;
    tz_i64 pad = width > content ? width - content : 0;
    char* out = tz_scratch(content + pad + 8);
    char* o = out;
    char f = (char)fill;
    if (align == '>') for (tz_i64 i = 0; i < pad; i++) *o++ = f;
    if (align == '^') for (tz_i64 i = 0; i < pad / 2; i++) *o++ = f;
    if (signch) *o++ = signch;
    memcpy(o, prefix, (size_t)plen); o += plen;
    if (align == '=') for (tz_i64 i = 0; i < pad; i++) *o++ = f;
    memcpy(o, body, (size_t)blen); o += blen;
    if (align == '<') for (tz_i64 i = 0; i < pad; i++) *o++ = f;
    if (align == '^') for (tz_i64 i = 0; i < pad - pad / 2; i++) *o++ = f;
    return (tz_i64)(o - out);
}
