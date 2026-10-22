#ifndef SETTINGS_H
#define SETTINGS_H

/* The port's settings (settings.c), which the options screen in the game's menus changes (menus.c) */

enum {
    SETTING_FULLSCREEN,
    SETTING_STRETCH,
    SETTING_SMOOTH,
    SETTING_RESOLUTION,
    SETTING_STEADY,
    SETTING_PERSPECTIVE,
    SETTING_COUNT
};

typedef struct {
    const char *key;           /* its name in settings.ini */
    const char *const *values; /* its values' names on the options screen, in the game's font (capitals), a value
                                * being its index */
    int count;
    int value;
} Setting;

extern Setting g_Settings[SETTING_COUNT];

/* The resolution's multiple of the PlayStation's, 1 its own (the software picture) */
int Host_ResolutionScale(void);
void Host_LoadSettings(void);
void Host_SaveSettings(void);

#endif /* SETTINGS_H */
