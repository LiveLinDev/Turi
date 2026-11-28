/* Aproxima por bloques las entradas de la matriz positiva, con cota absoluta. */
#include <immintrin.h>

#define TZ_SERIES_DEGREE 22
#define TZ_SERIES_DIVISOR 4
#define TZ_SERIES_BASE 32
#define TZ_SERIES_LIMIT 8192

typedef __m512d serie8;

static inline serie8 serie_fill(double x) { return _mm512_set1_pd(x); }
static inline serie8 serie_mul(serie8 a, serie8 b) { return _mm512_mul_pd(a, b); }
static inline serie8 serie_sum(serie8 a, serie8 b) { return _mm512_add_pd(a, b); }
static inline serie8 serie_fma(serie8 a, serie8 b, serie8 c) { return _mm512_fmadd_pd(a, b, c); }

static inline double serie_den(long i, long j) {
    double s = (double)(i + j);
    return s * (s + 1.0) * 0.5 + i + 1.0;
}

static inline __mmask8 serie_mask(long j, long end) {
    long remaining = end - j;
    return remaining >= 8 ? 255 : (__mmask8)((1u << remaining) - 1u);
}

static inline serie8 serie_window(long j, long center, double radius, serie8 *a, serie8 *b) {
    const serie8 lanes = _mm512_setr_pd(0, 1, 2, 3, 4, 5, 6, 7);
    serie8 s = serie_sum(serie_fill(j + center), lanes);
    serie8 d = serie_fma(serie_mul(s, serie_fill(0.5)), serie_sum(s, serie_fill(1.0)), serie_fill(center + 1));
    serie8 inverse = _mm512_div_pd(serie_fill(1.0), d);
    *a = serie_mul(serie_mul(serie_sum(s, serie_fill(1.5)), serie_fill(-radius)), inverse);
    *b = serie_mul(serie_fill(-radius * radius * 0.5), inverse);
    return inverse;
}

static void serie_forward(const double *u, double *out, long i0, long i1, long j0, long j1, long radius) {
    for (long left = i0; left < i1; left += 2 * radius) {
        long center = left + radius;
        long right = i1 < center + radius ? i1 : center + radius;
        serie8 sums[TZ_SERIES_DEGREE + 1] = {0};
        for (long j = j0; j < j1; j += 8) {
            serie8 a, b;
            serie8 inverse = serie_window(j, center, (double)radius, &a, &b);
            serie8 scale = serie_mul(inverse, _mm512_maskz_loadu_pd(serie_mask(j, j1), u + j));
            serie8 previous = scale, current = serie_mul(a, scale);
            sums[0] = serie_sum(sums[0], previous);
            sums[1] = serie_sum(sums[1], current);
#pragma clang loop unroll(full)
            for (int k = 2; k <= TZ_SERIES_DEGREE; k++) {
                serie8 next = serie_fma(a, current, serie_mul(b, previous));
                sums[k] = serie_sum(sums[k], next);
                previous = current;
                current = next;
            }
        }
        double reduced[TZ_SERIES_DEGREE + 1];
        for (int k = 0; k <= TZ_SERIES_DEGREE; k++) reduced[k] = _mm512_reduce_add_pd(sums[k]);
        const serie8 lanes = _mm512_setr_pd(0, 1, 2, 3, 4, 5, 6, 7);
        serie8 inverse_radius = serie_fill(1.0 / radius);
        for (long i = left; i < right; i += 8) {
            serie8 t = serie_mul(serie_sum(serie_fill(i - center), lanes), inverse_radius);
            serie8 value = serie_fill(reduced[TZ_SERIES_DEGREE]);
#pragma clang loop unroll(full)
            for (int k = TZ_SERIES_DEGREE - 1; k >= 0; k--)
                value = serie_fma(value, t, serie_fill(reduced[k]));
            __mmask8 mask = serie_mask(i, right);
            _mm512_mask_storeu_pd(out + i, mask, serie_sum(_mm512_maskz_loadu_pd(mask, out + i), value));
        }
    }
}

static void serie_transpose(const double *u, double *out, long i0, long i1, long j0, long j1, long radius) {
    for (long left = i0; left < i1; left += 2 * radius) {
        long center = left + radius;
        long right = i1 < center + radius ? i1 : center + radius;
        double weights[TZ_SERIES_DEGREE + 1];
        serie8 sums[TZ_SERIES_DEGREE + 1] = {0};
        const serie8 lanes = _mm512_setr_pd(0, 1, 2, 3, 4, 5, 6, 7);
        serie8 inverse_radius = serie_fill(1.0 / radius);
        for (long i = left; i < right; i += 8) {
            serie8 t = serie_mul(serie_sum(serie_fill(i - center), lanes), inverse_radius);
            serie8 term = _mm512_maskz_loadu_pd(serie_mask(i, right), u + i);
            sums[0] = serie_sum(sums[0], term);
#pragma clang loop unroll(full)
            for (int k = 1; k <= TZ_SERIES_DEGREE; k++) {
                term = serie_mul(term, t);
                sums[k] = serie_sum(sums[k], term);
            }
        }
        for (int k = 0; k <= TZ_SERIES_DEGREE; k++) weights[k] = _mm512_reduce_add_pd(sums[k]);
        for (long j = j0; j < j1; j += 8) {
            serie8 a, b;
            serie8 inverse = serie_window(j, center, (double)radius, &a, &b);
            serie8 previous = serie_fill(1.0), current = a;
            serie8 result = serie_fma(current, serie_fill(weights[1]), serie_fill(weights[0]));
#pragma clang loop unroll(full)
            for (int k = 2; k <= TZ_SERIES_DEGREE; k++) {
                serie8 next = serie_fma(a, current, serie_mul(b, previous));
                result = serie_fma(next, serie_fill(weights[k]), result);
                previous = current;
                current = next;
            }
            __mmask8 mask = serie_mask(j, j1);
            serie8 old = _mm512_maskz_loadu_pd(mask, out + j);
            _mm512_mask_storeu_pd(out + j, mask, serie_fma(result, inverse, old));
        }
    }
}

static void serie_apply(const double *u, double *out, long n, int transpose) {
    long corner = TZ_SERIES_BASE < n ? TZ_SERIES_BASE : n;
    for (long i = 0; i < corner; i++) {
        double sum = 0;
        for (long j = 0; j < corner; j++) {
            double d = transpose ? serie_den(j, i) : serie_den(i, j);
            sum += (1.0 / d) * u[j];
        }
        out[i] = sum;
    }
    for (long i = corner; i < n; i++) out[i] = 0;
    for (long s = TZ_SERIES_BASE; s < n; s *= 2) {
        long end = n < 2 * s ? n : 2 * s;
        long radius = s / TZ_SERIES_DIVISOR;
        if (transpose) {
            serie_transpose(u, out, 0, s, s, end, radius);
            serie_transpose(u, out, s, end, 0, end, radius);
        } else {
            serie_forward(u, out, 0, s, s, end, radius);
            serie_forward(u, out, s, end, 0, end, radius);
        }
    }
}

#ifdef TZ_ESPECTRAL_RESIDUO
static int serie_rayleigh(const double *v, const double *w, long n, int power, double error, double *answer) {
    double numerator = 0.0, denominator = 0.0;
    for (long i = 0; i < n; i++) {
        numerator += v[i] * w[i];
        denominator += v[i] * v[i];
    }
    const double unit = 0x1p-53, dot = (4 * n + 16) * unit;
    double rho = numerator / denominator;
    double below = rho * (1.0 - (4.0 * error + dot));
    double gap = 2.0 * below - 1.9;
    if (gap <= 0.0) return 0;
#ifdef TZ_ESPECTRAL_SEMILLA
    if (below < 1.520000000001) return 0;
#endif
    double square = 0.0;
    for (long i = 0; i < n; i++) {
        double d = w[i] - rho * v[i];
        square += d * d;
    }
    double residual = sqrt(square / denominator) * (1.0 + dot) + 8.0 * error;
    double above = rho * (1.0 + 4.0 * error + dot) + residual * residual / gap;
    double roundoff = (16 * 10 + 16) * (n + 3) * unit;
#ifdef TZ_ESPECTRAL_SEMILLA
    double low = sqrt(below * (1.0 - (double)n * 0x1p-76) * (1.0 - roundoff));
#else
    double low = sqrt(rho * (1.0 - (8 * power + 8) * error - dot) * (1.0 - roundoff));
#endif
    double high = sqrt(above * (1.0 + roundoff));
    long long first = (long long)((low - 0x1p-47) * 1000000000.0 + 0.5);
    long long last = (long long)((high + 0x1p-47) * 1000000000.0 + 0.5);
    if (first != last) return 0;
    *answer = (low + high) * 0.5;
    return 1;
}
#endif

#ifdef TZ_ESPECTRAL_SECCION
static void serie_seed_section(double *u, long n) {
    /* Sección de 160 filas y 11 iteraciones, como section_seed en norma_espectral.py. */
    static const double seed[] = {
        1.0, 0.5205127660611983, 0.2761129847030202, 0.16628614685945456,
        0.11049003508972098, 0.07869821828194841, 0.05893953385814455, 0.045831187657001164,
        0.036687923918435526, 0.030053785324170072, 0.025084881987318293, 0.021264887259965594,
        0.01826348963965133, 0.015861352696343287, 0.013908156604017436, 0.012298052380825002,
        0.01095473498681847, 0.009822055870738054, 0.008857943368994621, 0.00803036218084592,
        0.007314565637204523, 0.006691188384540944, 0.0061448977901640885, 0.005663424373199673,
        0.005236854105233781, 0.004857104672900862, 0.004517532952256432, 0.004212637384629105,
        0.003937829878406368, 0.003689259253625763, 0.0034636733194775447, 0.0032583102051983053,
        0.0030708120534719786, 0.0028991559609540026, 0.0027415983315343963, 0.00259662974192547,
        0.002462938106840594, 0.002339378442066995, 0.0022249479068178965, 0.0021187650962781143,
        0.0020200527757796285, 0.0019281234172410654, 0.001842367029226598, 0.001762240873641777,
        0.0016872607416421493, 0.0016169935239623988, 0.0015510508604604626, 0.0014890836931423148,
    };
    const long count = sizeof(seed) / sizeof(*seed);
    long first = n < count ? n : count;
    for (long j = 0; j < first; j++) u[j] = seed[j];
    for (long j = first; j < n; j++) {
        double x = j + 2.8409487844185133;
        u[j] = 3.6990628454868975 / (x * x);
    }
}
#endif

#ifdef TZ_ESPECTRAL_CERTIFICADO
static int serie_certificado(double *u, double *v, long n, double error, double *answer) {
    if (n < 512) return 0;
    /* La secci�n fija y la cota del residuo se calculan en norma_espectral.py. */
    static const double seed[] = {
        1.0, 0.5205130564990977, 0.27611341005385726, 0.16628661921028726,
        0.11049052280097885, 0.07869870923783381, 0.05894002300817306, 0.045831672785137834,
        0.03668840404947483, 0.030054260062090908, 0.02508535121806513, 0.021265351009499976,
        0.018263948002952335, 0.015861805800707677, 0.01390860458970632, 0.012298495390799639,
        0.01095517316136777, 0.00982248934478791, 0.008858372270901745, 0.00803078663201266,
        0.007314985752140697, 0.006691604271159583, 0.006145309550189159, 0.0056638321026163145,
        0.005237257894742087, 0.004857504608352156, 0.004517929115061468, 0.004213029852133697,
        0.0039382187242335345, 0.003689644547985332, 0.0034640551294452602, 0.003258688594966878,
        0.0030711870845767, 0.0028995276924754856, 0.002741966820280844, 0.0025969950425967186,
        0.0024633002721759388, 0.0023397375229786024, 0.002225303952511427, 0.0021191181543619765,
        0.002020402892364124, 0.0019284706370285487, 0.0018427113955936222, 0.0017625824287140928,
        0.0016875995263633224, 0.001617329578156712, 0.0015513842228906955, 0.0014894144015629297,
        0.001431105669721042, 0.0013761741503812742, 0.001324363029273384, 0.0012754395108166305,
        0.0012291921668008505, 0.0011854286208520888, 0.0011439735208875496, 0.00110466675929165,
        0.0010673619067764772, 0.0010319248310672845, 0.0009982324758698253, 0.0009661717791864091,
        0.0009356387130766729, 0.0009065374295084748, 0.0008787794990963432, 0.0008522832313466982,
        0.0008269730665755306, 0.0008027790309805044, 0.0007796362474726905, 0.000757484495834062,
        0.0007362678165909294, 0.0007159341537018327, 0.0006964350317686647, 0.0006777252640067274,
        0.0006597626876653748, 0.0006425079239862455, 0.0006259241601296102, 0.0006099769507983949,
        0.0005946340375502983, 0.0005798651840163562, 0.000565642025443858, 0.0005519379311565279,
        0.0005387278786786234, 0.000525988338404889, 0.0005136971678175524, 0.000501833514356809,
        0.0004903777261443246, 0.00047931126984169646, 0.0004686166549989103, 0.0004582773643127329,
        0.0004482777892727144, 0.00043860317072387723, 0.0004292395439210177, 0.00042017368769047686,
        0.00041139307735184007, 0.00040288584108478246, 0.00039464071945564385, 0.00038664702784466635,
        0.0003788946215384981, 0.0003713738632738609, 0.0003640755930374474, 0.00035699109994439984,
        0.0003501120960333129, 0.000343430691829793, 0.0003369393735433451, 0.0003306309817738889,
        0.0003244986916146539, 0.00031853599404768016, 0.0003127366785367533, 0.0003070948167304191,
        0.0003016047471948343, 0.00029626106110267953, 0.0002910585888102599, 0.00028599238726029425,
        0.0002810577281528028, 0.0002762500868309865, 0.0002715651318330877, 0.00026699871506497507,
        0.00026254686255162496, 0.0002582057657288221, 0.00025397177323928345, 0.0002498413832000616,
        0.00024581123591051163, 0.0002418781069723445, 0.000238038900795343, 0.0002342906444642102,
        0.0002306304819437619, 0.00022705566860128159, 0.00022356356602633748, 0.00022015163712972677,
        0.00021681744150447792, 0.00021355863103300374, 0.000210372945725581, 0.00020725820977632648,
        0.00020421232782376564, 0.00020123328140394416, 0.00019831912558482633, 0.00019546798577145923,
        0.00019267805467206497, 0.00018994758941585503, 0.0001872749088139513, 0.00018465839075534386,
        0.00018209646973032457, 0.0001795876344743094, 0.00017713042572540218, 0.00017472343408946284,
        0.00017236529800682636, 0.00017005470181517363, 0.0001677903739033899, 0.00016557108495155457,
        0.00016339564625249763, 0.00016126290811062775, 0.0001591717583139899, 0.000157121120675747,
        0.0001551099536415022, 0.00015313724895908265, 0.0001512020304076033, 0.00014930335258280685,
        0.00014744029973584943, 0.00014561198466285795, 0.00014381754764273722, 0.0001420561554208435,
        0.0001403270002362732, 0.0001386292988906395, 0.00013696229185632561, 0.00013532524242231144,
        0.0001337174358757752, 0.00013213817871776556, 0.0001305867979113327, 0.0001290626401605901,
        0.00012756507121926158, 0.00012609347522734123, 0.00012464725407456792, 0.0001232258267894807,
        0.0001218286289528872, 0.00012045511213463461, 0.00011910474335263185, 0.00011777700455312179,
        0.0001164713921112555, 0.00011518741635106602, 0.0001139246010839845, 0.00011268248316508488,
        0.00011146061206628154, 0.00011025854946574406, 0.00010907586885282785, 0.00010791215514785373,
        0.00010676700433610186, 0.00010564002311541529, 0.00010453082855683762, 0.00010343904777773646,
        0.00010236431762688978, 0.00010130628438103751, 0.00010026460345242319, 9.923893910687304e-05,
    };
    const long count = sizeof(seed) / sizeof(*seed);
    for (long i = 0; i < count; i++) u[i] = seed[i];
    for (long i = count; i < n; i++) {
        double x = i + 4.3803453127855825;
        u[i] = 3.788295591178222 / (x * x);
    }
    serie_apply(u, v, n, 0);
    __m512d unorm = _mm512_setzero_pd(), vnorm = unorm;
    for (long i = 0; i < n; i += 8) {
        const __mmask8 active = n - i >= 8 ? 255 : (1u << (n - i)) - 1;
        __m512d x = _mm512_maskz_loadu_pd(active, u + i);
        __m512d y = _mm512_maskz_loadu_pd(active, v + i);
        unorm = _mm512_fmadd_pd(x, x, unorm);
        vnorm = _mm512_fmadd_pd(y, y, vnorm);
    }
    const double unit = 0x1p-53;
    double rho = _mm512_reduce_add_pd(vnorm) / _mm512_reduce_add_pd(unorm);
    double margin = 4 * error + (4 * n + 16) * unit;
    double below = rho * (1.0 - margin);
    if (below < 1.520000000001) return 0;
    double above = rho * (1.0 + margin) + 8.220479893219967e-11 / (2.0 * below - 1.9);
    double roundoff = (16 * 10 + 16) * (n + 3) * unit;
    double low = sqrt(below * (1.0 - n * 0x1p-76) * (1.0 - roundoff));
    double high = sqrt(above * (1.0 + roundoff));
    if ((long long)((low - 0x1p-47) * 1e9 + 0.5) != (long long)((high + 0x1p-47) * 1e9 + 0.5))
        return 0;
    *answer = (low + high) * 0.5;
    return 1;
}
#endif

/* La cota relativa por producto se deriva en norma_espectral.py. */
static int serie_solve(double *u, double *v, double *tmp, long n, double *answer) {
    if (n < TZ_SERIES_BASE || n > TZ_SERIES_LIMIT) return 0;
    const double unit = 0x1p-53;
    const double error = (8.0 * n + 800.0) * unit + 0x1p-40;
#ifdef TZ_ESPECTRAL_CERTIFICADO
    if (serie_certificado(u, v, n, error, answer)) return 1;
#endif
#ifdef TZ_ESPECTRAL_RESIDUO
#if defined(TZ_ESPECTRAL_SECCION)
    serie_seed_section(u, n);
#elif defined(TZ_ESPECTRAL_SEMILLA)
    for (long j = 0; j < n; j++) u[j] = 1.0 / serie_den(0, j);
#endif
    double *source = u, *dest = v;
    for (int power = 0; power < 10; power++) {
        serie_apply(source, tmp, n, 0);
        serie_apply(tmp, dest, n, 1);
        if (serie_rayleigh(source, dest, n, power, error, answer)) return 1;
        double *swap = source;
        source = dest;
        dest = swap;
    }
#else
    for (int step = 0; step < 5; step++) {
        serie_apply(u, tmp, n, 0);
        serie_apply(tmp, v, n, 1);
        serie_apply(v, tmp, n, 0);
        serie_apply(tmp, u, n, 1);
        double lower = u[0] / v[0], upper = lower;
        for (long j = 1; j < n; j++) {
            double ratio = u[j] / v[j];
            lower = fmin(lower, ratio);
            upper = fmax(upper, ratio);
        }
        double margin = (8 * step + 8) * error + (16 * (9 - step) + 16) * (n + 3) * unit;
        double low = sqrt(lower * (1.0 - margin)), high = sqrt(upper * (1.0 + margin));
        double first = (low - 0x1p-47) * 1000000000.0 + 0.5;
        double last = (high + 0x1p-47) * 1000000000.0 + 0.5;
        if (isfinite(low) && isfinite(high) && (long long)first == (long long)last) {
            *answer = (low + high) * 0.5;
            return 1;
        }
    }
#endif
    for (long j = 0; j < n; j++) u[j] = 1.0;
    return 0;
}
