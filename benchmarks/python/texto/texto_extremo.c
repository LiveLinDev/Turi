/* Referencia para el ASCII del generador. Detecta espacios y convierte
   mayúsculas con AVX-512. Las claves se guardan como vistas del búfer. */
#include "../../c_extremo_tiempo.h"
#include <immintrin.h>
#include <stdint.h>
#include <string.h>

#ifndef __AVX512BW__
#error Este banco requiere AVX-512BW
#endif

typedef struct {
    const char *word;
    size_t length;
    uint64_t hash, count;
} Entry;

typedef struct {
    Entry *slots;
    size_t cap, used;
    uint64_t words;
#ifdef TZ_TEXTO_COMPACTO
    uint64_t packed[256], packed_count[256];
    int allow_packed;
#endif
} Dictionary;

static int punctuation(unsigned char c) {
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?';
}

static uint64_t word_hash(const char *word, size_t length) {
    const uint64_t multiplier = UINT64_C(0x9e3779b185ebca87);
    uint64_t hash = length * multiplier;
    while (length >= 8) {
        uint64_t piece;
        memcpy(&piece, word, 8);
        hash = (hash ^ piece) * multiplier;
        word += 8;
        length -= 8;
    }
    uint64_t last = 0;
    memcpy(&last, word, length);
    hash = (hash ^ last) * multiplier;
    return hash ^ (hash >> 32);
}

static int grow(Dictionary *dict) {
    if (dict->cap > SIZE_MAX / (2 * sizeof(Entry))) return 0;
    size_t cap = dict->cap * 2;
    Entry *slots = calloc(cap, sizeof(*slots));
    if (!slots) return 0;
    for (size_t i = 0; i < dict->cap; i++) {
        Entry old = dict->slots[i];
        if (!old.word) continue;
        size_t at = old.hash & (cap - 1);
        while (slots[at].word) at = (at + 1) & (cap - 1);
        slots[at] = old;
    }
    free(dict->slots);
    dict->slots = slots;
    dict->cap = cap;
    return 1;
}

static int count_word(Dictionary *dict, const char *word, size_t length) {
    while (length && punctuation((unsigned char)*word)) {
        word++;
        length--;
    }
    while (length && punctuation((unsigned char)word[length - 1])) length--;
    if (!length) return 1;
    dict->words++;
#ifdef TZ_TEXTO_COMPACTO
    if (dict->allow_packed && length <= 8) {
        uint64_t bytes;
        memcpy(&bytes, word, sizeof(bytes));
        uint64_t key = bytes & (UINT64_MAX >> (8 * (8 - length)));
#ifndef TZ_TEXTO_NATIVO
        key = __builtin_bswap64(key);
#endif
        size_t slot = (key * UINT64_C(0x9e3779b185ebca87)) >> 56;
        for (unsigned probes = 0; probes < 256; probes++) {
            uint64_t found = dict->packed[slot];
            if (found == key) {
                dict->packed_count[slot]++;
                return 1;
            }
            if (!found) {
                dict->packed[slot] = key;
                dict->packed_count[slot] = 1;
                return 1;
            }
            slot = (slot + 1) & 255;
        }
    }
#endif
    uint64_t hash = word_hash(word, length);
    size_t at = hash & (dict->cap - 1);
    while (dict->slots[at].word) {
        Entry *entry = &dict->slots[at];
        if (entry->hash == hash && entry->length == length && !memcmp(entry->word, word, length)) {
            entry->count++;
            return 1;
        }
        at = (at + 1) & (dict->cap - 1);
    }
    if (dict->used >= dict->cap / 2) {
        if (!grow(dict)) return 0;
        at = hash & (dict->cap - 1);
        while (dict->slots[at].word) at = (at + 1) & (dict->cap - 1);
    }
    dict->slots[at] = (Entry){word, length, hash, 1};
    dict->used++;
    return 1;
}

#ifdef TZ_TEXTO_PAREJAS
static inline uint64_t pair_key(const char *data, size_t start, size_t stop) {
    size_t width = stop - start;
    if (!width || width > 8) return 0;
    uint64_t bytes;
    memcpy(&bytes, data + start, sizeof(bytes));
#ifdef TZ_TEXTO_CLAVE_PROVISIONAL
    unsigned shift = (unsigned)(8 * (width - 1));
    uint64_t last = (bytes >> shift) & 255u;
    /* Solo cuenta si la clave provisional aparece en la tabla de palabras ya recortadas. */
    uint64_t punctuation = (UINT64_C(0x8c00500200000000) >> (last & 63u)) & 1u;
    uint64_t trim = punctuation & (last < 64u);
    uint64_t key = bytes & (UINT64_MAX >> (8 * (8 - width)));
    return key ^ ((UINT64_C(0) - trim) & (last << shift));
#else
    if ((bytes & 255u) < 64u || ((bytes >> (8 * (width - 1))) & 255u) < 64u) return 0;
    return bytes & (UINT64_MAX >> (8 * (8 - width)));
#endif
}

static inline int count_pair(Dictionary *dict, const char *data, size_t start, size_t mid, size_t end) {
    uint64_t a = pair_key(data, start, mid), b = pair_key(data, mid + 1, end);
    if (!a || !b) return 0;
    size_t i = (a * UINT64_C(0x9e3779b185ebca87)) >> 56;
    size_t j = (b * UINT64_C(0x9e3779b185ebca87)) >> 56;
    if (dict->packed[i] != a || dict->packed[j] != b) return 0;
    dict->packed_count[i]++;
    dict->packed_count[j]++;
    dict->words += 2;
    return 1;
}
#endif

static int tokenize(Dictionary *dict, char *data, size_t size) {
    const __m512i space = _mm512_set1_epi8(' ');
    size_t start = 0;
    for (size_t i = 0; i < size; i += 64) {
        size_t remaining = size - i;
        __mmask64 valid = remaining >= 64 ? UINT64_MAX : (UINT64_C(1) << remaining) - 1;
        __m512i bytes = _mm512_maskz_loadu_epi8(valid, data + i);
        if (_mm512_movepi8_mask(bytes)) return 0;
        __mmask64 upper = _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8('A'), _MM_CMPINT_GE) &
                          _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8('Z'), _MM_CMPINT_LE);
        __m512i lower = _mm512_mask_add_epi8(bytes, upper, bytes, _mm512_set1_epi8(32));
        _mm512_mask_storeu_epi8(data + i, valid, lower);
        uint64_t separators = (_mm512_cmpeq_epi8_mask(bytes, space)
            | (_mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8(9), _MM_CMPINT_GE)
               & _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8(13), _MM_CMPINT_LE))
            | (_mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8(28), _MM_CMPINT_GE)
               & _mm512_cmp_epu8_mask(bytes, _mm512_set1_epi8(31), _MM_CMPINT_LE))) & valid;
        while (separators) {
            size_t end = i + (unsigned)__builtin_ctzll(separators);
#ifdef TZ_TEXTO_PAREJAS
            uint64_t rest = separators & (separators - 1);
            if (rest && dict->allow_packed) {
                size_t after = i + (unsigned)__builtin_ctzll(rest);
                if (count_pair(dict, data, start, end, after)) {
                    start = after + 1;
                    separators = rest & (rest - 1);
                    continue;
                }
            }
#endif
            if (!count_word(dict, data + start, end - start)) return 0;
            start = end + 1;
            separators &= separators - 1;
        }
    }
    return count_word(dict, data + start, size - start);
}

static size_t normalize_newlines(char *data, size_t size) {
    if (!memchr(data, '\r', size)) return size;
    size_t written = 0;
    for (size_t i = 0; i < size; i++) {
        char c = data[i];
        if (c == '\r') {
            c = '\n';
            if (i + 1 < size && data[i + 1] == '\n') i++;
        }
        data[written++] = c;
    }
    return written;
}

#ifdef TZ_TEXTO_FILAS_SIMD
static int count_lines(const char *data, size_t size, size_t *lines, size_t *longest, const char **longest_at) {
    const __m512i newline = _mm512_set1_epi8('\n');
    uint64_t zero_bytes = 0;
    size_t at = 0, begin = 0, count = 0, width = 0, offset = 0;
    for (; at + 64 <= size; at += 64) {
        __m512i bytes = _mm512_loadu_si512(data + at);
        uint64_t breaks = _mm512_cmpeq_epi8_mask(bytes, newline);
#ifdef TZ_TEXTO_FILAS_NUL
        zero_bytes |= _mm512_cmpeq_epi8_mask(bytes, _mm512_setzero_si512());
#endif
        while (breaks) {
            size_t stop = at + (unsigned)__builtin_ctzll(breaks);
            if (stop - begin > width) {
                width = stop - begin;
                offset = begin;
            }
            ++count;
            begin = stop + 1;
            breaks &= breaks - 1;
        }
    }
    for (; at < size; ++at) {
#ifdef TZ_TEXTO_FILAS_NUL
        zero_bytes |= data[at] == 0;
#endif
        if (data[at] == '\n') {
            if (at - begin > width) {
                width = at - begin;
                offset = begin;
            }
            ++count;
            begin = at + 1;
        }
    }
    if (begin < size) {
        if (size - begin > width) {
            width = size - begin;
            offset = begin;
        }
        ++count;
    }
    *lines = count;
    *longest = width;
    *longest_at = data + offset;
    return zero_bytes != 0;
}
#endif

static int precedes(const Entry *a, const Entry *b) {
    if (a->count != b->count) return a->count > b->count;
    size_t common = a->length < b->length ? a->length : b->length;
    int prefix = memcmp(a->word, b->word, common);
    return prefix ? prefix < 0 : a->length < b->length;
}

static void print_top(const Dictionary *dict) {
    const Entry *top[10];
    size_t count = 0;
    size_t entries = dict->cap;
#ifdef TZ_TEXTO_COMPACTO
    char words[256][8];
    Entry packed[256];
    for (size_t i = 0; i < 256; i++) {
        uint64_t key = dict->packed[i];
        size_t length = 0;
        while (key) {
#ifdef TZ_TEXTO_NATIVO
            words[i][length++] = (char)(key & 255);
            key >>= 8;
#else
            words[i][length++] = (char)(key >> 56);
            key <<= 8;
#endif
        }
        packed[i] = (Entry){length ? words[i] : NULL, length, 0, dict->packed_count[i]};
    }
    entries += 256;
#endif
    for (size_t i = 0; i < entries; i++) {
#ifdef TZ_TEXTO_COMPACTO
        const Entry *entry = i < dict->cap ? &dict->slots[i] : &packed[i - dict->cap];
#else
        const Entry *entry = &dict->slots[i];
#endif
        if (!entry->word) continue;
        size_t at = count;
        while (at && precedes(entry, top[at - 1])) at--;
        if (at == 10) continue;
        if (count < 10) count++;
        for (size_t end = count - 1; end > at; end--) top[end] = top[end - 1];
        top[at] = entry;
    }
    for (size_t i = 0; i < count; i++) {
        const Entry *entry = top[i];
        if (entry->length < 12) printf("%*s", (int)(12 - entry->length), "");
        fwrite(entry->word, 1, entry->length, stdout);
        printf(" %llu\n", (unsigned long long)entry->count);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return EXIT_FAILURE;
    double start = c_extremo_inicio();
    FILE *file;
#ifdef _WIN32
    if (fopen_s(&file, argv[1], "rb") != 0) return EXIT_FAILURE;
#else
    file = fopen(argv[1], "rb");
    if (!file) return EXIT_FAILURE;
#endif
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return EXIT_FAILURE; }
    long length = ftell(file);
    rewind(file);
    if (length < 0) { fclose(file); return EXIT_FAILURE; }
#ifdef TZ_TEXTO_COMPACTO
    const size_t padding = 8;
#else
    const size_t padding = 1;
#endif
    char *data = malloc((size_t)length + padding);
    if (!data) { fclose(file); return EXIT_FAILURE; }
    size_t size = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (size != (size_t)length) { free(data); return EXIT_FAILURE; }
    size = normalize_newlines(data, size);
    size_t lines = 0, longest = 0;
    const char *longest_at = data;
#ifdef TZ_TEXTO_FILAS_SIMD
    int has_nul = count_lines(data, size, &lines, &longest, &longest_at);
#else
    for (size_t at = 0; at < size; ) {
        const char *end = memchr(data + at, '\n', size - at);
        size_t content = end ? (size_t)(end - (data + at)) : size - at;
        if (content > longest) {
            longest = content;
            longest_at = data + at;
        }
        lines++;
        at += content + (end != NULL);
    }
#endif
    char prefix[40];
    size_t prefix_length = longest < sizeof(prefix) ? longest : sizeof(prefix);
    memcpy(prefix, longest_at, prefix_length);
    Dictionary dict = {.slots = calloc(4096, sizeof(Entry)), .cap = 4096};
#ifdef TZ_TEXTO_COMPACTO
    memset(data + size, 0, padding);
#ifdef TZ_TEXTO_FILAS_NUL
    dict.allow_packed = !has_nul;
#else
    dict.allow_packed = memchr(data, 0, size) == NULL;
#endif
#endif
    int ok = dict.slots && tokenize(&dict, data, size);
    if (ok) {
        printf("%zu %llu %zu\n%zu ", lines, (unsigned long long)dict.words, size, longest);
        fwrite(prefix, 1, prefix_length, stdout);
        putchar('\n');
        print_top(&dict);
    }
    free(dict.slots);
    free(data);
    c_extremo_fin(start);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
