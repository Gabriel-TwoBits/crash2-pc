#include <stddef.h>
#include <stdint.h>

#include "host_log.h"
#include "audio.h"
#include "game_clock.h"
#include "psyq/host_psyq.h"
#include "video.h"

/* libapi's events and root counters, and libetc's VSync, on the game's own time: a VSync is one vertical blank, and
 * the root counter's interrupts come in the proportion of the CPU's cycles to a frame's.
 * TRADEOFF: time passes only at a VSync, not while the game computes; real time comes with the window (milestone 5) */

/* The CPU's clock; an NTSC frame: 263 scanlines of 3,413 video cycles at 53.693175 MHz (psx-spx, "GPU Timings") */
#define CPU_HZ 33868800ULL
#define VIDEO_HZ 53693175ULL
#define VIDEO_CYCLES_PER_LINE 3413ULL
#define LINES_PER_FRAME 263ULL
/* Root counter 2 counts the CPU's clock / 8: SetRCnt writes it the mode 0x258 (decomp's split, libapi's SetRCnt) */
#define ROOT_COUNTER_2 0xF2000002
#define ROOT_COUNTER_2_DIVIDER 8ULL
#define ROOT_COUNTER_2_INDEX 2
#define ROOT_COUNTERS 3
#define ROOT_COUNTER_MASK 0xFFFF

/* OpenEvent's handles, as libapi numbers them, and the events a table holds */
#define EVENT_HANDLE_BASE 0xF1000000
#define EVENT_HANDLE_MASK 0xFFFF
#define EVENTS 16
#define EVENT_FAILURE -1
#define EVENT_SUCCESS 1
/* An event's mode: its handler runs when it is delivered (EvMdINTR), or it waits for TestEvent or WaitEvent
 * (EvMdNOINTR); a root counter's interrupt is EvSpINT */
#define EVENT_MODE_INTERRUPT 0x1000
#define EVENT_SPEC_INTERRUPT 2

/* An event's handler is the host's (the game's C) or the retail sound libraries' code (sdk.c) */
typedef struct {
    unsigned int descriptor;
    int spec;
    int mode;
    void (*handler)(void);
    unsigned int retail_handler;
    int open;
    int enabled;
    int ready;
} Event;

static Event s_Events[EVENTS];
static unsigned int s_Targets[ROOT_COUNTERS];
static int s_Running[ROOT_COUNTERS];
/* CPU cycles, times VIDEO_HZ, since root counter 2's last interrupt */
static unsigned long long s_Counter2Cycles;
static int s_Frames;
/* The blanks at VSync's last return, from which VSync(n) counts */
static int s_LastVSync;
/* CPU cycles, times VIDEO_HZ, not yet given to the SPU */
static unsigned long long s_SpuCycles;

static Event *event_of(int handle) {
    unsigned int index = (unsigned int)handle & EVENT_HANDLE_MASK;

    if (((unsigned int)handle & ~EVENT_HANDLE_MASK) != EVENT_HANDLE_BASE || index >= EVENTS || !s_Events[index].open) {
        return NULL;
    } else {
        return &s_Events[index];
    }
}

static int open_event(unsigned int descriptor, int spec, int mode, void (*handler)(void), unsigned int retail_handler) {
    int index;

    for (index = 0; index < EVENTS; index++) {
        if (!s_Events[index].open) {
            s_Events[index].descriptor = descriptor;
            s_Events[index].spec = spec;
            s_Events[index].mode = mode;
            s_Events[index].handler = handler;
            s_Events[index].retail_handler = retail_handler;
            s_Events[index].open = 1;
            s_Events[index].enabled = 0;
            s_Events[index].ready = 0;
            return (int)(EVENT_HANDLE_BASE | (unsigned int)index);
        } else {
            /* Taken */
        }
    }
    Host_Log("OpenEvent(0x%08X): no event left", descriptor);
    return EVENT_FAILURE;
}

int OpenEvent(unsigned int descriptor, int spec, int mode, void (*handler)(void)) {
    return open_event(descriptor, spec, mode, handler, 0);
}

int Host_OpenRetailEvent(unsigned int descriptor, int spec, int mode, unsigned int handler) {
    return open_event(descriptor, spec, mode, NULL, handler);
}

static int set_enabled(int handle, int enabled) {
    Event *event = event_of(handle);

    if (event == NULL) {
        return 0;
    } else {
        event->enabled = enabled;
        return EVENT_SUCCESS;
    }
}

int EnableEvent(int handle) {
    return set_enabled(handle, 1);
}

int DisableEvent(int handle) {
    return set_enabled(handle, 0);
}

int CloseEvent(int handle) {
    Event *event = event_of(handle);

    if (event == NULL) {
        return 0;
    } else {
        event->open = 0;
        return EVENT_SUCCESS;
    }
}

/* The events open on the descriptor and spec delivered: a handler run, or the event ready for TestEvent. On the game
 * clock, the game's handlers on root counter 2 don't run: the clock sets the game's time itself (game_clock.c), as
 * decomp's tools/run_game.lua undoes the handler's count */
static void deliver(unsigned int descriptor, int spec) {
    int index;

    for (index = 0; index < EVENTS; index++) {
        Event *event = &s_Events[index];

        if (!event->open || !event->enabled || event->descriptor != descriptor || event->spec != spec) {
            continue;
        } else if (!(event->mode & EVENT_MODE_INTERRUPT)) {
            event->ready = 1;
        } else if (event->handler != NULL && !(descriptor == ROOT_COUNTER_2 && Host_GameClock())) {
            event->handler();
        } else if (event->retail_handler != 0) {
            Host_SoundCall(event->retail_handler);
        } else {
            /* No handler, or the game's on the game clock */
        }
    }
}

void DeliverEvent(unsigned int descriptor, int spec) {
    deliver(descriptor, spec);
}

int TestEvent(int handle) {
    Event *event = event_of(handle);
    int ready = event != NULL && event->ready;

    if (ready) {
        event->ready = 0;
    } else {
        /* Not delivered */
    }
    return ready;
}

/* WaitEvent: the event's delivery comes from an interrupt, which the host delivers before the code goes on */
int WaitEvent(int handle) {
    if (!TestEvent(handle)) {
        Host_Fatal("WaitEvent(0x%08X) for an event nothing delivers", (unsigned int)handle);
    } else {
        /* Delivered */
    }
    return EVENT_SUCCESS;
}

void EnterCriticalSection(void) {
}

void ExitCriticalSection(void) {
}

static int counter_index(unsigned int spec) {
    unsigned int index = spec & ROOT_COUNTER_MASK;

    if (index >= ROOT_COUNTERS) {
        return -1;
    } else {
        return (int)index;
    }
}

int SetRCnt(unsigned int spec, unsigned short target, int mode) {
    int index = counter_index(spec);

    (void)mode;
    if (index < 0) {
        return 0;
    } else {
        s_Targets[index] = target;
        return 1;
    }
}

static int set_running(unsigned int spec, int running) {
    int index = counter_index(spec);

    if (index < 0) {
        return 0;
    } else {
        s_Running[index] = running;
        return 1;
    }
}

int ResetRCnt(unsigned int spec) {
    if (counter_index(spec) == ROOT_COUNTER_2_INDEX) {
        s_Counter2Cycles = 0;
        return 1;
    } else {
        return counter_index(spec) >= 0;
    }
}

int StartRCnt(unsigned int spec) {
    return set_running(spec, 1);
}

int StopRCnt(unsigned int spec) {
    return set_running(spec, 0);
}

/* Root counter 2's interrupts in a frame, each delivering its events */
static void counter2_frame(void) {
    unsigned long long period = ROOT_COUNTER_2_DIVIDER * s_Targets[ROOT_COUNTER_2_INDEX] * VIDEO_HZ;

    if (!s_Running[ROOT_COUNTER_2_INDEX] || period == 0) {
        return;
    } else {
        s_Counter2Cycles += CPU_HZ * VIDEO_CYCLES_PER_LINE * LINES_PER_FRAME;
    }
    while (s_Counter2Cycles >= period) {
        s_Counter2Cycles -= period;
        deliver(ROOT_COUNTER_2, EVENT_SPEC_INTERRUPT);
    }
}

/* Headless, a line every so many blanks shows the game going on */
#define FRAMES_PER_LOG 600

int Host_Blanks(void) {
    return s_Frames;
}

void Host_VerticalBlank(void) {
    s_Frames++;
    Host_PresentFrame();
    Host_PadVerticalBlank();
    counter2_frame();
    Host_SoundVerticalBlank();
    s_SpuCycles += CPU_HZ * VIDEO_CYCLES_PER_LINE * LINES_PER_FRAME;
    Host_AudioFrame((uint32_t)(s_SpuCycles / VIDEO_HZ));
    s_SpuCycles %= VIDEO_HZ;
    if (s_Frames % FRAMES_PER_LOG == 0) {
        Host_Log("vertical blank %d", s_Frames);
    } else {
        /* Between logs */
    }
}

/* VSync(0) waits for the next blank, VSync(n) until n blanks have passed since its last return (the game frame's
 * first VSync(0) takes the game clock's pace, game_clock.c); a negative mode returns the blanks so far without
 * waiting */
int VSync(int mode) {
    mode = Host_PacedVSync(mode);
    if (mode < 0) {
        return s_Frames;
    } else if (mode == 0) {
        Host_VerticalBlank();
    } else {
        while (s_Frames - s_LastVSync < mode) {
            Host_VerticalBlank();
        }
    }
    s_LastVSync = s_Frames;
    return s_Frames;
}
