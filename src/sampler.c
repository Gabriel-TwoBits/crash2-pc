#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "sampler.h"

/* With CRASH2_SAMPLE set, where the program spends its time headless: the instruction a profiling timer interrupts,
 * counted, and the most frequent ones printed at exit (addr2line -f -e build/crash2 names them). A debugging aid: the
 * low RAM's single steps keep gdb from following the game */

#define SAMPLE_VARIABLE "CRASH2_SAMPLE"
#define SAMPLE_INTERVAL_US 1000
#define SLOTS 4096
#define SLOT_MASK (SLOTS - 1)
#define SHOWN 25
#define REPORT_LINE_BYTES 64

typedef struct {
    uint32_t eip;
    uint32_t count;
} Slot;

static Slot s_Slots[SLOTS];
static uint32_t s_Total;

static void on_sample(int signal_number, siginfo_t *info, void *context_pointer) {
    ucontext_t *context = context_pointer;
    uint32_t eip = (uint32_t)context->uc_mcontext.gregs[REG_EIP];
    uint32_t index = (eip * 2654435761u) & SLOT_MASK;
    int probes;

    (void)signal_number;
    (void)info;
    s_Total++;
    for (probes = 0; probes < SLOTS; probes++) {
        Slot *slot = &s_Slots[(index + (uint32_t)probes) & SLOT_MASK];

        if (slot->eip == eip || slot->count == 0) {
            slot->eip = eip;
            slot->count++;
            return;
        } else {
            /* Probe on */
        }
    }
}

static int by_count(const void *a, const void *b) {
    const Slot *first = a;
    const Slot *second = b;

    if (first->count > second->count) {
        return -1;
    } else if (first->count < second->count) {
        return 1;
    } else {
        return 0;
    }
}

static void report(void) {
    int index;

    qsort(s_Slots, SLOTS, sizeof(Slot), by_count);
    fprintf(stderr, "crash2-pc: %u samples\n", s_Total);
    for (index = 0; index < SHOWN && s_Slots[index].count != 0; index++) {
        fprintf(stderr, "crash2-pc: sample 0x%08X %u\n", s_Slots[index].eip, s_Slots[index].count);
    }
}

/* SIGTERM ends a sampled run: the report then, with write(2) only, since the signal may land inside stdio */
static void on_terminate(int signal_number) {
    char line[REPORT_LINE_BYTES];
    int index;
    int length;

    (void)signal_number;
    qsort(s_Slots, SLOTS, sizeof(Slot), by_count);
    length = snprintf(line, sizeof(line), "crash2-pc: %u samples\n", s_Total);
    write(STDERR_FILENO, line, (size_t)length);
    for (index = 0; index < SHOWN && s_Slots[index].count != 0; index++) {
        length = snprintf(line, sizeof(line), "crash2-pc: sample 0x%08X %u\n", s_Slots[index].eip,
                          s_Slots[index].count);
        write(STDERR_FILENO, line, (size_t)length);
    }
    _exit(EXIT_FAILURE);
}

void Host_StartSampler(void) {
    struct sigaction action;
    struct itimerval timer;

    if (getenv(SAMPLE_VARIABLE) == NULL) {
        return;
    } else {
        memset(&action, 0, sizeof(action));
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        action.sa_sigaction = on_sample;
        sigaction(SIGPROF, &action, NULL);
        signal(SIGTERM, on_terminate);
        atexit(report);
        timer.it_interval.tv_sec = 0;
        timer.it_interval.tv_usec = SAMPLE_INTERVAL_US;
        timer.it_value = timer.it_interval;
        setitimer(ITIMER_PROF, &timer, NULL);
    }
}
