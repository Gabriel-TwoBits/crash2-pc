#include <stdio.h>
#include <string.h>
#include <windows.h>

/* crash2.exe on Windows: the port is crash2-game.dll, based at 0x80000000 (src/host.h), which this program loads from
 * its own folder and enters. Wine reserves a 32-bit process's address space from 0x80000000 until the process runs, so
 * a program can't be mapped there, and a library loaded once it runs can; on Windows, the large-address-aware process
 * reaches it (CMakeLists.txt) */

#define GAME_LIBRARY "crash2-game.dll"
#define GAME_ENTRY "Host_Main"
#define LAUNCH_FAILURE 1

typedef int (*Entry)(int argc, char **argv);

static int fail(const char *what) {
    char message[MAX_PATH * 2];

    snprintf(message, sizeof(message), "%s (error %lu). The game needs 64-bit Windows, %s beside crash2.exe, and "
             "nothing else loaded at 0x80000000.", what, (unsigned long)GetLastError(), GAME_LIBRARY);
    fprintf(stderr, "crash2: %s\n", message);
    MessageBoxA(NULL, message, "Crash Bandicoot 2", MB_ICONERROR);
    return LAUNCH_FAILURE;
}

int main(int argc, char **argv) {
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
    char *folder_end = strrchr(path, '\\');
    HMODULE game;
    Entry entry;

    if (length == 0 || length >= sizeof(path) || folder_end == NULL ||
        (size_t)(folder_end + 1 - path) + strlen(GAME_LIBRARY) >= sizeof(path)) {
        return fail("can't find crash2.exe's folder");
    } else {
        strcpy(folder_end + 1, GAME_LIBRARY);
        game = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
    if (game == NULL) {
        return fail("can't load " GAME_LIBRARY);
    } else {
        entry = (Entry)(void *)GetProcAddress(game, GAME_ENTRY);
    }
    if (entry == NULL) {
        return fail(GAME_LIBRARY " has no " GAME_ENTRY);
    } else {
        return entry(argc, argv);
    }
}
