#include <SDL3/SDL.h>
#include <stdio.h>

#include "preferences.h"

#define PREFERENCE_ORGANIZATION "crash2-pc"
#define PREFERENCE_APPLICATION "crash2-pc"
#define NO_FOLDER -1

int Host_PreferencePath(const char *name, char *path, size_t bytes) {
    char *folder = SDL_GetPrefPath(PREFERENCE_ORGANIZATION, PREFERENCE_APPLICATION);
    int written;

    if (folder == NULL) {
        return NO_FOLDER;
    } else {
        written = snprintf(path, bytes, "%s%s", folder, name);
        SDL_free(folder);
    }
    return written < 0 || (size_t)written >= bytes ? NO_FOLDER : 0;
}
