#ifndef GAME_CLOCK_H
#define GAME_CLOCK_H

/* decomp's tools/run_game.lua game clock (game_clock.c): whether CRASH2_GAME_CLOCK set it, and its step on entering
 * Gfx_EndFrame */
int Host_GameClock(void);
void Host_FrameEnding(void);
/* The buttons held in the game frame running: CRASH2_GAME_CLOCK_PRESSES's, or the player's recorded */
unsigned int Host_ClockButtons(void);

#endif /* GAME_CLOCK_H */
