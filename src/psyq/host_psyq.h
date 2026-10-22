#ifndef HOST_PSYQ_H
#define HOST_PSYQ_H

/* The host's side of its PsyQ libraries: what main.c drives */

/* libcd's disc: a .cue (with its .bin) or a .iso */
void Host_OpenDisc(const char *path);

/* One vertical blank of the game's time: the root counter's interrupts it holds (libetc.c) */
void Host_VerticalBlank(void);

/* Where libgpu last put the display, and whether it shows (libgpu.c) */
void Host_DisplayArea(int *x, int *y, int *width, int *height, int *enabled);

/* The pad buffers' refresh at a vertical blank (pad.c) */
void Host_PadVerticalBlank(void);

#endif /* HOST_PSYQ_H */
