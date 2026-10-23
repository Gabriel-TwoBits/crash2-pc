#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_log.h"
#include "preferences.h"
#include "settings.h"

/* The settings in settings.ini in the preference folder, a "key=value" line each, the value a whole number, or in the
 * file CRASH2_SETTINGS names. Headless (CRASH2_HEADLESS) only the latter: a test run leaves the player's file alone */

#define SETTINGS_FILE "settings.ini"
#define HEADLESS_VARIABLE "CRASH2_HEADLESS"
#define FILE_VARIABLE "CRASH2_SETTINGS"
#define PATH_BYTES 4096
#define LINE_BYTES 256
#define DECIMAL 10

/* The options screen's names, short: a row shows "<NAME: VALUE>" in some 16 characters. Wobble and warping are the
 * PlayStation's polygons' jitter and its textures' warping, on as it draws them */
static const char *const s_Displays[] = {"WINDOW", "FULL"};
static const char *const s_Shapes[] = {"4:3", "STRETCH", "16:9", "ZOOM"};
static const char *const s_Switches[] = {"OFF", "ON"};
static const char *const s_Resolutions[] = {"PS1", "2X", "3X", "4X", "6X", "8X"};
static const int s_Scales[] = {1, 2, 3, 4, 6, 8};
static const char *const s_PlayStation[] = {"ON", "OFF"};
static const char *const s_FrameRates[] = {"30", "60"};

#define VALUE_COUNT(values) ((int)(sizeof(values) / sizeof((values)[0])))

Setting g_Settings[SETTING_COUNT] = {
    {"fullscreen", "SCREEN", s_Displays, VALUE_COUNT(s_Displays), 0},
    {"shape", "SHAPE", s_Shapes, VALUE_COUNT(s_Shapes), 0},
    {"smooth", "SMOOTH", s_Switches, VALUE_COUNT(s_Switches), 0},
    {"resolution", "RES", s_Resolutions, VALUE_COUNT(s_Resolutions), 0},
    {"steady", "WOBBLE", s_PlayStation, VALUE_COUNT(s_PlayStation), 0},
    {"perspective", "WARPING", s_PlayStation, VALUE_COUNT(s_PlayStation), 0},
    {"fps60", "FPS", s_FrameRates, VALUE_COUNT(s_FrameRates), 0},
};

_Static_assert(VALUE_COUNT(s_Scales) == VALUE_COUNT(s_Resolutions), "a scale for each resolution named");

int Host_ResolutionScale(void) {
    return s_Scales[g_Settings[SETTING_RESOLUTION].value];
}

/* The file's path, 0 with none to be had (or headless with none named) */
static int settings_path(char *path, size_t bytes) {
    const char *named = getenv(FILE_VARIABLE);

    if (named != NULL) {
        return snprintf(path, bytes, "%s", named) < (int)bytes;
    } else if (getenv(HEADLESS_VARIABLE) != NULL) {
        return 0;
    } else {
        return Host_PreferencePath(SETTINGS_FILE, path, bytes) == 0;
    }
}

/* One "key=value" line into its setting; a line that names none, or a value out of its range, logged and left */
static void read_line(char *line) {
    char *equals = strchr(line, '=');
    char *end;
    long value;
    int index;

    line[strcspn(line, "\r\n")] = '\0';
    if (equals == NULL) {
        Host_Log("%s: a line with no value: %s", SETTINGS_FILE, line);
        return;
    } else {
        *equals = '\0';
        value = strtol(equals + 1, &end, DECIMAL);
    }
    for (index = 0; index < SETTING_COUNT; index++) {
        if (strcmp(line, g_Settings[index].key) != 0) {
            continue;
        } else if (*end != '\0' || end == equals + 1 || value < 0 || value >= g_Settings[index].count) {
            Host_Log("%s: %s's value %s is none of its", SETTINGS_FILE, line, equals + 1);
            return;
        } else {
            g_Settings[index].value = (int)value;
            return;
        }
    }
    Host_Log("%s: no setting %s", SETTINGS_FILE, line);
}

void Host_LoadSettings(void) {
    char path[PATH_BYTES];
    char line[LINE_BYTES];
    FILE *file;

    if (!settings_path(path, sizeof(path))) {
        return;
    } else {
        file = fopen(path, "r");
    }
    if (file == NULL) {
        /* None saved yet: the defaults */
        return;
    } else {
        while (fgets(line, sizeof(line), file) != NULL) {
            read_line(line);
        }
        fclose(file);
    }
}

void Host_SaveSettings(void) {
    char path[PATH_BYTES];
    FILE *file;
    int index;

    if (!settings_path(path, sizeof(path))) {
        return;
    } else {
        file = fopen(path, "w");
    }
    if (file == NULL) {
        Host_Log("can't save the settings in %s", path);
        return;
    } else {
        for (index = 0; index < SETTING_COUNT; index++) {
            fprintf(file, "%s=%d\n", g_Settings[index].key, g_Settings[index].value);
        }
        fclose(file);
    }
}
