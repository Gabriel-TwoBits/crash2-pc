#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_log.h"

void Host_Log(const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    fputs("crash2-pc: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

void Host_Fatal(const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    fputs("crash2-pc: fatal: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
    exit(EXIT_FAILURE);
}
