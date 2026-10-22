#ifndef SETTINGS_H
#define SETTINGS_H

/* The port's settings (settings.c), which the options screen in the game's menus changes (menus.c) */

enum {
    SETTING_FULLSCREEN,
    SETTING_SHAPE,
    SETTING_SMOOTH,
    SETTING_RESOLUTION,
    SETTING_STEADY,
    SETTING_PERSPECTIVE,
    SETTING_FRAME_RATE,
    SETTING_COUNT
};

typedef struct {
    const char *key;           /* its name in settings.ini */
    const char *const *values; /* its values' names on the options screen, in the game's font (capitals), a value
                                * being its index */
    int count;
    int value;
} Setting;

/* The picture's shapes: the PlayStation's 4:3, that stretched to the window, and a wider view at 16:9 */
enum {
    SHAPE_4_3,
    SHAPE_STRETCHED,
    SHAPE_WIDE
};

extern Setting g_Settings[SETTING_COUNT];

/* decomp's FRAME_HALF_RATE: a game frame each two vertical blanks (30 a second), unless the settings ask for 60 */
int Host_HalfRate(void);
/* The resolution's multiple of the PlayStation's, 1 its own (the software picture) */
int Host_ResolutionScale(void);
void Host_LoadSettings(void);
void Host_SaveSettings(void);

#endif /* SETTINGS_H */
