#include "host_log.h"

/* The PsyQ functions the game calls, each a stub that returns 0 and logs its first call.
 * TRADEOFF: stubs until each library is written for the host (docs/ROADMAP.md, milestones 4 to 7); events.c,
 * card.c, libcd.c, libgpu.c, libgte.c and pad.c hold those written so far, and src/sdk.c runs the retail libsnd and
 * libspu */
#define PSYQ_STUB(name) \
    int name() { \
        static int called; \
        if (!called) { \
            called = 1; \
            Host_Log("stub: %s", #name); \
        } else { \
            /* Logged on the first call only */ \
        } \
        return 0; \
    }

/* libetc */
PSYQ_STUB(ResetCallback)
PSYQ_STUB(SetVideoMode)
PSYQ_STUB(StopCallback)
PSYQ_STUB(restartIntr)
PSYQ_STUB(setIntr)
PSYQ_STUB(startIntr)
PSYQ_STUB(stopIntr)

/* libgpu, and the internals its driver table in the data names (_addque to _sync) */
PSYQ_STUB(_addque)
PSYQ_STUB(_addque2)
PSYQ_STUB(_clr)
PSYQ_STUB(_ctl)
PSYQ_STUB(_cwb)
PSYQ_STUB(_cwc)
PSYQ_STUB(_drs)
PSYQ_STUB(_dws)
PSYQ_STUB(_exeque)
PSYQ_STUB(_getctl)
PSYQ_STUB(_otc)
PSYQ_STUB(_param)
PSYQ_STUB(_reset)
PSYQ_STUB(_status)
PSYQ_STUB(_sync)
