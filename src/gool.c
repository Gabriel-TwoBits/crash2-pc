#include "gool.h"
#include "retail.h"

#define EID_NAME_CHARACTERS (EID_NAME_BYTES - 1)
#define EID_FIRST_SHIFT 25
#define EID_CHARACTER_BITS 6
#define EID_CHARACTER_MASK 0x3F

void Host_EidName(uint32_t eid, char *name) {
    int index;

    for (index = 0; index < EID_NAME_CHARACTERS; index++) {
        name[index] = g_EidAlphabet[(eid >> (EID_FIRST_SHIFT - index * EID_CHARACTER_BITS)) & EID_CHARACTER_MASK];
    }
    name[EID_NAME_CHARACTERS] = '\0';
}
