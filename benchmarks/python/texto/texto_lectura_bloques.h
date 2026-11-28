/* Mantiene en memoria un bloque y guarda solamente las claves nuevas. */
static int read_block(FILE *file, char *target, size_t capacity, size_t *got) {
#ifdef _WIN32
    DWORD size = 0;
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
    DWORD wanted = (DWORD)(capacity < 0x3fffffffu ? capacity : 0x3fffffffu);
    if (!ReadFile(handle, target, wanted, &size, NULL) && GetLastError() != ERROR_HANDLE_EOF) return 0;
#else
    ssize_t size;
    size_t wanted = capacity < 0x3fffffffu ? capacity : 0x3fffffffu;
    do { size = read(fileno(file), target, wanted); } while (size < 0 && errno == EINTR);
    if (size < 0) return 0;
#endif
    *got = (size_t)size;
    return 1;
}

static size_t through_last_line(const char *data, size_t size) {
    if (size && data[size - 1] == '\r') --size;
    while (size > 0 && data[size - 1] != '\n' && data[size - 1] != '\r') --size;
    return size;
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
    size_t capacity = TZ_TEXTO_BLOQUES, ready = 0, lines = 0, longest = 0, chars = 0;
    char *data = malloc(capacity + 8);
    char prefix[40];
    Dictionary dict = {.slots = calloc(4096, sizeof(Entry)), .cap = 4096, .allow_packed = 1};
#ifdef TZ_TEXTO_REHASH
    dict.multiplier = initial_multiplier;
#endif
    int ok = data && dict.slots;
    while (ok) {
        if (ready == capacity) {
            if (capacity > (SIZE_MAX - 8) / 2) { ok = 0; break; }
            capacity *= 2;
            char *larger = realloc(data, capacity + 8);
            if (!larger) { ok = 0; break; }
            data = larger;
        }
        size_t got;
        if (!read_block(file, data + ready, capacity - ready, &got)) { ok = 0; break; }
        ready += got;
        size_t cut = got ? through_last_line(data, ready) : ready;
        if (cut) {
            size_t consumed = cut, chunk_lines, chunk_width;
            const char *chunk_longest;
            int has_nul = count_lines(data, consumed, &chunk_lines, &chunk_width, &chunk_longest);
            if (has_nul == 2) {
                consumed = normalize_newlines(data, consumed);
                has_nul = count_lines(data, consumed, &chunk_lines, &chunk_width, &chunk_longest);
            }
            dict.pairing = !has_nul;
            if (chunk_width > longest) {
                longest = chunk_width;
                memcpy(prefix, chunk_longest, longest < sizeof(prefix) ? longest : sizeof(prefix));
            }
            lines += chunk_lines;
            chars += consumed;
            memset(data + ready, 0, 8);
            ok = tokenize(&dict, data, consumed);
#ifdef TZ_TEXTO_REHASH
            if (ok && chars == consumed) rehash_packed(&dict);
#endif
            ready -= cut;
            if (ready) memmove(data, data + cut, ready);
        }
        if (!got) break;
    }
    fclose(file);
    if (ok) {
        printf("%zu %llu %zu\n%zu ", lines, (unsigned long long)dict.words, chars, longest);
        fwrite(prefix, 1, longest < sizeof(prefix) ? longest : sizeof(prefix), stdout);
        putchar('\n');
        print_top(&dict);
    }
    if (dict.slots) for (size_t i = 0; i < dict.cap; i++) free((void *)dict.slots[i].word);
    free(dict.slots);
    free(data);
    c_extremo_fin(start);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
