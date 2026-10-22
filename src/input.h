#ifndef INPUT_H
#define INPUT_H

#include <SDL3/SDL.h>

/* The host's keyboard and gamepad as the pad's buttons (input.c): set while pressed, in the digital pad's order */
void Host_InputStart(void);
void Host_InputEvent(const SDL_Event *event);
unsigned int Host_InputButtons(void);

#endif /* INPUT_H */
