#include "host_log.h"

/* The PsyQ functions the game calls, each a stub that returns 0 and logs its first call.
 * TRADEOFF: stubs until each library is written for the host (docs/ROADMAP.md, milestones 4 to 7); libcd.c and
 * events.c, libcd.c, libgpu.c, libgte.c, libspu.c and pad.c hold those written so far */
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

/* libapi: events, interrupts, root counters, the pad and the memory card's BIOS calls */
PSYQ_STUB(_bu_init)
PSYQ_STUB(_card_clear)
PSYQ_STUB(_card_info)
PSYQ_STUB(_card_load)
PSYQ_STUB(_card_read)
PSYQ_STUB(_card_write)
PSYQ_STUB(InitCARD)
PSYQ_STUB(StartCARD)
PSYQ_STUB(StopCARD)
PSYQ_STUB(_new_card)
PSYQ_STUB(erase)
PSYQ_STUB(firstfile)
PSYQ_STUB(format)
PSYQ_STUB(nextfile)

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

/* libsnd and libspu, with the 15 functions of their objects the split leaves unnamed */
PSYQ_STUB(SpuInitMalloc)
PSYQ_STUB(SpuSetReverbDepth)
PSYQ_STUB(SpuSetReverbVoice)
PSYQ_STUB(SsEnd)
PSYQ_STUB(SsInit)
PSYQ_STUB(SsSepClose)
PSYQ_STUB(SsSepOpen)
PSYQ_STUB(SsSepPause)
PSYQ_STUB(SsSepPlay)
PSYQ_STUB(SsSepReplay)
PSYQ_STUB(SsSeqCalledTbyT)
PSYQ_STUB(SsSetMVol)
PSYQ_STUB(SsSetReservedVoice)
PSYQ_STUB(SsSetTableSize)
PSYQ_STUB(SsSetTickMode)
PSYQ_STUB(SsStart)
PSYQ_STUB(SsVabClose)
PSYQ_STUB(SsVabFakeBody)
PSYQ_STUB(SsVabFakeHead)
PSYQ_STUB(func_8004FE38)
PSYQ_STUB(func_800501A8)
PSYQ_STUB(func_80051078)
PSYQ_STUB(func_80051238)
PSYQ_STUB(func_80051298)
PSYQ_STUB(func_800512F8)
PSYQ_STUB(func_800513D8)
PSYQ_STUB(func_8005182C)
PSYQ_STUB(func_800518B8)
PSYQ_STUB(func_800521A8)
PSYQ_STUB(func_800528C4)
PSYQ_STUB(func_80052B4C)
PSYQ_STUB(func_80056304)
PSYQ_STUB(func_80056760)
