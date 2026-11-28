/* Lectura especulativa de un archivo regular. La llamada anfitriona limpia el
   estado también si el código de Turi se detiene. */

#include <fcntl.h>
#include <sys/stat.h>
#if defined(__AVX2__) || defined(__AVX512BW__)
#include <immintrin.h>
#endif
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

enum { TZ_TEXT_BLOCK = 64 * 1024 };

int tz_file_without_audit(void);

typedef struct tz_text_file {
    struct tz_text_file* next;
    FILE* stream;
    size_t count, used, length;
    char data[TZ_TEXT_BLOCK + 8];
} tz_text_file;

static char* tz_file_path;
static tz_text_file* tz_files;

#ifndef TURIPY_STANDALONE
static PyObject *tz_file_globals, *tz_file_builtins, *tz_builtin_open;

static PyObject* tzm_files(PyObject* self, PyObject* args) {
    (void)self;
    PyObject *globals, *builtins, *original;
    if (!PyArg_ParseTuple(args, "O!O!O", &PyDict_Type, &globals, &PyDict_Type, &builtins, &original))
        return NULL;
    Py_XSETREF(tz_file_globals, Py_NewRef(globals));
    Py_XSETREF(tz_file_builtins, Py_NewRef(builtins));
    Py_XSETREF(tz_builtin_open, Py_NewRef(original));
    Py_RETURN_NONE;
}

static int tz_file_env_ok(void) {
    if (!tz_file_without_audit()) return 0;
    PyObject* current = PyDict_GetItemString(tz_file_globals, "open");
    if (!current) current = PyDict_GetItemString(tz_file_builtins, "open");
    return current == tz_builtin_open;
}
#else
static int tz_file_env_ok(void) { return 1; }
#endif

tz_i64 tzx_file_close(tz_i64 handle) {
    tz_text_file* reader = (tz_text_file*)handle;
    tz_text_file** link = &tz_files;
    while (*link != reader) link = &(*link)->next;
    *link = reader->next;
    int result = fclose(reader->stream);
    free(reader);
    return result == 0;
}

void tzx_file_reset(void) {
    while (tz_files) tzx_file_close((tz_i64)tz_files);
    free(tz_file_path);
    tz_file_path = NULL;
}

tz_i64 tzx_file_path(tz_i64 length) {
    free(tz_file_path);
    tz_file_path = NULL;
    if (length <= 0 || length > 0x7ffffffe) return 0;
    tz_file_path = malloc((size_t)length + 1);
    if (!tz_file_path) return 0;
    tz_file_path[length] = 0;
    return (tz_i64)tz_file_path;
}

static FILE* tz_regular_file(const char* path, size_t length) {
#ifdef _WIN32
    wchar_t* wide = malloc((length + 1) * sizeof(wchar_t));
    if (!wide) return NULL;
    for (size_t i = 0; i <= length; i++) wide[i] = (unsigned char)path[i];
    int fd = _wopen(wide, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    free(wide);
    if (fd < 0) return NULL;
    struct __stat64 status;
    if (_fstat64(fd, &status) != 0 || (status.st_mode & _S_IFMT) != _S_IFREG) {
        _close(fd);
        return NULL;
    }
    FILE* file = _fdopen(fd, "rb");
    if (!file) _close(fd);
#else
    (void)length;
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    struct stat status;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        close(fd);
        return NULL;
    }
    FILE* file = fdopen(fd, "rb");
    if (!file) close(fd);
#endif
    return file;
}

tz_i64 tzx_file_open(tz_i64 path_length) {
    if (!tz_file_path || !tz_file_env_ok() || memchr(tz_file_path, 0, (size_t)path_length))
        return 0;
    FILE* file = tz_regular_file(tz_file_path, (size_t)path_length);
    free(tz_file_path);
    tz_file_path = NULL;
    if (!file) return 0;
    tz_text_file* reader = malloc(sizeof(*reader));
    if (!reader) {
        fclose(file);
        return 0;
    }
    reader->stream = file;
    reader->count = reader->used = reader->length = 0;
    reader->next = tz_files;
    tz_files = reader;
    return (tz_i64)reader;
}

static tz_i64 tz_file_chunk(tz_i64 handle, int checked) {
    tz_text_file* reader = (tz_text_file*)handle;
    char* data = reader->data;
    size_t remaining = reader->count - reader->used;
    if (remaining) memmove(data, data + reader->used, remaining);
    reader->count = remaining + fread(data + remaining, 1, TZ_TEXT_BLOCK - remaining, reader->stream);
    if (ferror(reader->stream)) return 0;
    memset(data + reader->count, 0, 8);
    size_t length = reader->count;
    if (!feof(reader->stream)) {
        /* Mantén juntas las líneas y el par CRLF que cruce el final del bloque. */
        if (length && data[length - 1] == '\r') length--;
        while (length && data[length - 1] != '\n' && data[length - 1] != '\r') length--;
        if (!length) return 0;
    }
    reader->used = length;
    if (!memchr(data, '\r', length)) {
        unsigned invalid = 0;
        for (size_t i = 0; checked && i < length; i++) {
            unsigned c = (unsigned char)data[i];
            invalid |= (c >= 127) | ((c < 32) & (c != '\n') & (c != '\t'));
        }
        if (invalid) return 0;
        reader->length = length;
        return (tz_i64)data;
    }
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\r') {
            if (i + 1 < length && data[i + 1] == '\n') i++;
            c = '\n';
        } else if (checked && !(c == '\n' || c == '\t' || (c >= 32 && c < 127))) {
            return 0;
        }
        data[out++] = (char)c;
    }
    reader->length = out;
    return (tz_i64)data;
}

tz_i64 tzx_file_chunk(tz_i64 handle) { return tz_file_chunk(handle, 1); }

/* El informe valida el bloque al calcular los límites de sus líneas. */
tz_i64 tzx_text_file_chunk(tz_i64 handle) { return tz_file_chunk(handle, 0); }

tz_i64 tzx_file_len(tz_i64 handle) { return (tz_i64)((tz_text_file*)handle)->length; }

static void tz_text_line(uint64_t stop, uint64_t *begin, uint64_t *lines, uint64_t *longest, uint64_t *offset) {
    uint64_t length = stop - *begin;
    if (length > *longest) {
        *longest = length;
        *offset = *begin;
    }
    *begin = stop + 1;
    ++*lines;
}

/* Tres campos de 17 bits cubren los recuentos y las posiciones de 64 KiB. */
tz_i64 tzx_text_prepare(tz_i64 address, tz_i64 length, tz_i64 copy, tz_i64 separators) {
    const unsigned char *text = (const unsigned char *)(uintptr_t)address;
    unsigned char *lower = (unsigned char *)(uintptr_t)copy;
    uint64_t *flags = (uint64_t *)(uintptr_t)separators;
    uint64_t lines = 0, begin = 0, longest = 0, offset = 0;
    uint64_t at = 0, end = (uint64_t)length;
#ifdef __AVX512BW__
    const __m512i newline = _mm512_set1_epi8('\n');
    const __m512i tab = _mm512_set1_epi8('\t'), space = _mm512_set1_epi8(' ');
    for (; at + 64 <= end; at += 64) {
        __m512i bytes = _mm512_loadu_si512(text + at);
        uint64_t breaks = _mm512_cmpeq_epi8_mask(bytes, newline);
        uint64_t tabs = _mm512_cmpeq_epi8_mask(bytes, tab);
        uint64_t control = _mm512_cmp_epu8_mask(bytes, space, _MM_CMPINT_LT);
        uint64_t outside = _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8(127), _MM_CMPINT_GE);
        if (outside || (control & ~(breaks | tabs))) return -1;
        flags[at / 64] = breaks | tabs | _mm512_cmpeq_epi8_mask(bytes, space);
        __mmask64 capitals = _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8('A'), _MM_CMPINT_GE) &
                             _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8('Z'), _MM_CMPINT_LE);
        _mm512_storeu_si512(lower + at, _mm512_mask_add_epi8(bytes, capitals, bytes, _mm512_set1_epi8('a' - 'A')));
        while (breaks) {
            uint64_t stop = at + (unsigned)__builtin_ctzll(breaks);
            tz_text_line(stop, &begin, &lines, &longest, &offset);
            breaks &= breaks - 1;
        }
    }
#elif defined(__AVX2__)
    for (; at + 32 <= end; at += 32) {
        __m256i bytes = _mm256_loadu_si256((const __m256i *)(text + at));
        __m256i newlines = _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\n'));
        __m256i tabs = _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\t'));
        __m256i spaces = _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(' '));
        __m256i control = _mm256_cmpgt_epi8(_mm256_set1_epi8(32), bytes);
        __m256i invalid = _mm256_or_si256(_mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(127)),
                                         _mm256_andnot_si256(_mm256_or_si256(newlines, tabs), control));
        if (_mm256_movemask_epi8(invalid)) return -1;
        uint32_t breaks = (uint32_t)_mm256_movemask_epi8(newlines);
        uint64_t marks = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(newlines, _mm256_or_si256(spaces, tabs)));
        if (at & 32) flags[at / 64] |= marks << 32;
        else flags[at / 64] = marks;
        __m256i capitals = _mm256_and_si256(_mm256_cmpgt_epi8(bytes, _mm256_set1_epi8('A' - 1)),
                                            _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), bytes));
        __m256i folded = _mm256_or_si256(bytes, _mm256_and_si256(capitals, _mm256_set1_epi8('a' - 'A')));
        _mm256_storeu_si256((__m256i *)(lower + at), folded);
        while (breaks) {
            uint64_t stop = at + (unsigned)__builtin_ctzll(breaks);
            tz_text_line(stop, &begin, &lines, &longest, &offset);
            breaks &= breaks - 1;
        }
    }
#endif
    for (; at < end; ++at) {
        if ((at & 63) == 0) flags[at / 64] = 0;
        unsigned c = text[at];
        if (c >= 127 || (c < 32 && c != '\n' && c != '\t')) return -1;
        if (c == '\n') tz_text_line(at, &begin, &lines, &longest, &offset);
        if (c == ' ' || c == '\n' || c == '\t') flags[at / 64] |= UINT64_C(1) << (at & 63);
        lower[at] = (unsigned char)(c + (c >= 'A' && c <= 'Z' ? 'a' - 'A' : 0));
    }
    if (begin < end) tz_text_line(end, &begin, &lines, &longest, &offset);
    memset(lower + end, 0, 8);
    return (tz_i64)(lines | (longest << 17) | (offset << 34));
}
