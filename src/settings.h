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
    const char *label;         /* its name on the options screen, in the game's font (capitals) */
    const char *const *values; /* its values' names there, a value being its index */
    int count;
    int value;
} Setting;

/* The picture's shapes: the PlayStation's 4:3, that stretched to the window, a wider view at 16:9, and the 4:3
 * picture zoomed to fill 16:9, its top and bottom cut */
enum {
    SHAPE_4_3,
    SHAPE_STRETCHED,
    SHAPE_WIDE,
    SHAPE_ZOOMED
};
/* The share of the picture's height the zoomed shape shows, the middle of it: 16:9 out of 4:3 */
#define ZOOM_SHOWN_NUMERATOR 3
#define ZOOM_SHOWN_DENOMINATOR 4

extern Setting g_Settings[SETTING_COUNT];

/* The resolution's multiple of the PlayStation's, 1 its own (the software picture) */
int Host_ResolutionScale(void);
void Host_LoadSettings(void);
void Host_SaveSettings(void);

#endif /* SETTINGS_H */
