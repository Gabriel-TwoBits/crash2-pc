#include "host.h"
#include "host_log.h"

/* Operation 73's native code, MIPS from the disc's GOOL entries (decomp/docs/TARGET.md, "The GOOL interpreter") */
void Host_RunNative(struct GoolObject *object, int *state_pool, int **pc, int **sp, unsigned char *frame) {
    (void)object;
    (void)state_pool;
    (void)sp;
    (void)frame;
    Host_Fatal("native code at %p: its interpreter is docs/ROADMAP.md's milestone 6", (void *)*pc);
}
