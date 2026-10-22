#ifndef INPUT_H
#define INPUT_H

#include <SDL3/SDL.h>

/* A digital pad's buttons, a bit each in its order (psx-spx, "Pad Communication") */
enum {
    PAD_SELECT = 1 << 0,
    PAD_L3 = 1 << 1,
    PAD_R3 = 1 << 2,
    PAD_START = 1 << 3,
    PAD_UP = 1 << 4,
    PAD_RIGHT = 1 << 5,
    PAD_DOWN = 1 << 6,
    PAD_LEFT = 1 << 7,
    PAD_L2 = 1 << 8,
    PAD_R2 = 1 << 9,
    PAD_L1 = 1 << 10,
    PAD_R1 = 1 << 11,
    PAD_TRIANGLE = 1 << 12,
    PAD_CIRCLE = 1 << 13,
    PAD_CROSS = 1 << 14,
    PAD_SQUARE = 1 << 15
};

/* The host's keyboard and gamepad as the pad's buttons (input.c): set while pressed, in the digital pad's order */
void Host_InputStart(void);
void Host_InputEvent(const SDL_Event *event);
unsigned int Host_InputButtons(void);

#endif /* INPUT_H */
