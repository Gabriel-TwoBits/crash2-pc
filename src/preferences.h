#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <stddef.h>

/* A file of the port's in SDL's preference folder (on Linux under ~/.local/share, on Windows under %APPDATA%), its
 * path written into path: 0 once written, -1 with no folder to be had (preferences.c) */
int Host_PreferencePath(const char *name, char *path, size_t bytes);

#endif /* PREFERENCES_H */
