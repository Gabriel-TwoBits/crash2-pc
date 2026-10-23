#include <stdio.h>
#include <stdlib.h>

#include "host64.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

w2c_crash2 g_Module;

#ifdef _WIN32
/* A copy of a UTF-16 string in UTF-8, the module's paths' encoding: Windows' main and environ have the code page's */
static char *utf8(const wchar_t *wide) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text = bytes > 0 ? malloc((size_t)bytes) : NULL;

    if (text == NULL) {
        fprintf(stderr, "crash2-pc: fatal: no memory for the arguments and environment\n");
        exit(EXIT_FAILURE);
    } else {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, bytes, NULL, NULL);
        return text;
    }
}

/* A list of count strings and its terminating NULL */
static char **list(size_t count) {
    char **strings = calloc(count + 1, sizeof *strings);

    if (strings == NULL) {
        fprintf(stderr, "crash2-pc: fatal: no memory for the arguments and environment\n");
        exit(EXIT_FAILURE);
    } else {
        return strings;
    }
}

static char **utf8_arguments(int *argc) {
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), argc);
    char **argv;

    if (wide == NULL) {
        fprintf(stderr, "crash2-pc: fatal: no command line\n");
        exit(EXIT_FAILURE);
    } else {
        argv = list((size_t)*argc);
    }
    for (int i = 0; i < *argc; i++) {
        argv[i] = utf8(wide[i]);
    }
    LocalFree(wide);
    return argv;
}

/* The environment block: NUL-terminated strings, an empty one last */
static char **utf8_environment(void) {
    wchar_t *block = GetEnvironmentStringsW();
    size_t count = 0;
    char **environment;

    if (block == NULL) {
        return list(0);
    } else {
        for (const wchar_t *at = block; *at != L'\0'; at += wcslen(at) + 1) {
            count++;
        }
        environment = list(count);
    }
    count = 0;
    for (const wchar_t *at = block; *at != L'\0'; at += wcslen(at) + 1) {
        environment[count++] = utf8(at);
    }
    FreeEnvironmentStringsW(block);
    return environment;
}
#else
extern char **environ;
#endif

/* The module's start: the C library's, then the port's main (src/main.c); a trap ends the program with its reason */
int main(int argc, char **argv) {
    wasm_rt_trap_t trap;

#ifdef _WIN32
    argv = utf8_arguments(&argc);
    Wasi_Start(argc, argv, utf8_environment());
#else
    Wasi_Start(argc, argv, environ);
#endif
    wasm_rt_init();
    wasm2c_crash2_instantiate(&g_Module, &g_Env, &g_Wasi);
    trap = wasm_rt_impl_try();
    if (trap == WASM_RT_TRAP_NONE) {
        w2c_crash2_0x5Fstart(&g_Module);
    } else {
        fprintf(stderr, "crash2-pc: fatal: the module trapped: %s\n", wasm_rt_strerror(trap));
        return EXIT_FAILURE;
    }
    wasm2c_crash2_free(&g_Module);
    wasm_rt_free();
    return EXIT_SUCCESS;
}
