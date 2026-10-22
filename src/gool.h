#ifndef GOOL_H
#define GOOL_H

#include <stdint.h>

/* The game's objects and their GOOL entries as the host reads them, by offset: decomp's include/game/object.h
 * (GoolObject) and include/game/nsf.h (NsfEntry). An object's registers are its words from OBJECT_REGISTERS on */

#define OBJECT_EXE 0x0C
#define OBJECT_STATE 0x1C
#define OBJECT_REGISTERS 0x40
#define OBJECT_STATUS_A 0xA8
#define OBJECT_STATUS_B 0xAC
#define OBJECT_SUBTYPE 0xB4
#define OBJECT_SP 0xBC
#define OBJECT_PC 0xC0
#define OBJECT_FRAME_TIME 0xDC
#define OBJECT_STATE_TIME 0xE0
#define OBJECT_DISPLAY_MODE 0x108
#define ENTRY_EID 0x04
#define ENTRY_ITEMS 0x10
#define ENTRY_CODE_ITEM 1
#define ENTRY_ANIMATIONS_ITEM 5
#define WORD_BYTES 4

/* A word of an object or an entry, by its offset */
#define GOOL_WORD(base, offset) (*(int32_t *)((unsigned char *)(base) + (offset)))
/* An object's register by its number, as the GOOL code names it */
#define GOOL_REGISTER(object, number) GOOL_WORD(object, OBJECT_REGISTERS + (number) * WORD_BYTES)
/* An entry's item by its number */
#define GOOL_ITEM(entry, number) ((uint32_t *)(uintptr_t)GOOL_WORD(entry, ENTRY_ITEMS + (number) * WORD_BYTES))

/* An EID's name: 5 characters of 6 bits from bit 25 down, in the game's alphabet, written with its end */
#define EID_NAME_BYTES 6
void Host_EidName(uint32_t eid, char *name);

/* The game's walk over its object lists (decomp's 800158E0.c) */
void Obj_ForEachList(void *each_list, void *each_object, void *argument);
int Obj_ForEachDescendant(void *list, void *each_object, void *argument);

#endif /* GOOL_H */
