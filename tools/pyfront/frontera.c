/* Frontera entre CPython y el código nativo de una variante (motor híbrido, tools/pyfront/turipy.py).

   Se incluye en el shim de cada biblioteca. Las funciones tzc_* leen objetos de Python y escriben el
   bloque de datos que lee la función Turi `tze_entry` (mismo formato que antes escribía Python con
   ctypes); las tzd_* construyen objetos de Python con lo que la función Turi dejó en el bloque y
   liberan su memoria; las tzw_* devuelven a las listas de quien llama los cambios hechos en sitio.

   Devuelven 0 cuando el valor no es del tipo exacto esperado: entonces la llamada no va a nativo. */

static void tz_objs_reset(void);  /* objetos.c, que se incluye después */

#define TZ_OWN_MAX 96
typedef struct { void* p[TZ_OWN_MAX]; int n; } tz_own;

static void tz_own_free(tz_own* o) {
    for (int i = 0; i < o->n; i++) free(o->p[i]);
    o->n = 0;
}

static void* tz_own_alloc(tz_own* o, size_t bytes) {
    if (o->n >= TZ_OWN_MAX) return 0;
    void* p = malloc(bytes ? bytes : 8);
    if (p) o->p[o->n++] = p;
    return p;
}

static int tzc_int(PyObject* o, tz_i64* out) {
    if (!PyLong_CheckExact(o)) return 0;
    int overflow = 0;
    long long v = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow || (v == -1 && PyErr_Occurred())) {
        PyErr_Clear();
        return 0;
    }
    *out = v;
    return 1;
}

static int tzc_float(PyObject* o, double* out) {
    if (!PyFloat_CheckExact(o)) return 0;
    *out = PyFloat_AS_DOUBLE(o);
    return 1;
}

static int tzc_bool(PyObject* o, tz_i64* out) {
    if (o == Py_True) { *out = 1; return 1; }
    if (o == Py_False) { *out = 0; return 1; }
    return 0;
}

static int tzc_scalar(PyObject* o, int kind, tz_i64* out) {
    if (kind == 'F') return tzc_float(o, (double*)out);
    if (kind == 'B') return tzc_bool(o, out);
    return tzc_int(o, out);
}

/* Textos que cruzan: ASCII imprimible, tabulador y saltos de línea. */
static int tzc_plain(const unsigned char* p, Py_ssize_t n) {
    for (Py_ssize_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (!(c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127))) return 0;
    }
    return 1;
}

static int tzc_text(PyObject* o, const char** p, Py_ssize_t* n) {
    if (!PyUnicode_CheckExact(o) || !PyUnicode_IS_ASCII(o)) return 0;
    *p = (const char*)PyUnicode_DATA(o);
    *n = PyUnicode_GET_LENGTH(o);
    return tzc_plain((const unsigned char*)*p, *n);
}

static int tzc_str(PyObject* o, tz_i64* slot) {
    const char* p;
    Py_ssize_t n;
    if (!tzc_text(o, &p, &n)) return 0;
    slot[0] = (tz_i64)p;
    slot[1] = (tz_i64)n;
    return 1;
}

static int tzc_list(PyObject* o, int kind, tz_i64* slot, tz_own* own, Py_ssize_t fixed) {
    if (!PyList_CheckExact(o)) return 0;
    Py_ssize_t n = PyList_GET_SIZE(o);
    if (fixed >= 0 && n != fixed) return 0;
    tz_i64* buf = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
    if (!buf) return 0;
    for (Py_ssize_t i = 0; i < n; i++)
        if (!tzc_scalar(PyList_GET_ITEM(o, i), kind, &buf[i])) return 0;
    slot[0] = (tz_i64)buf;
    slot[1] = (tz_i64)n;
    return 1;
}

static int tzc_strs(PyObject* o, tz_i64* slot, tz_own* own) {
    if (!PyList_CheckExact(o)) return 0;
    Py_ssize_t n = PyList_GET_SIZE(o), total = 0;
    tz_i64* offs = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
    if (!offs) return 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        const char* p;
        Py_ssize_t m;
        if (!tzc_text(PyList_GET_ITEM(o, i), &p, &m)) return 0;
        offs[i] = total;
        total += m;
    }
    offs[n] = total;
    char* buf = (char*)tz_own_alloc(own, (size_t)total + 1);
    if (!buf) return 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* s = PyList_GET_ITEM(o, i);
        memcpy(buf + offs[i], PyUnicode_DATA(s), (size_t)(offs[i + 1] - offs[i]));
    }
    slot[0] = (tz_i64)buf;
    slot[1] = (tz_i64)offs;
    slot[2] = (tz_i64)n;
    return 1;
}

static int tz_ptr_cmp(const void* a, const void* b) {
    PyObject* x = *(PyObject* const*)a;
    PyObject* y = *(PyObject* const*)b;
    return x < y ? -1 : x > y;
}

/* Lista de listas de escalares. `distinct`: las filas se modifican y tienen que ser objetos
   distintos (en Python, dos filas que son la misma lista cambiarían a la vez). */
static int tzc_rows(PyObject* o, int kind, tz_i64* slot, tz_own* own, int distinct) {
    if (!PyList_CheckExact(o)) return 0;
    Py_ssize_t n = PyList_GET_SIZE(o), total = 0;
    tz_i64* lens = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
    if (!lens) return 0;
    for (Py_ssize_t r = 0; r < n; r++) {
        PyObject* row = PyList_GET_ITEM(o, r);
        if (!PyList_CheckExact(row)) return 0;
        lens[r] = PyList_GET_SIZE(row);
        total += lens[r];
    }
    if (distinct && n > 1) {
        PyObject** ptrs = (PyObject**)tz_own_alloc(own, (size_t)n * sizeof(PyObject*));
        if (!ptrs) return 0;
        for (Py_ssize_t r = 0; r < n; r++) ptrs[r] = PyList_GET_ITEM(o, r);
        qsort(ptrs, (size_t)n, sizeof(PyObject*), tz_ptr_cmp);
        for (Py_ssize_t r = 1; r < n; r++)
            if (ptrs[r] == ptrs[r - 1]) return 0;
    }
    tz_i64* data = (tz_i64*)tz_own_alloc(own, (size_t)(total + 1) * 8);
    if (!data) return 0;
    Py_ssize_t at = 0;
    for (Py_ssize_t r = 0; r < n; r++) {
        PyObject* row = PyList_GET_ITEM(o, r);
        for (Py_ssize_t c = 0; c < lens[r]; c++)
            if (!tzc_scalar(PyList_GET_ITEM(row, c), kind, &data[at++])) return 0;
    }
    slot[0] = (tz_i64)data;
    slot[1] = (tz_i64)lens;
    slot[2] = (tz_i64)n;
    return 1;
}

/* Lista de tuplas de escalares: una columna por campo. */
static int tzc_tuples(PyObject* o, const char* sig, tz_i64* slot, tz_own* own) {
    if (!PyList_CheckExact(o)) return 0;
    Py_ssize_t n = PyList_GET_SIZE(o);
    int width = (int)strlen(sig);
    for (int f = 0; f < width; f++) {
        tz_i64* col = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
        if (!col) return 0;
        slot[f] = (tz_i64)col;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* t = PyList_GET_ITEM(o, i);
        if (!PyTuple_CheckExact(t) || PyTuple_GET_SIZE(t) != width) return 0;
        for (int f = 0; f < width; f++)
            if (!tzc_scalar(PyTuple_GET_ITEM(t, f), sig[f], &((tz_i64*)slot[f])[i])) return 0;
    }
    slot[width] = (tz_i64)n;
    return 1;
}

/* ---- de nativo a Python (cada una libera la memoria que reservó el lado nativo) */
static PyObject* tzd_scalar(tz_i64* p, int kind) {
    if (kind == 'F') return PyFloat_FromDouble(*(double*)p);
    if (kind == 'B') return PyBool_FromLong(*p != 0);
    return PyLong_FromLongLong(*p);
}

static PyObject* tzd_str(tz_i64 addr, tz_i64 n) {
    PyObject* s = PyUnicode_DecodeUTF8((const char*)addr, (Py_ssize_t)n, "strict");
    tzeFreeBytes(addr);
    return s;
}

static PyObject* tzd_list(tz_i64 addr, tz_i64 n, int kind) {
    PyObject* out = PyList_New((Py_ssize_t)n);
    tz_i64* p = (tz_i64*)addr;
    for (tz_i64 i = 0; out && i < n; i++) {
        PyObject* item = tzd_scalar(&p[i], kind);
        if (!item) { Py_CLEAR(out); break; }
        PyList_SET_ITEM(out, (Py_ssize_t)i, item);
    }
    if (kind == 'F') tzeFreeFloats(addr); else tzeFreeInts(addr);
    return out;
}

static PyObject* tzd_strs(tz_i64 buf, tz_i64 offs, tz_i64 n) {
    PyObject* out = PyList_New((Py_ssize_t)n);
    tz_i64* o = (tz_i64*)offs;
    for (tz_i64 i = 0; out && i < n; i++) {
        PyObject* s = PyUnicode_DecodeUTF8((const char*)buf + o[i], (Py_ssize_t)(o[i + 1] - o[i]), "strict");
        if (!s) { Py_CLEAR(out); break; }
        PyList_SET_ITEM(out, (Py_ssize_t)i, s);
    }
    tzeFreeBytes(buf);
    tzeFreeInts(offs);
    return out;
}

static PyObject* tzd_rows(tz_i64 data, tz_i64 lens, tz_i64 n, int kind) {
    PyObject* out = PyList_New((Py_ssize_t)n);
    tz_i64* l = (tz_i64*)lens;
    tz_i64* d = (tz_i64*)data;
    tz_i64 at = 0;
    for (tz_i64 r = 0; out && r < n; r++) {
        PyObject* row = PyList_New((Py_ssize_t)l[r]);
        if (!row) { Py_CLEAR(out); break; }
        for (tz_i64 c = 0; c < l[r]; c++) {
            PyObject* item = tzd_scalar(&d[at++], kind);
            if (!item) { Py_CLEAR(row); break; }
            PyList_SET_ITEM(row, (Py_ssize_t)c, item);
        }
        if (!row) { Py_CLEAR(out); break; }
        PyList_SET_ITEM(out, (Py_ssize_t)r, row);
    }
    if (kind == 'F') tzeFreeFloats(data); else tzeFreeInts(data);
    tzeFreeInts(lens);
    return out;
}

static PyObject* tzd_tuples(const char* sig, tz_i64* cols, tz_i64 n) {
    int width = (int)strlen(sig);
    PyObject* out = PyList_New((Py_ssize_t)n);
    for (tz_i64 i = 0; out && i < n; i++) {
        PyObject* t = PyTuple_New(width);
        if (!t) { Py_CLEAR(out); break; }
        for (int f = 0; f < width; f++) {
            PyObject* item = tzd_scalar(&((tz_i64*)cols[f])[i], sig[f]);
            if (!item) { Py_CLEAR(t); break; }
            PyTuple_SET_ITEM(t, f, item);
        }
        if (!t) { Py_CLEAR(out); break; }
        PyList_SET_ITEM(out, (Py_ssize_t)i, t);
    }
    for (int f = 0; f < width; f++) {
        if (sig[f] == 'F') tzeFreeFloats(cols[f]); else tzeFreeInts(cols[f]);
    }
    return out;
}

/* Valor o None (bandera 1: hay valor). Se queda con `v` en los dos casos. */
static PyObject* tzd_opt(tz_i64 flag, PyObject* v) {
    if (!v) return 0;
    if (flag) return v;
    Py_DECREF(v);
    Py_RETURN_NONE;
}

/* Tupla nueva con `n` elementos ya creados (se queda con ellos). NULL si alguno es NULL. */
static PyObject* tzd_pack(int n, ...) {
    va_list ap;
    va_start(ap, n);
    PyObject* items[8];
    int bad = 0;
    for (int i = 0; i < n; i++) {
        items[i] = va_arg(ap, PyObject*);
        if (!items[i]) bad = 1;
    }
    va_end(ap);
    PyObject* t = bad ? 0 : PyTuple_New(n);
    for (int i = 0; i < n; i++) {
        if (t) PyTuple_SET_ITEM(t, i, items[i]);
        else Py_XDECREF(items[i]);
    }
    return t;
}

/* `lista[:] = valor` (se queda con `valor`). */
static int tzw_set(PyObject* list, PyObject* value) {
    if (!value) return -1;
    int r = PyList_SetSlice(list, 0, PY_SSIZE_T_MAX, value);
    Py_DECREF(value);
    return r;
}

/* `for fila, nueva in zip(lista, valor): fila[:] = nueva` (se queda con `valor`). */
static int tzw_rows(PyObject* list, PyObject* value) {
    if (!value) return -1;
    Py_ssize_t n = PyList_GET_SIZE(list), m = PyList_GET_SIZE(value);
    int r = 0;
    for (Py_ssize_t i = 0; i < n && i < m && r == 0; i++)
        r = PyList_SetSlice(PyList_GET_ITEM(list, i), 0, PY_SSIZE_T_MAX, PyList_GET_ITEM(value, i));
    Py_DECREF(value);
    return r;
}

#ifdef _WIN32
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long*);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long*);
#pragma comment(lib, "kernel32.lib")
static double tz_now(void) {
    static double scale = 0;
    long long c;
    if (!scale) {
        long long f;
        QueryPerformanceFrequency(&f);
        scale = 1.0 / (double)f;
    }
    QueryPerformanceCounter(&c);
    return (double)c * scale;
}
#else
#include <time.h>
static double tz_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}
#endif

/* Objetos de Python que usa la variante. Se fijan una vez (`setup`) antes de guardar el estado de
   la biblioteca, así que restaurarlo tras una parada no los toca. */
static PyObject *tzh_slow, *tzh_trap, *tzh_emit, *tzh_G, *tzh_MISS, *tzh_TRAP1, *tzh_TRAP2, *tzh_review;
static PyObject* tzh_names[64];
static PyObject* tzh_vals[64];
static PyObject* tzh_alts[64];  /* una función del programa vale como la envoltura del motor o como la original */
static int tzh_ndeps;
TZ_EXPORT double tz_t;

static PyObject* tzm_setup(PyObject* self, PyObject* args) {
    PyObject* deps;
    (void)self;
    if (!PyArg_ParseTuple(args, "OOOOOOOOO", &tzh_slow, &tzh_trap, &tzh_emit, &tzh_G, &tzh_MISS, &tzh_TRAP1,
                          &tzh_TRAP2, &deps, &tzh_review))
        return 0;
    Py_INCREF(tzh_slow); Py_INCREF(tzh_trap); Py_INCREF(tzh_emit); Py_INCREF(tzh_G); Py_INCREF(tzh_review);
    Py_INCREF(tzh_MISS); Py_INCREF(tzh_TRAP1); Py_INCREF(tzh_TRAP2);
    tzh_ndeps = 0;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(deps) && i < 64; i++) {
        PyObject* pair = PyTuple_GET_ITEM(deps, i);
        tzh_names[i] = PyTuple_GET_ITEM(pair, 0);
        tzh_vals[i] = PyTuple_GET_ITEM(pair, 1);
        tzh_alts[i] = PyTuple_GET_ITEM(pair, 2);
        Py_INCREF(tzh_names[i]);
        Py_INCREF(tzh_vals[i]);
        Py_INCREF(tzh_alts[i]);
        tzh_ndeps++;
    }
    Py_RETURN_NONE;
}

/* El programa puede haber cambiado una función o constante de la que depende el código nativo. */
static int tz_deps_ok(void) {
    for (int i = 0; i < tzh_ndeps; i++) {
        PyObject* v = PyDict_GetItemWithError(tzh_G, tzh_names[i]);
        if (v != tzh_vals[i] && v != tzh_alts[i]) {
            if (!v && PyErr_Occurred()) PyErr_Clear();
            return 0;
        }
    }
    return 1;
}

/* Tras una parada: Python decide (restaura, cuenta y repite la llamada). */
static PyObject* tz_after_trap(int rc, PyObject* const* args, Py_ssize_t nargs, int probe) {
    tz_rng_forget();
    tz_objs_reset();
    if (probe) return Py_NewRef(rc == 1 ? tzh_TRAP1 : tzh_TRAP2);
    PyObject* stack[33];
    if (nargs > 32) return PyErr_Format(PyExc_RuntimeError, "turipy: demasiados argumentos");
    stack[0] = PyLong_FromLong(rc);
    if (!stack[0]) return 0;
    for (Py_ssize_t i = 0; i < nargs; i++) stack[i + 1] = args[i];
    PyObject* r = PyObject_Vectorcall(tzh_trap, stack, (size_t)nargs + 1, 0);
    Py_DECREF(stack[0]);
    return r;
}

TZ_EXPORT int tz_last_miss;  /* línea del shim de la última comprobación que no pasó (depuración) */
static PyObject* tz_miss(PyObject* const* args, Py_ssize_t nargs, int probe, tz_own* own) {
    tz_own_free(own);
    tz_rng_forget();
    tz_objs_reset();
    if (PyErr_Occurred()) PyErr_Clear();
    if (probe) return Py_NewRef(tzh_MISS);
    return PyObject_Vectorcall(tzh_slow, args, (size_t)nargs, 0);
}

/* Regla de no empeorar, mientras el programa corre: una de cada 16 llamadas se mide (ciclos del
   procesador) cuánto cuesta pasar los datos y cuánto el cálculo. Si en una ventana pasar los datos
   cuesta más de diez veces el cálculo, Python vuelve a comparar las dos versiones. */
static unsigned long long tz_cross_cyc, tz_comp_cyc, tz_window = 100000000ULL;
static unsigned tz_ncalls;

static inline int tz_sampled(int probe) { return !probe && ((++tz_ncalls & 15) == 0); }
static inline unsigned long long tz_cycles(int on) { return on ? __builtin_ia32_rdtsc() : 0; }

/* Fracción de la llamada (desde `c0`) que fue cálculo, según las marcas que pone el código Turi. */
static double tz_share(unsigned long long c0) {
    unsigned long long total = __builtin_ia32_rdtsc() - c0;
    unsigned long long comp = tz_mark[1] - tz_mark[0];
    if (!total || tz_mark[1] < tz_mark[0] || comp > total) return 1.0;
    return (double)comp / (double)total;
}

static int tz_note(unsigned long long c0) {
    unsigned long long total = __builtin_ia32_rdtsc() - c0;
    unsigned long long comp = tz_mark[1] >= tz_mark[0] ? tz_mark[1] - tz_mark[0] : 0;
    if (comp > total) comp = total;
    tz_cross_cyc += total - comp;
    tz_comp_cyc += comp;
    if (tz_cross_cyc + tz_comp_cyc < tz_window) return 0;
    int bad = tz_cross_cyc > 10 * tz_comp_cyc;
    tz_cross_cyc = tz_comp_cyc = 0;
    if (bad) tz_window *= 4;
    return bad;
}

static int tz_review(void) {
    PyObject* r = PyObject_CallNoArgs(tzh_review);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}

static int tz_emit(void) {
    if (!tz_os || !tz_os->len) return 0;
    PyObject* r = PyObject_CallNoArgs(tzh_emit);
    if (!r) return -1;
    Py_DECREF(r);
    return 0;
}
