#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <wchar.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "configuracion.h"

#define TZ_EXPORT
typedef long long tz_i64;
typedef struct { const char* ptr; tz_i64 size; } TuriArgument;
extern void tz_sys_arg_at(tz_i64 index, TuriArgument* argument);
extern void tze_program(void);

#if TZA_FILES
#define TURIPY_STANDALONE
#include "archivos.c"
#endif

static tz_i64 process_argc = 1;
static char inline_format[512];
static char* format_buffer = inline_format;
static size_t format_capacity = sizeof(inline_format);
static unsigned char output_buffer[8192];
static size_t output_used;
static FILE* output_file;

enum { OUTPUT_EMPTY = -1, OUTPUT_FRAMED = -2 };
static int output_stream = OUTPUT_EMPTY;

typedef struct { size_t size; int error; } OutputPart;

static _Noreturn void fallback(tz_i64 argc);

_Noreturn void turi_embed_abort(void) {
    fallback(process_argc);
}

/* La rama nativa solo tiene efectos de salida. Se confirma al terminar.
   Para salidas grandes se usa un archivo temporal con el mismo orden de flujos. */
void turi_embed_write(const char* text, tz_i64 length, _Bool error) {
    if (length <= 0) return;
    OutputPart part = { (size_t)length, error };
    size_t room = sizeof(output_buffer) - output_used;
    if (!output_file && room >= part.size && (output_stream == OUTPUT_EMPTY || output_stream == error)) {
        output_stream = error;
        memcpy(output_buffer + output_used, text, part.size);
        output_used += part.size;
        return;
    }
    if (!output_file && output_stream >= 0 && output_stream != error && room >= sizeof(part)) {
        OutputPart prefix = { output_used, output_stream };
        memmove(output_buffer + sizeof(prefix), output_buffer, output_used);
        memcpy(output_buffer, &prefix, sizeof(prefix));
        output_used += sizeof(prefix);
        room -= sizeof(prefix);
        output_stream = OUTPUT_FRAMED;
    }
    if (!output_file && output_stream == OUTPUT_FRAMED && room >= sizeof(part) && room - sizeof(part) >= part.size) {
        memcpy(output_buffer + output_used, &part, sizeof(part));
        output_used += sizeof(part);
        memcpy(output_buffer + output_used, text, part.size);
        output_used += part.size;
        return;
    }
    if (!output_file) {
        output_file = tmpfile();
        if (!output_file) turi_embed_abort();
        if (output_used && output_stream != OUTPUT_FRAMED) {
            OutputPart prefix = { output_used, output_stream };
            if (fwrite(&prefix, sizeof(prefix), 1, output_file) != 1) turi_embed_abort();
        }
        if (fwrite(output_buffer, 1, output_used, output_file) != output_used) turi_embed_abort();
        output_used = 0;
    }
    if (fwrite(&part, sizeof(part), 1, output_file) != 1 || fwrite(text, 1, part.size, output_file) != part.size)
        turi_embed_abort();
}

static int commit_output(void) {
#ifdef _WIN32
    /* El runtime de Turi emite LF en binario. CPython traduce LF a CRLF
       en sus flujos de texto de Windows, también cuando se redirigen. */
    int out = _fileno(stdout), error = _fileno(stderr);
    if ((out >= 0 && _setmode(out, _O_TEXT) < 0) || (error >= 0 && _setmode(error, _O_TEXT) < 0)) return -1;
#endif
    if (!output_file) {
        if (output_stream != OUTPUT_FRAMED) {
            FILE* stream = output_stream == 1 ? stderr : stdout;
            if (output_used && fwrite(output_buffer, 1, output_used, stream) != output_used) return -1;
        } else {
            for (size_t at = 0; at < output_used;) {
                OutputPart part;
                memcpy(&part, output_buffer + at, sizeof(part));
                at += sizeof(part);
                if (fwrite(output_buffer + at, 1, part.size, part.error ? stderr : stdout) != part.size) return -1;
                at += part.size;
            }
        }
    } else {
        if (fflush(output_file) || fseek(output_file, 0, SEEK_SET)) return -1;
        OutputPart part;
        while (fread(&part, sizeof(part), 1, output_file) == 1) {
            FILE* stream = part.error ? stderr : stdout;
            while (part.size) {
                size_t chunk = part.size < sizeof(output_buffer) ? part.size : sizeof(output_buffer);
                if (fread(output_buffer, 1, chunk, output_file) != chunk ||
                    fwrite(output_buffer, 1, chunk, stream) != chunk) return -1;
                part.size -= chunk;
            }
        }
        if (ferror(output_file)) return -1;
    }
    if (fflush(stdout) || fflush(stderr)) return -1;
    return 0;
}

static void release_output(void) {
#if TZA_FILES
    tzx_file_reset();
#endif
    if (format_buffer != inline_format) free(format_buffer);
    if (output_file) fclose(output_file);
}

static char* tz_scratch(tz_i64 length) {
    size_t capacity = length < 512 ? 512 : (size_t)length;
    if (capacity > format_capacity) {
        int inline_used = format_buffer == inline_format;
        char* grown = inline_used ? malloc(capacity) : realloc(format_buffer, capacity);
        if (!grown) turi_embed_abort();
        if (inline_used) memcpy(grown, inline_format, sizeof(inline_format));
        format_buffer = grown;
        format_capacity = capacity;
    }
    return format_buffer;
}

static int ascii_arguments(tz_i64 argc) {
    for (const unsigned char* p = (const unsigned char*)tza_path; *p; ++p)
        if (*p < 32 || *p > 126) return 0;
#ifdef _WIN32
    /* argv[0] aplica reglas propias de comillas. Su contenido no se pasa a Python. */
    const wchar_t* arguments = GetCommandLineW();
    int quoted = 0, wide_name = 0;
    for (; *arguments; ++arguments) {
        if (*arguments == L'"') quoted = !quoted;
        else if (!quoted && (*arguments == L' ' || *arguments == L'\t')) break;
        wide_name |= *arguments > 127;
    }
    /* La conversión de un nombre Unicode tampoco debe cambiar el límite de argv[0]. */
    if (wide_name) {
        const unsigned char* narrow = (const unsigned char*)GetCommandLineA();
        quoted = 0;
        for (; *narrow; ++narrow) {
            if (*narrow == '"') quoted = !quoted;
            else if (!quoted && (*narrow == ' ' || *narrow == '\t')) break;
        }
        const wchar_t* p = arguments;
        for (; *p && *narrow; ++p, ++narrow)
            if (*p != *narrow) return 0;
        if (*p || *narrow) return 0;
    }
    for (const wchar_t* p = arguments; *p; ++p)
        if (*p > 127) return 0;
#endif
    for (tz_i64 i = 1; i < argc; ++i) {
        TuriArgument arg;
        tz_sys_arg_at(i, &arg);
        for (tz_i64 j = 0; j < arg.size; ++j) {
            const unsigned char c = (unsigned char)arg.ptr[j];
            if ((c < 32 && c != '\t' && c != '\r' && c != '\n') || c > 126) return 0;
        }
    }
    return 1;
}

static double calculation_clock(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&count)) return 0;
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec count;
    if (clock_gettime(CLOCK_MONOTONIC, &count)) return 0;
    return (double)count.tv_sec + (double)count.tv_nsec * 1e-9;
#endif
}

static int time_enabled(void) {
#ifdef _WIN32
    return GetEnvironmentVariableA("TURIPY_EXE_TIME", NULL, 0) != 0;
#else
    return getenv("TURIPY_EXE_TIME") != NULL;
#endif
}

#ifdef _WIN32
static wchar_t* wide(const char* text) {
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (size <= 0) return NULL;
    wchar_t* converted = malloc((size_t)size * sizeof(wchar_t));
    if (!converted) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, converted, size)) {
        free(converted);
        return NULL;
    }
    return converted;
}

/* La CRT requiere duplicar barras antes de una comilla y al cerrar un argumento. */
static wchar_t* quote_argument(wchar_t* out, const wchar_t* argument) {
    *out++ = L'"';
    while (*argument) {
        size_t slashes = 0;
        while (*argument == L'\\') { ++slashes; ++argument; }
        size_t count = (*argument == L'"' || !*argument) ? 2 * slashes : slashes;
        for (size_t i = 0; i < count; ++i) *out++ = L'\\';
        if (*argument == L'"') *out++ = L'\\';
        if (*argument) *out++ = *argument++;
    }
    *out++ = L'"';
    return out;
}

static wchar_t* replay_command(const wchar_t* python, const wchar_t* loader,
                              const wchar_t* file_handle, const wchar_t* path, wchar_t** args, int count) {
    const wchar_t* prefix[] = {python, L"-c", loader, file_handle, path};
    size_t maximum = 1;
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); ++i) maximum += 2 * wcslen(prefix[i]) + 3;
    for (int i = 1; i < count; ++i) maximum += 2 * wcslen(args[i]) + 3;
    wchar_t* command = malloc(maximum * sizeof(wchar_t));
    if (!command) return NULL;
    wchar_t* out = command;
    for (size_t i = 0; i < sizeof(prefix) / sizeof(prefix[0]); ++i) {
        if (i) *out++ = L' ';
        out = quote_argument(out, prefix[i]);
    }
    for (int i = 1; i < count; ++i) {
        *out++ = L' ';
        out = quote_argument(out, args[i]);
    }
    *out = L'\0';
    return command;
}

static int child_status(const wchar_t* python, wchar_t* command) {
    HANDLE inputs[] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    HANDLE handles[3] = {NULL, NULL, NULL};
    int status = -1;
    for (int i = 0; i < 3; ++i) {
        if (!inputs[i] || inputs[i] == INVALID_HANDLE_VALUE) continue;
        if (!DuplicateHandle(GetCurrentProcess(), inputs[i], GetCurrentProcess(), &handles[i], 0,
                             TRUE, DUPLICATE_SAME_ACCESS)) goto finish;
    }
    STARTUPINFOW setup = {0};
    setup.cb = sizeof(setup);
    setup.dwFlags = STARTF_USESTDHANDLES;
    setup.hStdInput = handles[0];
    setup.hStdOutput = handles[1];
    setup.hStdError = handles[2];
    PROCESS_INFORMATION child = {0};
    if (CreateProcessW(python, command, NULL, NULL, TRUE, 0, NULL, NULL, &setup, &child)) {
        DWORD code;
        if (WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code))
            status = (int)code;
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    }
finish:
    for (int i = 0; i < 3; ++i) if (handles[i]) CloseHandle(handles[i]);
    return status;
}

static int replay(tz_i64 argc, FILE* source) {
    (void)argc;
    typedef wchar_t** (WINAPI *SplitCommandLine)(const wchar_t*, int*);
    HMODULE library = LoadLibraryExW(L"shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    SplitCommandLine split = library ? (SplitCommandLine)GetProcAddress(library, "CommandLineToArgvW") : NULL;
    int count = 0;
    wchar_t** args = split ? split(GetCommandLineW(), &count) : NULL;
    wchar_t* python = wide(tza_python);
    wchar_t* loader = wide(tza_loader);
    wchar_t* path = wide(tza_path);
    HANDLE file_handle = NULL;
    wchar_t* command = NULL;
    if (args && python && loader && path &&
        DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(_fileno(source)), GetCurrentProcess(),
                        &file_handle, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        wchar_t handle_value[32];
        swprintf(handle_value, sizeof(handle_value) / sizeof(handle_value[0]), L"%llu",
                 (unsigned long long)(uintptr_t)file_handle);
        command = replay_command(python, loader, handle_value, path, args, count);
    }
    int status = command ? child_status(python, command) : -1;
    if (file_handle) CloseHandle(file_handle);
    free(command);
    free(path);
    free(loader);
    free(python);
    if (args) LocalFree(args);
    if (library) FreeLibrary(library);
    return status;
}
#else
static int replay(tz_i64 argc, FILE* source) {
    int fd = fileno(source);
    if (fd < 0 || fcntl(fd, F_SETFD, 0) < 0) return -1;
    char fd_value[32];
    snprintf(fd_value, sizeof(fd_value), "%d", fd);
    char** args = calloc((size_t)argc + 5, sizeof(char*));
    if (!args) return -1;
    args[0] = (char*)tza_python;
    args[1] = "-c";
    args[2] = (char*)tza_loader;
    args[3] = fd_value;
    args[4] = (char*)tza_path;
    for (tz_i64 i = 1; i < argc; ++i) {
        TuriArgument arg;
        tz_sys_arg_at(i, &arg);
        args[i + 4] = (char*)arg.ptr;
    }
    execv(tza_python, args);
    free(args);
    return -1;
}
#endif

static _Noreturn void fallback(tz_i64 argc) {
    release_output();
    FILE* source = tmpfile();
    int status = -1;
    if (source) {
        size_t length = sizeof(tza_source) - 1;
        if (fwrite(tza_source, 1, length, source) == length && !fflush(source) && !fseek(source, 0, SEEK_SET))
            status = replay(argc, source);
        fclose(source);
    }
    if (status < 0 || status == 127) {
        fprintf(stderr, "turi: %s requiere CPython para recuperar la ejecución\n", tza_path);
        exit(1);
    }
    exit(status);
}

void tz_run_executable(tz_i64 argc) {
    process_argc = argc;
    const char* encoding = getenv("PYTHONIOENCODING");
    if (!ascii_arguments(argc) || (encoding && *encoding && strcmp(encoding, "utf-8") && strcmp(encoding, "ascii")))
        fallback(argc);
    int measure = time_enabled();
    double start = measure ? calculation_clock() : 0;
    tze_program();
    double elapsed = measure ? calculation_clock() - start : 0;
    int status = commit_output();
    release_output();
    if (status) {
        fprintf(stderr, "turi: error de escritura de %s\n", tza_path);
        exit(1);
    }
    if (measure) fprintf(stderr, "TURI_EXE_MS=%.6f\n", elapsed * 1000);
}

#include "formato.c"
