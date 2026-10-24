#ifndef HOST_PSYQ_H
#define HOST_PSYQ_H

/* The host's side of its PsyQ libraries: what main.c drives */

/* libcd's disc: a .cue (with its .bin) or a .iso */
void Host_OpenDisc(const char *path);
/* A file of the disc's root directory ("SCUS_941.54;1"), read whole into memory of malloc's */
void *Host_ReadDiscFile(const char *name, unsigned int *bytes);
/* The drive's sectors read by a vertical blank, into the read's buffer (libcd.c) */
void Host_CdVerticalBlank(void);

/* One vertical blank of the game's time: the root counter's interrupts it holds (libetc.c) */
void Host_VerticalBlank(void);

/* The vertical blanks so far (events.c) */
int Host_Blanks(void);
/* The CPU's cycles so far on the game's time: the blanks' and those the host's waits took since the last (events.c) */
unsigned long long Host_Cycles(void);
/* CYCLES of the CPU's time passed waiting on the hardware, with the blanks they reach (events.c) */
void Host_Wait(unsigned long long cycles);

/* Where libgpu last put the display, and whether it shows (libgpu.c) */
void Host_DisplayArea(int *x, int *y, int *width, int *height, int *enabled);

/* The retail sound libraries on the interpreter, loaded from the disc once it is open; a vertical blank's interrupt for
 * them (sdk.c) */
void Host_SoundStart(void);
void Host_SoundVerticalBlank(void);
/* A call into the retail libraries' code, from an event or a callback (sdk.c) */
void Host_SoundCall(unsigned int address);

/* libapi's events and root counters (events.c), for the retail libraries' calls (sdk.c): an event whose handler is
 * their code, and the calls the game's C doesn't make */
int Host_OpenRetailEvent(unsigned int descriptor, int spec, int mode, unsigned int handler);
int EnableEvent(int handle);
void DeliverEvent(unsigned int descriptor, int spec);
int TestEvent(int handle);
int WaitEvent(int handle);
int SetRCnt(unsigned int spec, unsigned short target, int mode);
int ResetRCnt(unsigned int spec);

/* The pad buffers' refresh at a vertical blank (pad.c) */
void Host_PadVerticalBlank(void);

#endif /* HOST_PSYQ_H */
