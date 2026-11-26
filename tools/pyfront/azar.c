/* `random` de CPython en nativo (motor híbrido, tools/pyfront/turipy.py).

   El mismo Mersenne Twister que `_randommodule.c` y las mismas fórmulas que `random.py`, así que una
   simulación da exactamente los mismos números. El estado de cada objeto `random.Random` que usa la
   llamada se copia aquí al entrar (getstate) y se devuelve al terminar bien (setstate); si la llamada
   se detiene no se devuelve nada y el objeto de Python queda como estaba.

   Se incluye en el shim después de frontera.c. */

#pragma STDC FP_CONTRACT OFF

#define TZ_MT_N 624
#define TZ_MT_M 397
#define TZ_RNG_MAX 16

typedef struct {
    unsigned int mt[TZ_MT_N];
    int index;
    int seeded;          /* se llamó a seed(): gauss_next vuelve a None */
    PyObject* owner;     /* el objeto de Python (prestado; vive mientras dura la llamada) */
    PyObject* gauss;     /* gauss_next tal como estaba (referencia propia) */
} tz_rng;

typedef struct { tz_rng r[TZ_RNG_MAX]; int n; } tz_rngs;
static tz_rngs* tz_rg;   /* en memoria reservada una vez: restaurar la biblioteca no la toca */
static PyObject* tzh_random_type;

static unsigned int tz_genrand(tz_rng* self) {
    static const unsigned int mag01[2] = {0x0U, 0x9908b0dfU};
    unsigned int y;
    unsigned int* mt = self->mt;
    if (self->index >= TZ_MT_N) {
        int kk;
        for (kk = 0; kk < TZ_MT_N - TZ_MT_M; kk++) {
            y = (mt[kk] & 0x80000000U) | (mt[kk + 1] & 0x7fffffffU);
            mt[kk] = mt[kk + TZ_MT_M] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        for (; kk < TZ_MT_N - 1; kk++) {
            y = (mt[kk] & 0x80000000U) | (mt[kk + 1] & 0x7fffffffU);
            mt[kk] = mt[kk + (TZ_MT_M - TZ_MT_N)] ^ (y >> 1) ^ mag01[y & 0x1U];
        }
        y = (mt[TZ_MT_N - 1] & 0x80000000U) | (mt[0] & 0x7fffffffU);
        mt[TZ_MT_N - 1] = mt[TZ_MT_M - 1] ^ (y >> 1) ^ mag01[y & 0x1U];
        self->index = 0;
    }
    y = mt[self->index++];
    y ^= (y >> 11);
    y ^= (y << 7) & 0x9d2c5680U;
    y ^= (y << 15) & 0xefc60000U;
    y ^= (y >> 18);
    return y;
}

static void tz_init_genrand(tz_rng* self, unsigned int s) {
    unsigned int* mt = self->mt;
    mt[0] = s;
    for (int i = 1; i < TZ_MT_N; i++) mt[i] = (1812433253U * (mt[i - 1] ^ (mt[i - 1] >> 30)) + (unsigned int)i);
    self->index = TZ_MT_N;
}

static void tz_init_by_array(tz_rng* self, const unsigned int* key, int length) {
    unsigned int* mt = self->mt;
    tz_init_genrand(self, 19650218U);
    int i = 1, j = 0;
    for (int k = TZ_MT_N > length ? TZ_MT_N : length; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525U)) + key[j] + (unsigned int)j;
        i++;
        j++;
        if (i >= TZ_MT_N) { mt[0] = mt[TZ_MT_N - 1]; i = 1; }
        if (j >= length) j = 0;
    }
    for (int k = TZ_MT_N - 1; k; k--) {
        mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941U)) - (unsigned int)i;
        i++;
        if (i >= TZ_MT_N) { mt[0] = mt[TZ_MT_N - 1]; i = 1; }
    }
    mt[0] = 0x80000000U;
}

static tz_rng* tz_rng_at(tz_i64 h) {
    if (h < 0 || !tz_rg || h >= tz_rg->n) { turi_embed_abort(); return 0; }
    return &tz_rg->r[h];
}

double tzx_rnd_random(tz_i64 h) {
    tz_rng* r = tz_rng_at(h);
    unsigned int a = tz_genrand(r) >> 5, b = tz_genrand(r) >> 6;
    return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
}

/* getrandbits(k) para 1 <= k <= 63: las palabras de 32 bits van de la menos significativa a la más. */
tz_i64 tzx_rnd_bits(tz_i64 h, tz_i64 k) {
    tz_rng* r = tz_rng_at(h);
    if (k <= 0 || k > 63) { turi_embed_abort(); return 0; }
    if (k <= 32) return (tz_i64)(tz_genrand(r) >> (32 - k));
    unsigned long long lo = tz_genrand(r);
    unsigned long long hi = tz_genrand(r) >> (64 - k);
    return (tz_i64)(lo | (hi << 32));
}

/* _randbelow(n), n >= 1 */
tz_i64 tzx_rnd_below(tz_i64 h, tz_i64 n) {
    if (n <= 0) { turi_embed_abort(); return 0; }
    int k = 64 - __builtin_clzll((unsigned long long)n);
    tz_i64 v = tzx_rnd_bits(h, k);
    while (v >= n) v = tzx_rnd_bits(h, k);
    return v;
}

double tzx_rnd_uniform(tz_i64 h, double a, double b) {
    double r = tzx_rnd_random(h);
    double width = b - a;
    double scaled = width * r;
    return a + scaled;
}

/* random.seed(a) con un entero: todos los bits de |a|, en palabras de 32 bits. */
void tzx_rnd_seed(tz_i64 h, tz_i64 a) {
    tz_rng* r = tz_rng_at(h);
    unsigned long long n = a < 0 ? 0ULL - (unsigned long long)a : (unsigned long long)a;
    unsigned int key[2] = {(unsigned int)n, (unsigned int)(n >> 32)};
    tz_init_by_array(r, key, (n >> 32) ? 2 : 1);
    r->seeded = 1;
}

/* ---- frontera */
static int tzc_random(PyObject* o, tz_i64* slot) {
    if (!tzh_random_type || (PyObject*)Py_TYPE(o) != tzh_random_type) return 0;
    if (!tz_rg) {
        tz_rg = (tz_rngs*)calloc(1, sizeof(tz_rngs));
        if (!tz_rg) return 0;
    }
    for (int i = 0; i < tz_rg->n; i++)
        if (tz_rg->r[i].owner == o) { *slot = i; return 1; }  /* el mismo objeto dos veces */
    if (tz_rg->n >= TZ_RNG_MAX) return 0;
    PyObject* state = PyObject_CallMethod(o, "getstate", 0);
    if (!state) { PyErr_Clear(); return 0; }
    int ok = 0;
    if (PyTuple_Check(state) && PyTuple_GET_SIZE(state) == 3) {
        PyObject* words = PyTuple_GET_ITEM(state, 1);
        if (PyTuple_Check(words) && PyTuple_GET_SIZE(words) == TZ_MT_N + 1) {
            tz_rng* r = &tz_rg->r[tz_rg->n];
            ok = 1;
            for (int i = 0; i <= TZ_MT_N && ok; i++) {
                unsigned long v = PyLong_AsUnsignedLong(PyTuple_GET_ITEM(words, i));
                if (PyErr_Occurred()) { PyErr_Clear(); ok = 0; break; }
                if (i < TZ_MT_N) r->mt[i] = (unsigned int)v; else r->index = (int)v;
            }
            if (ok) {
                r->owner = o;
                r->seeded = 0;
                r->gauss = PyTuple_GET_ITEM(state, 2);
                Py_INCREF(r->gauss);
                *slot = tz_rg->n++;
            }
        }
    }
    Py_DECREF(state);
    return ok;
}

/* Tras una llamada que terminó bien: cada objeto recibe su estado nuevo. */
static int tzw_randoms(void) {
    int status = 0;
    if (!tz_rg) return 0;
    for (int i = 0; i < tz_rg->n; i++) {
        tz_rng* r = &tz_rg->r[i];
        if (status == 0) {
            PyObject* words = PyTuple_New(TZ_MT_N + 1);
            for (int k = 0; words && k <= TZ_MT_N; k++) {
                PyObject* v = PyLong_FromUnsignedLong(k < TZ_MT_N ? r->mt[k] : (unsigned long)r->index);
                if (!v) { Py_CLEAR(words); break; }
                PyTuple_SET_ITEM(words, k, v);
            }
            PyObject* gauss = r->seeded ? Py_None : r->gauss;
            PyObject* state = words ? Py_BuildValue("(iNO)", 3, words, gauss) : 0;
            PyObject* done = state ? PyObject_CallMethod(r->owner, "setstate", "(O)", state) : 0;
            if (!done) status = -1;
            Py_XDECREF(done);
            Py_XDECREF(state);
        }
        Py_CLEAR(r->gauss);
        r->owner = 0;
    }
    tz_rg->n = 0;
    return status;
}

/* Tras una parada, o si la llamada no llegó a nativo: se olvida todo. */
static void tz_rng_forget(void) {
    if (!tz_rg) return;
    for (int i = 0; i < tz_rg->n; i++) {
        Py_CLEAR(tz_rg->r[i].gauss);
        tz_rg->r[i].owner = 0;
    }
    tz_rg->n = 0;
}

static PyObject* tzh_random_inst;
/* random_type(random.Random, random._inst): se llama al preparar la variante, antes de guardar el
   estado de la biblioteca. */
static PyObject* tzm_random_type(PyObject* self, PyObject* args) {
    (void)self;
    PyObject *type, *inst;
    if (!PyArg_ParseTuple(args, "OO", &type, &inst)) return 0;
    Py_INCREF(type);
    Py_INCREF(inst);
    tzh_random_type = type;
    tzh_random_inst = inst;
    if (!tz_rg) tz_rg = (tz_rngs*)calloc(1, sizeof(tz_rngs));
    Py_RETURN_NONE;
}

/* El objeto oculto del módulo random va siempre el primero (número 0). */
static int tzc_global_random(void) {
    tz_i64 h = -1;
    return tzh_random_inst && tzc_random(tzh_random_inst, &h) && h == 0;
}
