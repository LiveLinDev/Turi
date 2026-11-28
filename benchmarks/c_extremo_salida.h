#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char *c_extremo_insertar_entero(char *end, int64_t value) {
    static const char pairs[] =
        "0001020304050607080910111213141516171819"
        "2021222324252627282930313233343536373839"
        "4041424344454647484950515253545556575859"
        "6061626364656667686970717273747576777879"
        "8081828384858687888990919293949596979899";
    uint64_t digits = value < 0 ? UINT64_C(0) - (uint64_t)value : (uint64_t)value;
    while (digits >= 100) {
        unsigned pair = (unsigned)(digits % 100);
        digits /= 100;
        end -= 2;
        memcpy(end, pairs + 2 * pair, 2);
    }
    if (digits < 10) *--end = (char)('0' + digits);
    else {
        end -= 2;
        memcpy(end, pairs + 2 * digits, 2);
    }
    if (value < 0) *--end = '-';
    return end;
}

static void c_extremo_escribir_pareja(int64_t first, int64_t second) {
    char line[48];
    char *end = line + sizeof(line);
    end[-1] = '\n';
    char *begin = c_extremo_insertar_entero(end - 1, second);
    *--begin = ' ';
    begin = c_extremo_insertar_entero(begin, first);
    fwrite(begin, 1, (size_t)(end - begin), stdout);
}
