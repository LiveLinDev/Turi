#pragma once
#include <string>

namespace veloz::native {

namespace pg_rt_parts {

inline constexpr const char* kPgHeader = R"c(
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sched.h>
#endif

typedef struct tz_pg_turi_str {
    uint32_t hdr;
    uint32_t word1;
    const char* ptr;
} tz_pg_turi_str;

_Static_assert(sizeof(tz_pg_turi_str) == 16, "Turi %text size mismatch");
_Static_assert(offsetof(tz_pg_turi_str, word1) == 4, "Turi %text word1 offset mismatch");
_Static_assert(offsetof(tz_pg_turi_str, ptr) == 8, "Turi %text ptr offset mismatch");

static _Thread_local char tz_pg_err[512];

static inline void tz_pg_set_err(const void** out_ptr, int64_t* out_len, const char* msg) {
    if (!msg || !msg[0]) {
        if (out_ptr) *out_ptr = (const void*)"";
        if (out_len) *out_len = 0;
        return;
    }
    size_t n = strlen(msg);
    if (n >= sizeof(tz_pg_err)) n = sizeof(tz_pg_err) - 1u;
    memcpy(tz_pg_err, msg, n);
    tz_pg_err[n] = '\0';
    if (out_ptr) *out_ptr = (const void*)tz_pg_err;
    if (out_len) *out_len = (int64_t)n;
}

static inline const char* tz_pg_str_view(const void* p, size_t* len) {
    if (!p) { *len = 0; return ""; }
    const tz_pg_turi_str* ts = (const tz_pg_turi_str*)p;
    size_t n = (size_t)(ts->hdr & 0x7FFFFFFFu);
    *len = n;
    if (n == 0u) return "";
    if (n <= 12u) return (const char*)&ts->word1;
    return ts->ptr ? ts->ptr : "";
}

static inline char* tz_pg_dup(const char* p, size_t n) {
    char* r = (char*)malloc(n + 1u);
    if (!r) return NULL;
    if (n > 0u && p) memcpy(r, p, n);
    r[n] = '\0';
    return r;
}

#define TZ_PG_CONNECTION_OK 0
#define TZ_PG_EMPTY_QUERY 0
#define TZ_PG_COMMAND_OK 1
#define TZ_PG_TUPLES_OK 2

static struct {
    int loaded;
    int ok;
    void* (*connectdb)(const char*);
    int (*status)(const void*);
    char* (*error_msg)(const void*);
    void (*finish)(void*);
    void* (*exec_params)(void*, const char*, int, const unsigned int*, const char* const*, const int*, const int*, int);
    int (*result_status)(const void*);
    int (*ntuples)(const void*);
    int (*nfields)(const void*);
    char* (*fname)(const void*, int);
    int (*getisnull)(const void*, int, int);
    char* (*getvalue)(const void*, int, int);
    void (*clear)(void*);
} tz_pg_api = {0};

static int tz_pg_sym(
#ifdef _WIN32
    HMODULE mod,
#else
    void* mod,
#endif
    const char* name, void* dst, size_t sz) {
#ifdef _WIN32
    FARPROC p = GetProcAddress(mod, name);
    if (!p) return 0;
    memcpy(dst, &p, sz);
    return 1;
#else
    void* p = dlsym(mod, name);
    if (!p) return 0;
    memcpy(dst, &p, sz);
    return 1;
#endif
}

static int tz_pg_init_api(void) {
    if (__atomic_load_n(&tz_pg_api.loaded, __ATOMIC_ACQUIRE) == 2) return tz_pg_api.ok;
    int exp = 0;
    if (__atomic_compare_exchange_n(&tz_pg_api.loaded, &exp, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        int ok = 0;
        const char* env_dll = getenv("TURI_LIBPQ_DLL");
#ifdef _WIN32
        const char* cands[5] = {
            env_dll,
            "libpq.dll",
            "C:\\Program Files\\PostgreSQL\\18\\bin\\libpq.dll",
            "C:\\Program Files\\PostgreSQL\\17\\bin\\libpq.dll",
            "C:\\Program Files\\PostgreSQL\\16\\bin\\libpq.dll"
        };
        for (int i = 0; i < 5 && !ok; ++i) {
            if (!cands[i] || !cands[i][0]) continue;
            HMODULE mod = (strchr(cands[i], '\\') || strchr(cands[i], '/'))
                ? LoadLibraryExA(cands[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH)
                : LoadLibraryA(cands[i]);
#else
        const char* cands[4] = { env_dll, "libpq.so.5", "libpq.so", "libpq.dylib" };
        for (int i = 0; i < 4 && !ok; ++i) {
            if (!cands[i] || !cands[i][0]) continue;
            void* mod = dlopen(cands[i], RTLD_LAZY | RTLD_LOCAL);
#endif
            if (!mod) continue;
            #define TZ_PQ(f, n) tz_pg_sym(mod, n, &tz_pg_api.f, sizeof(tz_pg_api.f))
            if (TZ_PQ(connectdb, "PQconnectdb") &&
                TZ_PQ(status, "PQstatus") &&
                TZ_PQ(error_msg, "PQerrorMessage") &&
                TZ_PQ(finish, "PQfinish") &&
                TZ_PQ(exec_params, "PQexecParams") &&
                TZ_PQ(result_status, "PQresultStatus") &&
                TZ_PQ(ntuples, "PQntuples") &&
                TZ_PQ(nfields, "PQnfields") &&
                TZ_PQ(fname, "PQfname") &&
                TZ_PQ(getisnull, "PQgetisnull") &&
                TZ_PQ(getvalue, "PQgetvalue") &&
                TZ_PQ(clear, "PQclear")) {
                ok = 1;
            }
            #undef TZ_PQ
        }
        tz_pg_api.ok = ok;
        __atomic_store_n(&tz_pg_api.loaded, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&tz_pg_api.loaded, __ATOMIC_ACQUIRE) != 2) {
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }
    return tz_pg_api.ok;
}
)c";

inline constexpr const char* kPgBody = R"c(
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} tz_pg_buf;

static int tz_pg_buf_append(tz_pg_buf* b, const char* s, size_t n) {
    if (b->len + n + 1u > b->cap) {
        size_t nc = b->cap ? b->cap * 2u : 512u;
        while (nc < b->len + n + 1u) nc *= 2u;
        char* p = (char*)realloc(b->data, nc);
        if (!p) return 0;
        b->data = p;
        b->cap = nc;
    }
    if (n > 0u && s) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int tz_pg_append_json_str(tz_pg_buf* b, const char* s) {
    if (!s) return tz_pg_buf_append(b, "null", 4u);
    if (!tz_pg_buf_append(b, "\"", 1u)) return 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        unsigned char c = *p;
        if (c == '"') { if (!tz_pg_buf_append(b, "\\\"", 2u)) return 0; }
        else if (c == '\\') { if (!tz_pg_buf_append(b, "\\\\", 2u)) return 0; }
        else if (c == '\b') { if (!tz_pg_buf_append(b, "\\b", 2u)) return 0; }
        else if (c == '\f') { if (!tz_pg_buf_append(b, "\\f", 2u)) return 0; }
        else if (c == '\n') { if (!tz_pg_buf_append(b, "\\n", 2u)) return 0; }
        else if (c == '\r') { if (!tz_pg_buf_append(b, "\\r", 2u)) return 0; }
        else if (c == '\t') { if (!tz_pg_buf_append(b, "\\t", 2u)) return 0; }
        else if (c < 0x20u) {
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
            if (!tz_pg_buf_append(b, esc, 6u)) return 0;
        } else {
            char ch = (char)c;
            if (!tz_pg_buf_append(b, &ch, 1u)) return 0;
        }
    }
    return tz_pg_buf_append(b, "\"", 1u);
}

int32_t tz_pg_available(void) {
    return tz_pg_init_api() ? 1 : 0;
}

void* tz_pg_connect(const void* conninfo_tstr, const void** err_ptr, int64_t* err_len) {
    tz_pg_set_err(err_ptr, err_len, NULL);
    if (!tz_pg_init_api()) {
        tz_pg_set_err(err_ptr, err_len, "libpq no disponible en el sistema");
        return NULL;
    }
    size_t n = 0;
    const char* s = tz_pg_str_view(conninfo_tstr, &n);
    char* cstr = tz_pg_dup(s, n);
    if (!cstr) {
        tz_pg_set_err(err_ptr, err_len, "Sin memoria al conectar a PostgreSQL");
        return NULL;
    }
    void* conn = tz_pg_api.connectdb(cstr);
    free(cstr);
    if (!conn || tz_pg_api.status(conn) != TZ_PG_CONNECTION_OK) {
        const char* msg = conn ? tz_pg_api.error_msg(conn) : "Error conectando a PostgreSQL";
        tz_pg_set_err(err_ptr, err_len, msg && msg[0] ? msg : "Error conectando a PostgreSQL");
        if (conn) tz_pg_api.finish(conn);
        return NULL;
    }
    return conn;
}

void tz_pg_close(void* conn) {
    if (conn && tz_pg_init_api()) tz_pg_api.finish(conn);
}

void tz_pg_free_buf(void* p) {
    free(p);
}

int32_t tz_pg_exec_json(void* conn, const void* sql_tstr, const void* params_packed_tstr,
                        int32_t param_count, uint8_t** out_ptr, int64_t* out_len,
                        const void** err_ptr, int64_t* err_len) {
    tz_pg_set_err(err_ptr, err_len, NULL);
    if (out_ptr) *out_ptr = NULL;
    if (out_len) *out_len = 0;
    if (!conn || !tz_pg_init_api()) {
        tz_pg_set_err(err_ptr, err_len, "Conexion PostgreSQL invalida");
        return 0;
    }
    size_t sqlen = 0, plen = 0;
    const char* sql_v = tz_pg_str_view(sql_tstr, &sqlen);
    const char* pdata = tz_pg_str_view(params_packed_tstr, &plen);
    char* sql = tz_pg_dup(sql_v, sqlen);
    if (!sql) { tz_pg_set_err(err_ptr, err_len, "Sin memoria"); return 0; }

    const char** pvals = NULL;
    if (param_count > 0) {
        pvals = (const char**)calloc((size_t)param_count, sizeof(char*));
        size_t pos = 0u;
        for (int32_t i = 0; i < param_count; ++i) {
            size_t flen = 0u;
            int has_colon = 0;
            while (pos < plen && pdata[pos] >= '0' && pdata[pos] <= '9') {
                flen = flen * 10u + (size_t)(pdata[pos] - '0');
                pos++;
            }
            if (pos < plen && pdata[pos] == ':') { has_colon = 1; pos++; }
            if (!has_colon || pos + flen > plen) {
                for (int32_t j = 0; j < i; ++j) free((void*)pvals[j]);
                free((void*)pvals);
                free(sql);
                tz_pg_set_err(err_ptr, err_len, "Parametros empaquetados invalidos");
                return 0;
            }
            pvals[i] = tz_pg_dup(pdata + pos, flen);
            pos += flen;
        }
    }

    void* res = tz_pg_api.exec_params(conn, sql, param_count, NULL, pvals, NULL, NULL, 0);
    if (pvals) {
        for (int32_t i = 0; i < param_count; ++i) free((void*)pvals[i]);
        free((void*)pvals);
    }
    free(sql);

    if (!res) {
        tz_pg_set_err(err_ptr, err_len, tz_pg_api.error_msg(conn));
        return 0;
    }
    int st = tz_pg_api.result_status(res);
    if (st != TZ_PG_COMMAND_OK && st != TZ_PG_TUPLES_OK && st != TZ_PG_EMPTY_QUERY) {
        const char* em = tz_pg_api.error_msg(conn);
        tz_pg_set_err(err_ptr, err_len, em && em[0] ? em : "Error ejecutando consulta PostgreSQL");
        tz_pg_api.clear(res);
        return 0;
    }

    tz_pg_buf jb = { NULL, 0u, 0u };
    if (!tz_pg_buf_append(&jb, "[", 1u)) { tz_pg_api.clear(res); return 0; }
    if (st == TZ_PG_TUPLES_OK) {
        int rows = tz_pg_api.ntuples(res);
        int cols = tz_pg_api.nfields(res);
        for (int r = 0; r < rows; ++r) {
            if (r > 0) tz_pg_buf_append(&jb, ",", 1u);
            tz_pg_buf_append(&jb, "{", 1u);
            for (int c = 0; c < cols; ++c) {
                if (c > 0) tz_pg_buf_append(&jb, ",", 1u);
                tz_pg_append_json_str(&jb, tz_pg_api.fname(res, c));
                tz_pg_buf_append(&jb, ":", 1u);
                if (tz_pg_api.getisnull(res, r, c)) tz_pg_buf_append(&jb, "null", 4u);
                else tz_pg_append_json_str(&jb, tz_pg_api.getvalue(res, r, c));
            }
            tz_pg_buf_append(&jb, "}", 1u);
        }
    }
    tz_pg_buf_append(&jb, "]", 1u);
    tz_pg_api.clear(res);
    if (out_ptr) *out_ptr = (uint8_t*)jb.data;
    if (out_len) *out_len = (int64_t)jb.len;
    return 1;
}
)c";

} // namespace pg_rt_parts

inline std::string turiPgRuntimeC() {
    std::string out;
    out.reserve(16384);
    out += pg_rt_parts::kPgHeader;
    out += pg_rt_parts::kPgBody;
    return out;
}

} // namespace veloz::native
