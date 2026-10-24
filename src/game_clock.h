#ifndef GAME_CLOCK_H
#define GAME_CLOCK_H

/* decomp's tools/run_game.lua game clock (game_clock.c): whether CRASH2_GAME_CLOCK set it, and its step on entering
 * Gfx_EndFrame */
int Host_GameClock(void);
void Host_FrameEnding(void);
/* The buttons held in the game frame running: CRASH2_GAME_CLOCK_PRESSES's, or the player's recorded */
unsigned int Host_ClockButtons(void);
/* The game frame running, on the game clock (0 off it) */
int Host_GameFrame(void);
/* Whether the game clock runs a paced schedule, as the PlayStation's runs it compares with */
int Host_GameClockPaced(void);
/* VSync's mode as the schedule's pace gives it: its count for the game frame's first VSync(0), mode otherwise */
int Host_PacedVSync(int mode);

#endif /* GAME_CLOCK_H */
