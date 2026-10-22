#ifndef BETWEEN_H
#define BETWEEN_H

/* 60 frames a second: a frame of the picture alone between two of the game's (between.c) */

/* decomp's FRAME_BETWEEN, FRAME_BETWEEN_SHOWN and FRAME_ENDED */
int Host_FrameBetween(void);
void Host_FrameBetweenShown(void);
void Host_FrameEnded(void);
/* The corner of the frame to show at a vertical blank, in the order drawn (the one between, then the game's): 0 when
 * the display is the game's own */
int Host_ShownArea(int *x, int *y);

#endif /* BETWEEN_H */
