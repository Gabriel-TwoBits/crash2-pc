#include <stdint.h>

#include "gool.h"
#include "settings.h"

/* At 60 game frames a second an object's step through the world's collision is half the PlayStation's, and the
 * collision rounds each step's x and z down to its 2048-unit grid: the rounding took 3% of a running Crash's step at
 * 30, and took 12% at 60. So two steps at 60 go as one at 30: what the rounding takes of the first is carried into
 * the second, as long as the collision didn't stop it (what is left is less than the grid and on the step's side),
 * and what it takes of the second is lost, as the PlayStation's one step loses it. At 30 the game moves as it does
 * on the PlayStation. TRADEOFF: the carries sit in a small table by the object's address, two objects there at once
 * sharing a slot losing theirs; the game moves few objects through the collision */

#define CARRY_SLOTS 64
#define CARRY_SLOT_MASK (CARRY_SLOTS - 1)
#define OBJECT_SIZE_SHIFT 4
#define GRID 2048
#define AXES 3
/* The axes the grid rounds, x and z; y lands on floors */
#define AXIS_X 0
#define AXIS_Z 2

typedef struct {
    void *object;
    int32_t carry[AXES];
    int second; /* the next step the second of a pair */
} Carry;

static Carry s_Carries[CARRY_SLOTS];

/* The game's step through the collision (decomp's 80029884.c) */
void func_80029884(void *object, int32_t step, int32_t zone);

static int32_t left_over(int32_t wanted, int32_t moved) {
    int32_t left = wanted - moved;

    if (left == 0 || left >= GRID || left <= -GRID) {
        return 0;
    } else if ((left > 0) != (wanted > 0)) {
        return 0;
    } else {
        return left;
    }
}

void Host_MoveObject(void *object, int32_t step, int32_t zone) {
    int32_t *wanted = (int32_t *)(uintptr_t)step;
    Carry *slot = &s_Carries[((uintptr_t)object >> OBJECT_SIZE_SHIFT) & CARRY_SLOT_MASK];
    int32_t x = GOOL_WORD(object, OBJECT_X);
    int32_t z = GOOL_WORD(object, OBJECT_Z);

    if (Host_HalfRate()) {
        func_80029884(object, step, zone);
        return;
    } else if (slot->object != object) {
        slot->object = object;
        slot->carry[AXIS_X] = 0;
        slot->carry[AXIS_Z] = 0;
        slot->second = 0;
    } else {
        /* Its carry from its last step */
    }
    wanted[AXIS_X] += slot->carry[AXIS_X];
    wanted[AXIS_Z] += slot->carry[AXIS_Z];
    func_80029884(object, step, zone);
    if (slot->second) {
        slot->carry[AXIS_X] = 0;
        slot->carry[AXIS_Z] = 0;
    } else {
        slot->carry[AXIS_X] = left_over(wanted[AXIS_X], GOOL_WORD(object, OBJECT_X) - x);
        slot->carry[AXIS_Z] = left_over(wanted[AXIS_Z], GOOL_WORD(object, OBJECT_Z) - z);
    }
    slot->second = !slot->second;
}
