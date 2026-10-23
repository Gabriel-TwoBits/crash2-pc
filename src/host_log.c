#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_log.h"

#define HEADLESS_VARIABLE "CRASH2_HEADLESS"
#define MESSAGE_BYTES 1024
/* The functions whose callers' leftovers are noted, each once */
#define CALLER_LEFT_MAX 16
#define BOX_TITLE "Crash Bandicoot 2"

/* Set while a fault's handler runs (low_ram.c), where SDL's message box could wait on a lock the fault holds */
static volatile int s_InHandler;

void Host_InHandler(int inside) {
    s_InHandler = inside;
}

void Host_CallerLeft(const char *function) {
    static const char *s_Noted[CALLER_LEFT_MAX];
    static int s_NotedCount;
    int index;

    for (index = 0; index < s_NotedCount; index++) {
        if (strcmp(s_Noted[index], function) == 0) {
            return;
        } else {
            continue;
        }
    }
    if (s_NotedCount < CALLER_LEFT_MAX) {
        s_Noted[s_NotedCount++] = function;
    } else {
        /* Noted again past the table's room: the log says it each time */
    }
    Host_Log("%s reads what its callers left, which the port's C has no value for", function);
}

int Host_Headless(void) {
    return getenv(HEADLESS_VARIABLE) != NULL;
}

void Host_Log(const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    fputs("crash2-pc: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

/* The error on stderr, and in a message box for the player unless headless or in a fault's handler */
void Host_Fatal(const char *format, ...) {
    va_list arguments;
    char message[MESSAGE_BYTES];

    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    fprintf(stderr, "crash2-pc: fatal: %s\n", message);
    if (!s_InHandler && !Host_Headless()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, BOX_TITLE, message, NULL);
    } else {
        /* stderr only */
    }
    /* A core dump keeps where it stopped, which gdb reads back (the low RAM's traps keep it from a live run) */
    abort();
}
