#include <stddef.h>

#include "host_log.h"
#include "psyq/host_psyq.h"

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

typedef struct {
    unsigned int descriptor;
    int spec;
    int mode;
    void (*handler)(void);
    int open;
    int enabled;
} Event;

static Event s_Events[EVENTS];
static unsigned int s_Targets[ROOT_COUNTERS];
static int s_Running[ROOT_COUNTERS];
/* CPU cycles, times VIDEO_HZ, since root counter 2's last interrupt */
static unsigned long long s_Counter2Cycles;
static int s_Frames;

static Event *event_of(int handle) {
    unsigned int index = (unsigned int)handle & EVENT_HANDLE_MASK;

    if (((unsigned int)handle & ~EVENT_HANDLE_MASK) != EVENT_HANDLE_BASE || index >= EVENTS || !s_Events[index].open) {
        return NULL;
    } else {
        return &s_Events[index];
    }
}

int OpenEvent(unsigned int descriptor, int spec, int mode, void (*handler)(void)) {
    int index;

    for (index = 0; index < EVENTS; index++) {
        if (!s_Events[index].open) {
            s_Events[index].descriptor = descriptor;
            s_Events[index].spec = spec;
            s_Events[index].mode = mode;
            s_Events[index].handler = handler;
            s_Events[index].open = 1;
            s_Events[index].enabled = 0;
            return (int)(EVENT_HANDLE_BASE | (unsigned int)index);
        } else {
            /* Taken */
        }
    }
    Host_Log("OpenEvent(0x%08X): no event left", descriptor);
    return EVENT_FAILURE;
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

/* TRADEOFF: no event is ever pending (the memory card's come with milestone 7) */
int TestEvent(int handle) {
    (void)handle;
    return 0;
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

int StartRCnt(unsigned int spec) {
    return set_running(spec, 1);
}

int StopRCnt(unsigned int spec) {
    return set_running(spec, 0);
}

/* Root counter 2's interrupts in a frame, each calling the enabled events' handlers on it */
static void counter2_frame(void) {
    unsigned long long period = ROOT_COUNTER_2_DIVIDER * s_Targets[ROOT_COUNTER_2_INDEX] * VIDEO_HZ;
    int index;

    if (!s_Running[ROOT_COUNTER_2_INDEX] || period == 0) {
        return;
    } else {
        s_Counter2Cycles += CPU_HZ * VIDEO_CYCLES_PER_LINE * LINES_PER_FRAME;
    }
    while (s_Counter2Cycles >= period) {
        s_Counter2Cycles -= period;
        for (index = 0; index < EVENTS; index++) {
            Event *event = &s_Events[index];

            if (event->open && event->enabled && event->descriptor == ROOT_COUNTER_2 && event->handler != NULL) {
                event->handler();
            } else {
                /* Another event, or one not taking it */
            }
        }
    }
}

/* Headless, a line every so many blanks shows the game going on */
#define FRAMES_PER_LOG 600

void Host_VerticalBlank(void) {
    s_Frames++;
    Host_PadVerticalBlank();
    counter2_frame();
    if (s_Frames % FRAMES_PER_LOG == 0) {
        Host_Log("vertical blank %d", s_Frames);
    } else {
        /* Between logs */
    }
}

/* VSync(n) waits n blanks (0 the next one); a negative mode returns the blanks so far without waiting */
int VSync(int mode) {
    int blanks;

    if (mode < 0) {
        return s_Frames;
    } else if (mode == 0) {
        Host_VerticalBlank();
        return s_Frames;
    } else {
        for (blanks = 0; blanks < mode; blanks++) {
            Host_VerticalBlank();
        }
        return s_Frames;
    }
}
