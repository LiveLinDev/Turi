/* Objetos de clases traducidas en la frontera (paquete C3; motor híbrido, tools/pyfront/turipy.py).

   En nativo, los objetos de cada clase viven en un almacén (un arreglo de structs de Turi) y una
   referencia es su posición (-1 = None). Al entrar se recorren todos los objetos alcanzables desde los
   argumentos (por sus campos y por las listas de objetos) con un mapa de identidad: cada objeto de
   Python tiene una sola posición, así que los alias y los ciclos son los de Python. Sus campos se
   pasan en columnas (una por campo; los textos y las listas como en tzc_strs / tzc_rows).

   Al volver, los campos que el código nativo puede cambiar se escriben en los mismos objetos de
   Python, y los objetos nuevos se crean solo si se llega a ellos (desde el resultado, desde un campo
   o una lista que vuelve a Python): los temporales que el código nativo creó y soltó no cuestan nada.

   Tipos de campo: I F B (escalares), S (texto), O (objeto o None), i f b o (listas de lo mismo). */

#define TZ_CLS_MAX 16
#define TZ_FIELD_MAX 48

typedef struct {
    PyObject* type;
    int nfields;
    PyObject* names[TZ_FIELD_MAX];
    char kinds[TZ_FIELD_MAX];
    int ref[TZ_FIELD_MAX];
    char mutated[TZ_FIELD_MAX];  /* 1: se vuelve a asignar; 2: lista que cambia en sitio */
} tz_class;

typedef struct {
    PyObject** objs;    /* objetos de Python que llegaron (prestados durante la llamada) */
    tz_i64 n, cap;
    PyObject** made;    /* tras la llamada: objeto de Python de cada posición nueva (referencia propia) */
    tz_i64 nout;
    tz_i64* cols[TZ_FIELD_MAX];   /* columnas de salida (las escribió Turi) */
    tz_i64* aux[TZ_FIELD_MAX];    /* desplazamientos (textos) o longitudes (listas) */
    tz_i64* starts[TZ_FIELD_MAX]; /* listas: dónde empieza cada objeto */
} tz_poolrt;

typedef struct {
    tz_class cls[TZ_CLS_MAX];
    int ncls;
    tz_poolrt rt[TZ_CLS_MAX];
    PyObject** keys;     /* mapa de identidad: objeto -> (clase << 40) | posición */
    tz_i64* vals;
    tz_i64 mapcap, mapn;
    tz_i64* work;        /* objetos nuevos cuyos campos faltan por escribir: (clase << 40) | posición */
    tz_i64 nwork, workcap;
    int active;
} tz_objstate;

static tz_objstate* tz_ob;  /* reservado al preparar la variante (antes de guardar el estado) */

static void tz_objs_reset(void) {
    tz_objstate* s = tz_ob;
    if (!s || !s->active) return;
    for (int c = 0; c < s->ncls; c++) {
        tz_poolrt* p = &s->rt[c];
        if (p->made) {
            for (tz_i64 i = 0; i < p->nout - p->n; i++) Py_XDECREF(p->made[i]);
            free(p->made);
        }
        free(p->objs);
        for (int f = 0; f < TZ_FIELD_MAX; f++) free(p->starts[f]);
        memset(p, 0, sizeof *p);
    }
    free(s->keys);
    free(s->vals);
    free(s->work);
    s->keys = 0;
    s->vals = 0;
    s->work = 0;
    s->mapcap = s->mapn = s->nwork = s->workcap = 0;
    s->active = 0;
}

static size_t tz_ptrhash(PyObject* o) {
    size_t h = (size_t)o;
    h ^= h >> 17;
    h *= 0x9E3779B97F4A7C15ULL;
    return h ^ (h >> 29);
}

static int tz_map_grow(void) {
    tz_objstate* s = tz_ob;
    tz_i64 cap = s->mapcap ? s->mapcap * 2 : 1024;
    PyObject** keys = (PyObject**)calloc((size_t)cap, sizeof(PyObject*));
    tz_i64* vals = (tz_i64*)malloc((size_t)cap * sizeof(tz_i64));
    if (!keys || !vals) { free(keys); free(vals); return 0; }
    for (tz_i64 i = 0; i < s->mapcap; i++) {
        if (!s->keys[i]) continue;
        size_t at = tz_ptrhash(s->keys[i]) & (size_t)(cap - 1);
        while (keys[at]) at = (at + 1) & (size_t)(cap - 1);
        keys[at] = s->keys[i];
        vals[at] = s->vals[i];
    }
    free(s->keys);
    free(s->vals);
    s->keys = keys;
    s->vals = vals;
    s->mapcap = cap;
    return 1;
}

/* Posición del objeto `o` (de la clase `c`) en su almacén; lo añade si es nuevo. -1 si no vale. */
static tz_i64 tz_intern(PyObject* o, int c) {
    tz_objstate* s = tz_ob;
    if ((PyObject*)Py_TYPE(o) != s->cls[c].type) return -2;
    if ((s->mapn + 1) * 2 > s->mapcap && !tz_map_grow()) return -2;
    size_t at = tz_ptrhash(o) & (size_t)(s->mapcap - 1);
    while (s->keys[at]) {
        if (s->keys[at] == o) return (s->vals[at] >> 40) == c ? (s->vals[at] & ((1LL << 40) - 1)) : -2;
        at = (at + 1) & (size_t)(s->mapcap - 1);
    }
    tz_poolrt* p = &s->rt[c];
    if (p->n == p->cap) {
        tz_i64 cap = p->cap ? p->cap * 2 : 64;
        PyObject** grown = (PyObject**)realloc(p->objs, (size_t)cap * sizeof(PyObject*));
        if (!grown) return -2;
        p->objs = grown;
        p->cap = cap;
    }
    tz_i64 idx = p->n++;
    p->objs[idx] = o;
    s->keys[at] = o;
    s->vals[at] = ((tz_i64)c << 40) | idx;
    s->mapn++;
    return idx;
}

static void tz_objs_begin(void) {
    if (tz_ob && !tz_ob->active) tz_ob->active = 1;
}

static int tzc_obj(PyObject* o, int c, tz_i64* slot) {
    if (!tz_ob) return 0;
    tz_objs_begin();
    if (o == Py_None) { *slot = -1; return 1; }
    if (c < 0) return 0;  /* campo que solo ha valido None */
    tz_i64 idx = tz_intern(o, c);
    if (idx < 0) return 0;
    *slot = idx;
    return 1;
}

static int tzc_objs(PyObject* list, int c, tz_i64* slot, tz_own* own) {
    if (!tz_ob || !PyList_CheckExact(list)) return 0;
    tz_objs_begin();
    Py_ssize_t n = PyList_GET_SIZE(list);
    tz_i64* buf = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
    if (!buf) return 0;
    for (Py_ssize_t i = 0; i < n; i++)
        if (!tzc_obj(PyList_GET_ITEM(list, i), c, &buf[i])) return 0;
    slot[0] = (tz_i64)buf;
    slot[1] = (tz_i64)n;
    return 1;
}

/* Lee un campo (referencia nueva). */
static PyObject* tz_field(PyObject* o, PyObject* name) {
    PyObject* v = PyObject_GetAttr(o, name);
    if (!v) PyErr_Clear();
    return v;
}

static int tz_field_slots(char kind) { return (kind == 'S' || kind == 'i' || kind == 'f' || kind == 'b' || kind == 'o') ? 2 : 1; }

/* Recorre los objetos alcanzables y escribe las columnas de cada almacén a partir de S[base]: por
   clase, el número de objetos y luego cada campo (1 o 2 posiciones). */
static int tz_pools_encode(tz_i64* S, int base, tz_own* own) {
    tz_objstate* s = tz_ob;
    if (!s) return 1;
    tz_objs_begin();
    /* 1: descubrir (los campos de objeto y las listas de objetos añaden objetos). */
    int grew = 1;
    tz_i64 done[TZ_CLS_MAX] = {0};
    while (grew) {
        grew = 0;
        for (int c = 0; c < s->ncls; c++) {
            tz_class* k = &s->cls[c];
            tz_poolrt* p = &s->rt[c];
            for (; done[c] < p->n; done[c]++) {
                grew = 1;
                PyObject* o = p->objs[done[c]];
                for (int f = 0; f < k->nfields; f++) {
                    if (k->kinds[f] != 'O' && k->kinds[f] != 'o') continue;
                    PyObject* v = tz_field(o, k->names[f]);
                    if (!v) return 0;
                    int ok = 1;
                    if (k->kinds[f] == 'O') {
                        tz_i64 t;
                        ok = v == Py_None || tzc_obj(v, k->ref[f], &t);
                    } else if (!PyList_CheckExact(v)) {
                        ok = 0;
                    } else {
                        for (Py_ssize_t i = 0; ok && i < PyList_GET_SIZE(v); i++) {
                            tz_i64 t;
                            ok = tzc_obj(PyList_GET_ITEM(v, i), k->ref[f], &t);
                        }
                    }
                    Py_DECREF(v);
                    if (!ok) return 0;
                }
            }
        }
    }
    /* 2: columnas. */
    int at = base;
    for (int c = 0; c < s->ncls; c++) {
        tz_class* k = &s->cls[c];
        tz_poolrt* p = &s->rt[c];
        tz_i64 n = p->n;
        S[at++] = n;
        for (int f = 0; f < k->nfields; f++) {
            char kind = k->kinds[f];
            if (kind == 'I' || kind == 'F' || kind == 'B' || kind == 'O') {
                tz_i64* col = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
                if (!col) return 0;
                for (tz_i64 i = 0; i < n; i++) {
                    PyObject* v = tz_field(p->objs[i], k->names[f]);
                    if (!v) return 0;
                    int ok = kind == 'O' ? tzc_obj(v, k->ref[f], &col[i]) : tzc_scalar(v, kind, &col[i]);
                    Py_DECREF(v);
                    if (!ok) return 0;
                }
                S[at++] = (tz_i64)col;
            } else {
                /* texto: como tzc_strs; lista: como tzc_rows (datos y longitudes) */
                PyObject* items = PyList_New((Py_ssize_t)n);
                if (!items) { PyErr_Clear(); return 0; }
                int ok = 1;
                for (tz_i64 i = 0; ok && i < n; i++) {
                    PyObject* v = tz_field(p->objs[i], k->names[f]);
                    if (!v) { ok = 0; break; }
                    PyList_SET_ITEM(items, (Py_ssize_t)i, v);
                }
                if (ok && kind == 'S') {
                    tz_i64 text[3];
                    ok = tzc_strs(items, text, own);
                    if (ok) { S[at] = text[0]; S[at + 1] = text[1]; }
                } else if (ok && kind == 'o') {
                    /* filas de posiciones */
                    tz_i64* lens = (tz_i64*)tz_own_alloc(own, (size_t)(n + 1) * 8);
                    tz_i64 total = 0;
                    for (tz_i64 i = 0; ok && lens && i < n; i++) {
                        PyObject* row = PyList_GET_ITEM(items, (Py_ssize_t)i);
                        ok = PyList_CheckExact(row);
                        lens[i] = ok ? PyList_GET_SIZE(row) : 0;
                        total += lens[i];
                    }
                    tz_i64* data = ok && lens ? (tz_i64*)tz_own_alloc(own, (size_t)(total + 1) * 8) : 0;
                    tz_i64 pos = 0;
                    for (tz_i64 i = 0; ok && data && i < n; i++) {
                        PyObject* row = PyList_GET_ITEM(items, (Py_ssize_t)i);
                        for (Py_ssize_t j = 0; ok && j < PyList_GET_SIZE(row); j++)
                            ok = tzc_obj(PyList_GET_ITEM(row, j), k->ref[f], &data[pos++]);
                    }
                    if (!lens || !data) ok = 0;
                    if (ok) { S[at] = (tz_i64)data; S[at + 1] = (tz_i64)lens; }
                } else if (ok) {
                    tz_i64 rows[3];
                    ok = tzc_rows(items, kind == 'f' ? 'F' : kind == 'b' ? 'B' : 'I', rows, own, k->mutated[f] == 2);
                    if (ok) { S[at] = rows[0]; S[at + 1] = rows[1]; }
                }
                Py_DECREF(items);
                if (!ok) return 0;
                at += 2;
            }
        }
    }
    return 1;
}

/* ---- vuelta */
static PyObject* tz_obj_value(int c, tz_i64 idx);

static PyObject* tz_field_value(int c, int f, tz_i64 idx) {
    tz_class* k = &tz_ob->cls[c];
    tz_poolrt* p = &tz_ob->rt[c];
    char kind = k->kinds[f];
    tz_i64* col = p->cols[f];
    if (kind == 'I' || kind == 'F' || kind == 'B') return tzd_scalar(&col[idx], kind);
    if (kind == 'O') return tz_obj_value(k->ref[f], col[idx]);
    if (kind == 'S') {
        tz_i64* offs = p->aux[f];
        return PyUnicode_DecodeUTF8((const char*)col + offs[idx], (Py_ssize_t)(offs[idx + 1] - offs[idx]), "strict");
    }
    tz_i64* lens = p->aux[f];
    if (!p->starts[f]) {
        p->starts[f] = (tz_i64*)malloc((size_t)(p->nout + 1) * 8);
        if (!p->starts[f]) return PyErr_NoMemory();
        tz_i64 at = 0;
        for (tz_i64 i = 0; i < p->nout; i++) { p->starts[f][i] = at; at += lens[i]; }
    }
    tz_i64 start = p->starts[f][idx], m = lens[idx];
    PyObject* out = PyList_New((Py_ssize_t)m);
    for (tz_i64 j = 0; out && j < m; j++) {
        PyObject* item = kind == 'o' ? tz_obj_value(k->ref[f], col[start + j])
                                     : tzd_scalar(&col[start + j], kind == 'f' ? 'F' : kind == 'b' ? 'B' : 'I');
        if (!item) { Py_CLEAR(out); break; }
        PyList_SET_ITEM(out, (Py_ssize_t)j, item);
    }
    return out;
}

/* Objeto de Python para la posición `idx` del almacén `c` (referencia nueva). Los nuevos se crean
   aquí, sin campos; tz_objs_finish los rellena. */
static PyObject* tz_obj_value(int c, tz_i64 idx) {
    if (idx == -1) Py_RETURN_NONE;
    tz_objstate* s = tz_ob;
    tz_poolrt* p = &s->rt[c];
    if (idx < 0 || idx >= p->nout) return PyErr_Format(PyExc_RuntimeError, "turipy: referencia fuera del almacén");
    if (idx < p->n) return Py_NewRef(p->objs[idx]);
    tz_i64 k = idx - p->n;
    if (p->made[k]) return Py_NewRef(p->made[k]);
    PyObject* type = s->cls[c].type;
    PyObject* obj = PyObject_CallMethod(type, "__new__", "(O)", type);
    if (!obj) return 0;
    p->made[k] = obj;
    if (s->nwork == s->workcap) {
        tz_i64 cap = s->workcap ? s->workcap * 2 : 256;
        tz_i64* grown = (tz_i64*)realloc(s->work, (size_t)cap * 8);
        if (!grown) return PyErr_NoMemory();
        s->work = grown;
        s->workcap = cap;
    }
    s->work[s->nwork++] = ((tz_i64)c << 40) | idx;
    return Py_NewRef(obj);
}

/* Columnas que escribió Turi (a partir de S[base], el mismo formato que la entrada). Escribe los
   campos que cambian en los objetos que llegaron. */
static int tz_pools_decode(tz_i64* S, int base) {
    tz_objstate* s = tz_ob;
    if (!s) return 0;
    int at = base;
    for (int c = 0; c < s->ncls; c++) {
        tz_class* k = &s->cls[c];
        tz_poolrt* p = &s->rt[c];
        p->nout = S[at++];
        if (p->nout < p->n) { PyErr_SetString(PyExc_RuntimeError, "turipy: el almacén encogió"); return -1; }
        p->made = (PyObject**)calloc((size_t)(p->nout - p->n + 1), sizeof(PyObject*));
        if (!p->made) { PyErr_NoMemory(); return -1; }
        for (int f = 0; f < k->nfields; f++) {
            p->cols[f] = (tz_i64*)S[at];
            p->aux[f] = tz_field_slots(k->kinds[f]) == 2 ? (tz_i64*)S[at + 1] : 0;
            at += tz_field_slots(k->kinds[f]);
        }
    }
    for (int c = 0; c < s->ncls; c++) {
        tz_class* k = &s->cls[c];
        tz_poolrt* p = &s->rt[c];
        for (int f = 0; f < k->nfields; f++) {
            if (!k->mutated[f]) continue;
            for (tz_i64 i = 0; i < p->n; i++) {
                PyObject* v = tz_field_value(c, f, i);
                if (!v) return -1;
                int r;
                if (k->mutated[f] == 2) {  /* la misma lista, cambiada en sitio */
                    PyObject* old = tz_field(p->objs[i], k->names[f]);
                    r = old && PyList_CheckExact(old) ? PyList_SetSlice(old, 0, PY_SSIZE_T_MAX, v) : -1;
                    Py_XDECREF(old);
                } else {
                    r = PyObject_SetAttr(p->objs[i], k->names[f], v);
                }
                Py_DECREF(v);
                if (r < 0) return -1;
            }
        }
    }
    return 0;
}

/* Rellena los campos de los objetos nuevos a los que se llegó y libera las columnas de Turi. */
static int tz_objs_finish(void) {
    tz_objstate* s = tz_ob;
    int status = 0;
    if (!s || !s->active) return 0;
    while (status == 0 && s->nwork) {
        tz_i64 w = s->work[--s->nwork];
        int c = (int)(w >> 40);
        tz_i64 idx = w & ((1LL << 40) - 1);
        PyObject* obj = s->rt[c].made[idx - s->rt[c].n];
        tz_class* k = &s->cls[c];
        for (int f = 0; f < k->nfields && status == 0; f++) {
            PyObject* v = tz_field_value(c, f, idx);
            if (!v || PyObject_SetAttr(obj, k->names[f], v) < 0) status = -1;
            Py_XDECREF(v);
        }
    }
    for (int c = 0; c < s->ncls; c++) {
        tz_class* k = &s->cls[c];
        tz_poolrt* p = &s->rt[c];
        for (int f = 0; f < k->nfields; f++) {
            char kind = k->kinds[f];
            if (!p->cols[f]) continue;
            if (kind == 'S') { tzeFreeBytes((tz_i64)p->cols[f]); tzeFreeInts((tz_i64)p->aux[f]); }
            else if (kind == 'F' || kind == 'f') tzeFreeFloats((tz_i64)p->cols[f]);
            else tzeFreeInts((tz_i64)p->cols[f]);
            if (kind != 'S' && p->aux[f]) tzeFreeInts((tz_i64)p->aux[f]);
            p->cols[f] = p->aux[f] = 0;
        }
    }
    tz_objs_reset();
    return status;
}

static PyObject* tzd_obj(int c, tz_i64 idx) { return tz_obj_value(c, idx); }

static PyObject* tzd_objs(int c, tz_i64 addr, tz_i64 n) {
    PyObject* out = PyList_New((Py_ssize_t)n);
    tz_i64* p = (tz_i64*)addr;
    for (tz_i64 i = 0; out && i < n; i++) {
        PyObject* item = tz_obj_value(c, p[i]);
        if (!item) { Py_CLEAR(out); break; }
        PyList_SET_ITEM(out, (Py_ssize_t)i, item);
    }
    tzeFreeInts(addr);
    return out;
}

/* classes(((tipo, ((nombre, clase_tipo, ref, cambia), ...)), ...)): una vez, al preparar la variante. */
static PyObject* tzm_classes(PyObject* self, PyObject* arg) {
    (void)self;
    if (!tz_ob) tz_ob = (tz_objstate*)calloc(1, sizeof(tz_objstate));
    if (!tz_ob) return PyErr_NoMemory();
    Py_ssize_t n = PyTuple_GET_SIZE(arg);
    if (n > TZ_CLS_MAX) return PyErr_Format(PyExc_RuntimeError, "turipy: demasiadas clases");
    for (Py_ssize_t c = 0; c < n; c++) {
        PyObject* item = PyTuple_GET_ITEM(arg, c);
        tz_class* k = &tz_ob->cls[c];
        k->type = PyTuple_GET_ITEM(item, 0);
        Py_INCREF(k->type);
        PyObject* fields = PyTuple_GET_ITEM(item, 1);
        k->nfields = (int)PyTuple_GET_SIZE(fields);
        if (k->nfields > TZ_FIELD_MAX) return PyErr_Format(PyExc_RuntimeError, "turipy: demasiados campos");
        for (int f = 0; f < k->nfields; f++) {
            PyObject* fd = PyTuple_GET_ITEM(fields, f);
            k->names[f] = PyTuple_GET_ITEM(fd, 0);
            Py_INCREF(k->names[f]);
            k->kinds[f] = (char)PyLong_AsLong(PyTuple_GET_ITEM(fd, 1));
            k->ref[f] = (int)PyLong_AsLong(PyTuple_GET_ITEM(fd, 2));
            k->mutated[f] = (char)PyLong_AsLong(PyTuple_GET_ITEM(fd, 3));
        }
    }
    tz_ob->ncls = (int)n;
    Py_RETURN_NONE;
}
