#pragma once

namespace veloz::native {

inline const char* turiSysRuntimeC() {
    return R"c(
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef TZ_USE_NET
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <windows.h>
#include <malloc.h>
#include <sys/stat.h>
#else
#include <errno.h>
#include <pthread.h>
#ifdef TZ_USE_NET
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#include <errno.h>

typedef struct tz_text {
    const char* ptr;
    int64_t len;
} tz_text;

static char* tz_dup_n(const char* p, int64_t n) {
    size_t len = n > 0 ? (size_t)n : 0;
    char* buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    if (len > 0 && p) memcpy(buf, p, len);
    buf[len] = '\0';
    return buf;
}

#ifdef _WIN32
static wchar_t* tz_to_wide(const char* p, int64_t n) {
    if (n <= 0 || !p) {
        wchar_t* empty = (wchar_t*)malloc(sizeof(wchar_t));
        if (empty) empty[0] = L'\0';
        return empty;
    }
    int bytes = (int)n;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, p, bytes, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, p, bytes, w, wlen);
    w[wlen] = L'\0';
    return w;
}
#endif

static int tz_g_argc = 0;
static char** tz_g_argv = NULL;

void tz_sys_set_args(int argc, char** argv) {
    tz_g_argc = argc;
    tz_g_argv = argv;
}

int64_t tz_sys_args_count(void) {
    return (int64_t)tz_g_argc;
}

void tz_sys_arg_at(int64_t idx, tz_text* out) {
    if (idx < 0 || idx >= (int64_t)tz_g_argc || !tz_g_argv || !tz_g_argv[idx]) {
        out->ptr = "";
        out->len = 0;
        return;
    }
    const char* s = tz_g_argv[idx];
    out->ptr = s;
    out->len = (int64_t)strlen(s);
}

int64_t tz_sys_format_float(double v, char* buf, int64_t cap) {
    if (cap <= 0 || !buf) return 0;
    if (isnan(v)) {
        if (cap < 3) return 0;
        memcpy(buf, "NaN", 3);
        return 3;
    }
    if (isinf(v)) {
        if (v < 0.0) {
            if (cap < 4) return 0;
            memcpy(buf, "-Inf", 4);
            return 4;
        }
        if (cap < 3) return 0;
        memcpy(buf, "Inf", 3);
        return 3;
    }
    if (v == 0.0) v = 0.0;
    int k = snprintf(buf, (size_t)cap, "%.15g", v);
    return k > 0 ? (int64_t)k : 0;
}

uint8_t tz_sys_parse_float(const char* p, int64_t n, double* out) {
    if (!out) return 0;
    *out = 0.0;
    if (!p || n <= 0) return 0;
    int64_t i = 0;
    if (p[i] == '+' || p[i] == '-') ++i;
    int64_t digits = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') { ++i; ++digits; }
    if (i < n && p[i] == '.') {
        ++i;
        while (i < n && p[i] >= '0' && p[i] <= '9') { ++i; ++digits; }
    }
    if (digits == 0) return 0;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        ++i;
        if (i < n && (p[i] == '+' || p[i] == '-')) ++i;
        int64_t expDigits = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') { ++i; ++expDigits; }
        if (expDigits == 0) return 0;
    }
    if (i != n) return 0;
    char* copy = tz_dup_n(p, n);
    if (!copy) return 0;
    char* endPtr = NULL;
    errno = 0;
    double v = strtod(copy, &endPtr);
    uint8_t ok = (endPtr == copy + n && errno != ERANGE && isfinite(v)) ? 1 : 0;
    free(copy);
    if (ok) *out = v;
    return ok;
}

void tz_sys_print_float(double v) {
    char buf[64];
    int64_t k = tz_sys_format_float(v, buf, (int64_t)sizeof(buf) - 1);
    buf[k] = '\n';
    fwrite(buf, 1, (size_t)(k + 1), stdout);
}

int64_t tz_sys_format_u64(uint64_t v, char* buf, int64_t cap) {
    char tmp[32];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v > 0) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    int64_t out = 0;
    while (n > 0 && out < cap) {
        buf[out++] = tmp[--n];
    }
    return out;
}

void tz_sys_print_u64(uint64_t v) {
    char buf[32];
    int64_t k = tz_sys_format_u64(v, buf, (int64_t)sizeof(buf) - 1);
    buf[k] = '\n';
    fwrite(buf, 1, (size_t)(k + 1), stdout);
}

void tz_sys_read_file(const char* p, int64_t n, tz_text* out) {
    out->ptr = NULL;
    out->len = 0;
    if (n <= 0 || !p) return;
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, n);
    if (!wp) return;
    FILE* f = _wfopen(wp, L"rb");
    free(wp);
#else
    char* path = tz_dup_n(p, n);
    if (!path) return;
    FILE* f = fopen(path, "rb");
    free(path);
#endif
    if (!f) return;
    size_t cap = 4096;
    size_t used = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        fclose(f);
        return;
    }
    for (;;) {
        if (used == cap) {
            size_t nextCap = cap * 2;
            char* nextBuf = (char*)realloc(buf, nextCap);
            if (!nextBuf) {
                free(buf);
                fclose(f);
                return;
            }
            buf = nextBuf;
            cap = nextCap;
        }
        size_t got = fread(buf + used, 1, cap - used, f);
        used += got;
        if (got == 0) break;
    }
    fclose(f);
    if (used == 0) {
        free(buf);
        return;
    }
    out->ptr = buf;
    out->len = (int64_t)used;
}

static uint8_t tz_write_file_impl(const char* p, int64_t pn, const char* c, int64_t cn, int append) {
    if (pn <= 0 || !p) return 0;
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, pn);
    if (!wp) return 0;
    FILE* f = _wfopen(wp, append ? L"ab" : L"wb");
    free(wp);
#else
    char* path = tz_dup_n(p, pn);
    if (!path) return 0;
    FILE* f = fopen(path, append ? "ab" : "wb");
    free(path);
#endif
    if (!f) return 0;
    int ok = 1;
    if (cn > 0 && c) {
        size_t written = fwrite(c, 1, (size_t)cn, f);
        if (written != (size_t)cn) ok = 0;
    }
    if (fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    return (uint8_t)ok;
}

uint8_t tz_sys_write_file(const char* p, int64_t pn, const char* c, int64_t cn) {
    return tz_write_file_impl(p, pn, c, cn, 0);
}

uint8_t tz_sys_append_file(const char* p, int64_t pn, const char* c, int64_t cn) {
    return tz_write_file_impl(p, pn, c, cn, 1);
}

static void tz_set_err(tz_text* err, const char* msg);

uint8_t tz_sys_try_read_file(const char* p, int64_t n, tz_text* out, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    out->ptr = NULL;
    out->len = 0;
    if (n <= 0 || !p) {
        tz_set_err(errOut, "Ruta de archivo vacía");
        return 0;
    }
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, n);
    if (!wp) {
        tz_set_err(errOut, "Ruta UTF-8 inválida");
        return 0;
    }
    FILE* f = _wfopen(wp, L"rb");
    free(wp);
#else
    char* path = tz_dup_n(p, n);
    if (!path) {
        tz_set_err(errOut, "Sin memoria");
        return 0;
    }
    FILE* f = fopen(path, "rb");
    free(path);
#endif
    if (!f) {
        tz_set_err(errOut, "No se pudo abrir el archivo");
        return 0;
    }
    size_t cap = 4096;
    size_t used = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        fclose(f);
        tz_set_err(errOut, "Sin memoria al leer");
        return 0;
    }
    for (;;) {
        if (used == cap) {
            size_t nextCap = cap * 2;
            char* nextBuf = (char*)realloc(buf, nextCap);
            if (!nextBuf) {
                free(buf);
                fclose(f);
                tz_set_err(errOut, "Sin memoria al leer");
                return 0;
            }
            buf = nextBuf;
            cap = nextCap;
        }
        size_t got = fread(buf + used, 1, cap - used, f);
        used += got;
        if (got == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        free(buf);
        tz_set_err(errOut, "Error de lectura en archivo");
        return 0;
    }
    if (used == 0) {
        free(buf);
        return 1;
    }
    out->ptr = buf;
    out->len = (int64_t)used;
    return 1;
}

uint8_t tz_sys_try_write_file(const char* p, int64_t pn, const char* c, int64_t cn, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    if (pn <= 0 || !p) {
        tz_set_err(errOut, "Ruta de archivo vacía");
        return 0;
    }
    if (!tz_write_file_impl(p, pn, c, cn, 0)) {
        tz_set_err(errOut, "No se pudo escribir el archivo");
        return 0;
    }
    return 1;
}

uint8_t tz_sys_file_exists(const char* p, int64_t pn) {
    if (pn <= 0 || !p) return 0;
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, pn);
    if (!wp) return 0;
    DWORD attr = GetFileAttributesW(wp);
    free(wp);
    return attr != INVALID_FILE_ATTRIBUTES ? 1 : 0;
#else
    char* path = tz_dup_n(p, pn);
    if (!path) return 0;
    struct stat st;
    int ok = stat(path, &st) == 0;
    free(path);
    return (uint8_t)(ok ? 1 : 0);
#endif
}

uint8_t tz_sys_remove_file(const char* p, int64_t pn) {
    if (pn <= 0 || !p) return 0;
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, pn);
    if (!wp) return 0;
    int ok = _wremove(wp) == 0;
    if (!ok) ok = RemoveDirectoryW(wp) != 0;
    free(wp);
    return (uint8_t)(ok ? 1 : 0);
#else
    char* path = tz_dup_n(p, pn);
    if (!path) return 0;
    int ok = remove(path) == 0;
    free(path);
    return (uint8_t)(ok ? 1 : 0);
#endif
}

uint8_t tz_sys_make_dir(const char* p, int64_t pn) {
    if (pn <= 0 || !p) return 0;
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, pn);
    if (!wp) return 0;
    size_t wlen = wcslen(wp);
    while (wlen > 0 && (wp[wlen - 1] == L'/' || wp[wlen - 1] == L'\\')) {
        if (wlen == 3 && wp[1] == L':') break;
        wp[--wlen] = L'\0';
    }
    if (wlen == 0) { free(wp); return 0; }
    for (size_t i = 0; i < wlen; ++i) {
        if (wp[i] == L'/' || wp[i] == L'\\') {
            if (i == 0 || (i == 2 && wp[1] == L':')) continue;
            wchar_t saved = wp[i];
            wp[i] = L'\0';
            DWORD attr = GetFileAttributesW(wp);
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (!CreateDirectoryW(wp, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                    free(wp);
                    return 0;
                }
            }
            wp[i] = saved;
        }
    }
    BOOL created = CreateDirectoryW(wp, NULL);
    DWORD attr = GetFileAttributesW(wp);
    free(wp);
    if (created) return 1;
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    char* path = tz_dup_n(p, pn);
    if (!path) return 0;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
    for (size_t i = 1; i < len; ++i) {
        if (path[i] == '/') {
            path[i] = '\0';
            if (mkdir(path, 0777) != 0 && errno != EEXIST) {
                free(path);
                return 0;
            }
            path[i] = '/';
        }
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        free(path);
        return 0;
    }
    struct stat st;
    int isDir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    free(path);
    return (uint8_t)(isDir ? 1 : 0);
#endif
}

void tz_sys_env_get(const char* p, int64_t pn, tz_text* out) {
    out->ptr = NULL;
    out->len = 0;
    if (pn <= 0 || !p) return;
    char* name = tz_dup_n(p, pn);
    if (!name) return;
#ifdef _WIN32
    char* val = NULL;
    size_t rawLen = 0;
    if (_dupenv_s(&val, &rawLen, name) == 0 && val) {
        size_t len = strlen(val);
        char* stable = len > 0 ? tz_dup_n(val, (int64_t)len) : NULL;
        if (stable) {
            out->ptr = stable;
            out->len = (int64_t)len;
        }
        free(val);
    }
#else
    const char* val = getenv(name);
    if (val) {
        size_t len = strlen(val);
        char* stable = len > 0 ? tz_dup_n(val, (int64_t)len) : NULL;
        if (stable) {
            out->ptr = stable;
            out->len = (int64_t)len;
        }
    }
#endif
    free(name);
}

int64_t tz_sys_clock_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    int64_t f = (int64_t)freq.QuadPart;
    int64_t t = (int64_t)now.QuadPart;
    if (f <= 0) return 0;
    return (t / f) * 1000000000LL + ((t % f) * 1000000000LL) / f;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

void tz_sys_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)(ms > 0xffffffffLL ? 0xffffffffLL : ms));
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000LL);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {}
#endif
}

static int64_t tz_exec_impl(const char* p, int64_t pn, tz_text* out) {
    if (out) { out->ptr = NULL; out->len = 0; }
    if (!p) return -1;
    char* cmd = tz_dup_n(p, pn);
    if (!cmd) return -1;
#ifdef _WIN32
    FILE* pipe = _popen(cmd, "rb");
#else
    FILE* pipe = popen(cmd, "r");
#endif
    free(cmd);
    if (!pipe) return -1;
    size_t cap = 4096, used = 0;
    char* buf = (char*)malloc(cap);
    char discard[4096];
    for (;;) {
        if (out && buf) {
            if (used == cap) {
                char* nb = (char*)realloc(buf, cap * 2);
                if (nb) { buf = nb; cap *= 2; }
                else { free(buf); buf = NULL; }
            }
            if (buf) {
                size_t got = fread(buf + used, 1, cap - used, pipe);
                used += got;
                if (got == 0) break;
                continue;
            }
        }
        if (fread(discard, 1, sizeof(discard), pipe) == 0) break;
    }
#ifdef _WIN32
    int code = _pclose(pipe);
#else
    int st = pclose(pipe);
    int code = st;
    if (st >= 0 && WIFEXITED(st)) code = WEXITSTATUS(st);
#endif
    if (out && buf) {
        if (used > 0) {
            out->ptr = buf;
            out->len = (int64_t)used;
        } else {
            free(buf);
        }
    } else if (buf) {
        free(buf);
    }
    return (int64_t)code;
}

void tz_sys_exec_capture(const char* p, int64_t pn, tz_text* out) {
    tz_exec_impl(p, pn, out);
}

int64_t tz_sys_exec_status(const char* p, int64_t pn) {
    return tz_exec_impl(p, pn, NULL);
}

static void tz_set_err(tz_text* errOut, const char* msg) {
    if (errOut) {
        errOut->ptr = msg;
        errOut->len = msg ? (int64_t)strlen(msg) : 0;
    }
}

void* tz_sys_file_open(const char* p, int64_t pn, const char* m, int64_t mn, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    const char* cmode = NULL;
    if (m && mn == 1) {
        if (m[0] == 'r') cmode = "rb";
        else if (m[0] == 'w') cmode = "wb";
        else if (m[0] == 'a') cmode = "ab";
    } else if (m && mn == 2 && m[1] == '+') {
        if (m[0] == 'r') cmode = "r+b";
        else if (m[0] == 'w') cmode = "w+b";
        else if (m[0] == 'a') cmode = "a+b";
    }
    if (!cmode) {
        tz_set_err(errOut, "Modo de archivo inválido");
        return NULL;
    }
    if (!p || pn <= 0) {
        tz_set_err(errOut, "No se pudo abrir el archivo");
        return NULL;
    }
    FILE* f = NULL;
#ifdef _WIN32
    wchar_t* wp = tz_to_wide(p, pn);
    if (wp) {
        wchar_t wmode[8];
        size_t mi = 0;
        for (; cmode[mi] && mi + 1 < 8; ++mi) wmode[mi] = (wchar_t)cmode[mi];
        wmode[mi] = L'\0';
        f = _wfopen(wp, wmode);
        free(wp);
    }
#else
    char* path = tz_dup_n(p, pn);
    if (path) {
        f = fopen(path, cmode);
        free(path);
    }
#endif
    if (!f) tz_set_err(errOut, "No se pudo abrir el archivo");
    return (void*)f;
}

uint8_t tz_sys_file_read(void* fp, int64_t maxBytes, tz_text* out, tz_text* errOut) {
    if (out) { out->ptr = NULL; out->len = 0; }
    tz_set_err(errOut, NULL);
    if (!fp) {
        tz_set_err(errOut, "Archivo cerrado");
        return 0;
    }
    if (maxBytes == 0) return 1;
    FILE* f = (FILE*)fp;
    size_t n = (size_t)maxBytes;
    char* buf = (char*)malloc(n);
    if (!buf) {
        tz_set_err(errOut, "Sin memoria al leer archivo");
        return 0;
    }
    size_t got = fread(buf, 1, n, f);
    if (got == 0) {
        int hadErr = ferror(f);
        free(buf);
        if (hadErr) {
            tz_set_err(errOut, "Error de lectura en archivo");
            return 0;
        }
        return 1;
    }
    if (out) {
        out->ptr = buf;
        out->len = (int64_t)got;
    } else {
        free(buf);
    }
    return 1;
}

uint8_t tz_sys_file_read_all(void* fp, tz_text* out, tz_text* errOut) {
    if (out) { out->ptr = NULL; out->len = 0; }
    tz_set_err(errOut, NULL);
    if (!fp) {
        tz_set_err(errOut, "Archivo cerrado");
        return 0;
    }
    FILE* f = (FILE*)fp;
    size_t cap = 4096;
    size_t used = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        tz_set_err(errOut, "Sin memoria al leer archivo");
        return 0;
    }
    for (;;) {
        if (used == cap) {
            if (cap >= (size_t)0x3FFFFFFF) {
                free(buf);
                tz_set_err(errOut, "Archivo demasiado grande");
                return 0;
            }
            size_t nc = cap * 2;
            if (nc > (size_t)0x3FFFFFFF) nc = (size_t)0x3FFFFFFF;
            char* nb = (char*)realloc(buf, nc);
            if (!nb) {
                free(buf);
                tz_set_err(errOut, "Sin memoria al leer archivo");
                return 0;
            }
            buf = nb;
            cap = nc;
        }
        size_t got = fread(buf + used, 1, cap - used, f);
        used += got;
        if (got < cap - used + got && got == 0) break;
    }
    if (ferror(f)) {
        free(buf);
        tz_set_err(errOut, "Error de lectura en archivo");
        return 0;
    }
    if (used == 0) {
        free(buf);
        return 1;
    }
    if (out) {
        out->ptr = buf;
        out->len = (int64_t)used;
    } else {
        free(buf);
    }
    return 1;
}

int32_t tz_sys_file_read_line(void* fp, tz_text* out, tz_text* errOut) {
    if (out) { out->ptr = NULL; out->len = 0; }
    tz_set_err(errOut, NULL);
    if (!fp) {
        tz_set_err(errOut, "Archivo cerrado");
        return -1;
    }
    FILE* f = (FILE*)fp;
    size_t cap = 128;
    size_t used = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        tz_set_err(errOut, "Sin memoria al leer línea");
        return -1;
    }
    int any = 0;
    for (;;) {
        int ch = fgetc(f);
        if (ch == EOF) {
            if (ferror(f)) {
                free(buf);
                tz_set_err(errOut, "Error de lectura en archivo");
                return -1;
            }
            break;
        }
        any = 1;
        if (ch == '\n') break;
        if (used >= (size_t)0x3FFFFFFF) {
            free(buf);
            tz_set_err(errOut, "Línea demasiado larga");
            return -1;
        }
        if (used == cap) {
            size_t nc = cap * 2;
            char* nb = (char*)realloc(buf, nc);
            if (!nb) {
                free(buf);
                tz_set_err(errOut, "Sin memoria al leer línea");
                return -1;
            }
            buf = nb;
            cap = nc;
        }
        buf[used++] = (char)ch;
    }
    if (!any) {
        free(buf);
        return 0;
    }
    if (used > 0 && buf[used - 1] == '\r') --used;
    if (used == 0) {
        free(buf);
        return 1;
    }
    if (out) {
        out->ptr = buf;
        out->len = (int64_t)used;
    } else {
        free(buf);
    }
    return 1;
}

int64_t tz_sys_file_write(void* fp, const char* data, int64_t len, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    if (!fp) {
        tz_set_err(errOut, "Archivo cerrado");
        return -1;
    }
    if (len <= 0 || !data) return 0;
    FILE* f = (FILE*)fp;
    size_t put = fwrite(data, 1, (size_t)len, f);
    if (put != (size_t)len || ferror(f)) {
        tz_set_err(errOut, "Error de escritura en archivo");
        return -1;
    }
    return (int64_t)put;
}

uint8_t tz_sys_file_flush(void* fp, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    if (!fp) {
        tz_set_err(errOut, "Archivo cerrado");
        return 0;
    }
    if (fflush((FILE*)fp) != 0) {
        tz_set_err(errOut, "Error al vaciar el archivo");
        return 0;
    }
    return 1;
}

void tz_sys_file_close(void** slot) {
    if (!slot || !*slot) return;
    FILE* f = (FILE*)*slot;
    *slot = NULL;
    fclose(f);
}

typedef struct tz_mutex_box {
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    LONG refs;
#else
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int64_t refs;
#endif
    int locked;
} tz_mutex_box;

static void tz_mutex_release(tz_mutex_box* b) {
    if (!b) return;
#ifdef _WIN32
    if (InterlockedDecrement(&b->refs) == 0) {
        DeleteCriticalSection(&b->cs);
        free(b);
    }
#else
    if (__atomic_sub_fetch(&b->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        pthread_cond_destroy(&b->cv);
        pthread_mutex_destroy(&b->mu);
        free(b);
    }
#endif
}

void* tz_sys_mutex_new(void) {
    tz_mutex_box* b = (tz_mutex_box*)calloc(1, sizeof(tz_mutex_box));
    if (!b) return NULL;
#ifdef _WIN32
    InitializeCriticalSection(&b->cs);
    InitializeConditionVariable(&b->cv);
    b->refs = 1;
#else
    if (pthread_mutex_init(&b->mu, NULL) != 0) { free(b); return NULL; }
    if (pthread_cond_init(&b->cv, NULL) != 0) { pthread_mutex_destroy(&b->mu); free(b); return NULL; }
    b->refs = 1;
#endif
    b->locked = 0;
    return (void*)b;
}

void* tz_sys_mutex_lock(void* mp) {
    tz_mutex_box* b = (tz_mutex_box*)mp;
    if (!b) return NULL;
#ifdef _WIN32
    EnterCriticalSection(&b->cs);
    while (b->locked) SleepConditionVariableCS(&b->cv, &b->cs, INFINITE);
    b->locked = 1;
    InterlockedIncrement(&b->refs);
    LeaveCriticalSection(&b->cs);
#else
    pthread_mutex_lock(&b->mu);
    while (b->locked) pthread_cond_wait(&b->cv, &b->mu);
    b->locked = 1;
    __atomic_add_fetch(&b->refs, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&b->mu);
#endif
    return (void*)b;
}

void* tz_sys_mutex_try_lock(void* mp) {
    tz_mutex_box* b = (tz_mutex_box*)mp;
    if (!b) return NULL;
    void* out = NULL;
#ifdef _WIN32
    EnterCriticalSection(&b->cs);
    if (!b->locked) {
        b->locked = 1;
        InterlockedIncrement(&b->refs);
        out = (void*)b;
    }
    LeaveCriticalSection(&b->cs);
#else
    pthread_mutex_lock(&b->mu);
    if (!b->locked) {
        b->locked = 1;
        __atomic_add_fetch(&b->refs, 1, __ATOMIC_RELAXED);
        out = (void*)b;
    }
    pthread_mutex_unlock(&b->mu);
#endif
    return out;
}

void tz_sys_lock_unlock(void** slot) {
    if (!slot || !*slot) return;
    tz_mutex_box* b = (tz_mutex_box*)*slot;
    *slot = NULL;
#ifdef _WIN32
    EnterCriticalSection(&b->cs);
    b->locked = 0;
    WakeConditionVariable(&b->cv);
    LeaveCriticalSection(&b->cs);
#else
    pthread_mutex_lock(&b->mu);
    b->locked = 0;
    pthread_cond_signal(&b->cv);
    pthread_mutex_unlock(&b->mu);
#endif
    tz_mutex_release(b);
}

void tz_sys_mutex_drop(void** slot) {
    if (!slot || !*slot) return;
    tz_mutex_box* b = (tz_mutex_box*)*slot;
    *slot = NULL;
    tz_mutex_release(b);
}

#ifdef TZ_USE_NET
typedef struct tz_sock_box {
    uintptr_t fd;
    int port;
} tz_sock_box;

#ifdef _WIN32
static INIT_ONCE tz_wsa_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK tz_wsa_init_cb(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    return TRUE;
}
#endif

static void tz_net_init(void) {
#ifdef _WIN32
    InitOnceExecuteOnce(&tz_wsa_once, tz_wsa_init_cb, NULL, NULL);
#endif
}

static char* tz_resolve_host_str(const char* host, int64_t hlen) {
    if (!host || hlen <= 0) return tz_dup_n("0.0.0.0", 7);
    if (hlen == 9 && memcmp(host, "localhost", 9) == 0) return tz_dup_n("127.0.0.1", 9);
    return tz_dup_n(host, hlen);
}

void* tz_sys_tcp_listen(const char* host, int64_t hlen, int64_t port, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    if (port < 0 || port > 65535) {
        tz_set_err(errOut, "Puerto inválido");
        return NULL;
    }
    tz_net_init();
    char* hstr = tz_resolve_host_str(host, hlen);
    if (!hstr) {
        tz_set_err(errOut, "Sin memoria");
        return NULL;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    int resolved = (inet_pton(AF_INET, hstr, &addr.sin_addr) == 1);
    if (!resolved) {
        struct addrinfo hints;
        struct addrinfo* res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(hstr, NULL, &hints, &res) == 0 && res) {
            addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
            resolved = 1;
            freeaddrinfo(res);
        }
    }
    free(hstr);
    if (!resolved) {
        tz_set_err(errOut, "No se pudo escuchar en el puerto");
        return NULL;
    }
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        tz_set_err(errOut, "No se pudo crear el socket");
        return NULL;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    if (bind(s, (struct sockaddr*)&addr, (int)sizeof(addr)) != 0 || listen(s, 64) != 0) {
        closesocket(s);
        tz_set_err(errOut, "No se pudo escuchar en el puerto");
        return NULL;
    }
    struct sockaddr_in bound;
    int blen = (int)sizeof(bound);
    int actualPort = (int)port;
    if (getsockname(s, (struct sockaddr*)&bound, &blen) == 0) actualPort = (int)ntohs(bound.sin_port);
    tz_sock_box* box = (tz_sock_box*)malloc(sizeof(tz_sock_box));
    if (!box) { closesocket(s); tz_set_err(errOut, "Sin memoria"); return NULL; }
    box->fd = (uintptr_t)s;
    box->port = actualPort;
    return (void*)box;
#else
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        tz_set_err(errOut, "No se pudo crear el socket");
        return NULL;
    }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (bind(s, (struct sockaddr*)&addr, (socklen_t)sizeof(addr)) != 0 || listen(s, 64) != 0) {
        close(s);
        tz_set_err(errOut, "No se pudo escuchar en el puerto");
        return NULL;
    }
    struct sockaddr_in bound;
    socklen_t blen = (socklen_t)sizeof(bound);
    int actualPort = (int)port;
    if (getsockname(s, (struct sockaddr*)&bound, &blen) == 0) actualPort = (int)ntohs(bound.sin_port);
    tz_sock_box* box = (tz_sock_box*)malloc(sizeof(tz_sock_box));
    if (!box) { close(s); tz_set_err(errOut, "Sin memoria"); return NULL; }
    box->fd = (uintptr_t)s;
    box->port = actualPort;
    return (void*)box;
#endif
}

int64_t tz_sys_tcp_local_port(void* lp) {
    tz_sock_box* box = (tz_sock_box*)lp;
    return box ? (int64_t)box->port : 0;
}

void* tz_sys_tcp_accept(void* lp, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    tz_sock_box* box = (tz_sock_box*)lp;
    if (!box) {
        tz_set_err(errOut, "Listener cerrado");
        return NULL;
    }
#ifdef _WIN32
    SOCKET cli = accept((SOCKET)box->fd, NULL, NULL);
    if (cli == INVALID_SOCKET) {
        tz_set_err(errOut, "Error al aceptar conexión");
        return NULL;
    }
    tz_sock_box* cbox = (tz_sock_box*)malloc(sizeof(tz_sock_box));
    if (!cbox) { closesocket(cli); tz_set_err(errOut, "Sin memoria"); return NULL; }
    cbox->fd = (uintptr_t)cli;
    cbox->port = 0;
    return (void*)cbox;
#else
    int cli = accept((int)box->fd, NULL, NULL);
    if (cli < 0) {
        tz_set_err(errOut, "Error al aceptar conexión");
        return NULL;
    }
    tz_sock_box* cbox = (tz_sock_box*)malloc(sizeof(tz_sock_box));
    if (!cbox) { close(cli); tz_set_err(errOut, "Sin memoria"); return NULL; }
    cbox->fd = (uintptr_t)cli;
    cbox->port = 0;
    return (void*)cbox;
#endif
}

void* tz_sys_tcp_connect(const char* host, int64_t hlen, int64_t port, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    if (port <= 0 || port > 65535) {
        tz_set_err(errOut, "Puerto inválido");
        return NULL;
    }
    tz_net_init();
    char* hstr = (!host || hlen <= 0) ? tz_dup_n("127.0.0.1", 9) : tz_resolve_host_str(host, hlen);
    if (!hstr) {
        tz_set_err(errOut, "Sin memoria");
        return NULL;
    }
    char pstr[16];
    snprintf(pstr, sizeof(pstr), "%lld", (long long)port);
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(hstr, pstr, &hints, &res);
    free(hstr);
    if (gai != 0 || !res) {
        tz_set_err(errOut, "No se pudo conectar");
        return NULL;
    }
#ifdef _WIN32
    SOCKET conn = INVALID_SOCKET;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        SOCKET s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) { conn = s; break; }
        closesocket(s);
    }
    freeaddrinfo(res);
    if (conn == INVALID_SOCKET) {
        tz_set_err(errOut, "No se pudo conectar");
        return NULL;
    }
    tz_sock_box* box = (tz_sock_box*)malloc(sizeof(tz_sock_box));
    if (!box) { closesocket(conn); tz_set_err(errOut, "Sin memoria"); return NULL; }
    box->fd = (uintptr_t)conn;
    box->port = 0;
    return (void*)box;
#else
    int conn = -1;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        int s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s < 0) continue;
        if (connect(s, rp->ai_addr, rp->ai_addrlen) == 0) { conn = s; break; }
        close(s);
    }
    freeaddrinfo(res);
    if (conn < 0) {
        tz_set_err(errOut, "No se pudo conectar");
        return NULL;
    }
    tz_sock_box* box = (tz_sock_box*)malloc(sizeof(tz_sock_box));
    if (!box) { close(conn); tz_set_err(errOut, "Sin memoria"); return NULL; }
    box->fd = (uintptr_t)conn;
    box->port = 0;
    return (void*)box;
#endif
}

static void tz_sock_box_close(void** slot) {
    if (!slot || !*slot) return;
    tz_sock_box* box = (tz_sock_box*)*slot;
    *slot = NULL;
#ifdef _WIN32
    closesocket((SOCKET)box->fd);
#else
    close((int)box->fd);
#endif
    free(box);
}

void tz_sys_tcp_close(void** slot) { tz_sock_box_close(slot); }
void tz_sys_sock_close(void** slot) { tz_sock_box_close(slot); }

uint8_t tz_sys_sock_read(void* sp, int64_t maxBytes, tz_text* out, tz_text* errOut) {
    if (out) { out->ptr = NULL; out->len = 0; }
    tz_set_err(errOut, NULL);
    tz_sock_box* box = (tz_sock_box*)sp;
    if (!box) {
        tz_set_err(errOut, "Socket cerrado");
        return 0;
    }
    if (maxBytes == 0) return 1;
    size_t n = (size_t)maxBytes;
    char* buf = (char*)malloc(n);
    if (!buf) {
        tz_set_err(errOut, "Sin memoria al leer socket");
        return 0;
    }
#ifdef _WIN32
    int got = recv((SOCKET)box->fd, buf, (int)n, 0);
    if (got < 0) {
        free(buf);
        tz_set_err(errOut, "Error de lectura en socket");
        return 0;
    }
    if (got == 0) {
        free(buf);
        return 1;
    }
    if (out) {
        out->ptr = buf;
        out->len = (int64_t)got;
    } else {
        free(buf);
    }
    return 1;
#else
    ssize_t got = 0;
    do {
        got = recv((int)box->fd, buf, n, 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        free(buf);
        tz_set_err(errOut, "Error de lectura en socket");
        return 0;
    }
    if (got == 0) {
        free(buf);
        return 1;
    }
    if (out) {
        out->ptr = buf;
        out->len = (int64_t)got;
    } else {
        free(buf);
    }
    return 1;
#endif
}

int64_t tz_sys_sock_write(void* sp, const char* data, int64_t len, tz_text* errOut) {
    tz_set_err(errOut, NULL);
    tz_sock_box* box = (tz_sock_box*)sp;
    if (!box) {
        tz_set_err(errOut, "Socket cerrado");
        return -1;
    }
    if (len <= 0 || !data) return 0;
    size_t sent = 0;
    size_t total = (size_t)len;
    while (sent < total) {
#ifdef _WIN32
        int n = send((SOCKET)box->fd, data + sent, (int)(total - sent), 0);
        if (n <= 0) {
            tz_set_err(errOut, "Error de escritura en socket");
            return -1;
        }
#else
        ssize_t n = send((int)box->fd, data + sent, total - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            tz_set_err(errOut, "Error de escritura en socket");
            return -1;
        }
#endif
        sent += (size_t)n;
    }
    return (int64_t)sent;
}
#endif

void* tz_sys_aligned_alloc(int64_t bytes, int64_t align) {
    size_t sz = bytes > 0 ? (size_t)bytes : 1;
    size_t al = align > 0 ? (size_t)align : 1;
#ifdef _WIN32
    return _aligned_malloc(sz, al);
#else
    if (al < sizeof(void*)) al = sizeof(void*);
    void* p = NULL;
    if (posix_memalign(&p, al, sz) != 0) return NULL;
    return p;
#endif
}

void tz_sys_aligned_free(void* p) {
    if (!p) return;
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

int64_t tz_sys_cstr_len(const char* p) {
    if (!p) return 0;
    return (int64_t)strlen(p);
}

void tz_sys_spin_hint(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
    __asm__ volatile("yield");
#endif
}

typedef struct tz_sys_region_chunk {
    uint8_t* data;
    size_t cap;
    size_t used;
    size_t align;
    struct tz_sys_region_chunk* next;
} tz_sys_region_chunk;

typedef struct tz_sys_region {
    tz_sys_region_chunk* first;
    tz_sys_region_chunk* cur;
    size_t init_cap;
    size_t total_used;
    size_t total_cap;
} tz_sys_region;

static size_t tz_region_align_up(size_t v, size_t a) {
    return a <= 1u ? v : ((v + a - 1u) & ~(a - 1u));
}

void* tz_sys_region_new(int64_t init_bytes) {
    if (init_bytes < 0 || init_bytes > (1LL << 30)) return NULL;
    tz_sys_region* r = (tz_sys_region*)calloc(1, sizeof(tz_sys_region));
    if (!r) return NULL;
    r->init_cap = (size_t)init_bytes;
    if (init_bytes > 0) {
        size_t cap = (size_t)init_bytes;
        uint8_t* data = (uint8_t*)tz_sys_aligned_alloc((int64_t)cap, 64);
        if (!data) {
            free(r);
            return NULL;
        }
        tz_sys_region_chunk* ch = (tz_sys_region_chunk*)malloc(sizeof(tz_sys_region_chunk));
        if (!ch) {
            tz_sys_aligned_free(data);
            free(r);
            return NULL;
        }
        ch->data = data;
        ch->cap = cap;
        ch->used = 0u;
        ch->align = 64u;
        ch->next = NULL;
        r->first = ch;
        r->cur = ch;
        r->total_cap = cap;
    }
    return (void*)r;
}

void* tz_sys_region_alloc(void* rp, int64_t bytes, int64_t align) {
    tz_sys_region* r = (tz_sys_region*)rp;
    if (!r || bytes < 0 || align <= 0) return NULL;
    size_t sz = (size_t)bytes;
    size_t eff_align = (size_t)align;
    if (r->cur && r->cur->align >= eff_align) {
        size_t off = tz_region_align_up(r->cur->used, eff_align);
        if (off >= r->cur->used && off <= r->cur->cap && sz <= r->cur->cap - off) {
            uint8_t* p = r->cur->data + off;
            r->total_used += (off + sz) - r->cur->used;
            r->cur->used = off + sz;
            return (void*)p;
        }
    }
    size_t chunk_align = eff_align > 64u ? eff_align : 64u;
    size_t next_cap = r->init_cap > 0u ? r->init_cap : 4096u;
    if (r->cur) {
        size_t prev = r->cur->cap;
        next_cap = prev <= (SIZE_MAX / 2u) ? prev * 2u : SIZE_MAX;
    }
    if (next_cap < 64u) next_cap = 64u;
    if (next_cap < sz) next_cap = sz;
    uint8_t* mem = (uint8_t*)tz_sys_aligned_alloc((int64_t)(next_cap > 0u ? next_cap : 1u), (int64_t)chunk_align);
    if (!mem) return NULL;
    tz_sys_region_chunk* ch = (tz_sys_region_chunk*)malloc(sizeof(tz_sys_region_chunk));
    if (!ch) {
        tz_sys_aligned_free(mem);
        return NULL;
    }
    ch->data = mem;
    ch->cap = next_cap;
    ch->used = sz;
    ch->align = chunk_align;
    ch->next = NULL;
    if (!r->first) r->first = ch;
    else r->cur->next = ch;
    r->cur = ch;
    r->total_cap += next_cap;
    r->total_used += sz;
    return (void*)ch->data;
}

void tz_sys_region_reset(void* rp) {
    tz_sys_region* r = (tz_sys_region*)rp;
    if (!r) return;
    r->total_used = 0u;
    if (r->first) {
        tz_sys_region_chunk* c = r->first->next;
        while (c) {
            tz_sys_region_chunk* nx = c->next;
            tz_sys_aligned_free(c->data);
            free(c);
            c = nx;
        }
        r->first->next = NULL;
        r->first->used = 0u;
        r->cur = r->first;
        r->total_cap = r->first->cap;
    } else {
        r->cur = NULL;
        r->total_cap = 0u;
    }
}

int64_t tz_sys_region_used(void* rp) {
    tz_sys_region* r = (tz_sys_region*)rp;
    return r ? (int64_t)r->total_used : 0;
}

int64_t tz_sys_region_cap(void* rp) {
    tz_sys_region* r = (tz_sys_region*)rp;
    return r ? (int64_t)r->total_cap : 0;
}

void tz_sys_region_drop(void** slot) {
    if (!slot || !*slot) return;
    tz_sys_region* r = (tz_sys_region*)*slot;
    *slot = NULL;
    tz_sys_region_chunk* c = r->first;
    while (c) {
        tz_sys_region_chunk* nx = c->next;
        tz_sys_aligned_free(c->data);
        free(c);
        c = nx;
    }
    free(r);
}
)c";
}

} // namespace veloz::native
