#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_log.h"
#include "preferences.h"
#include "psyq/host_psyq.h"

/* The memory card in port 1, in a file: a 128 KB image of the card (the emulators' .mcd), its frames written through
 * as the game writes them. CRASH2_MEMORY_CARD names the file, else memcard1.mcd in SDL's preference folder; a missing
 * file is a formatted, empty card. On it, the BIOS's file functions on "bu00:" and libcard's calls the game makes, as
 * psx-spx documents them ("Memory Card Data Format", "BIOS Memory Card Functions"): the directory in block 0, each file
 * in 8 KB blocks chained from its first. libcard's answers come as their events at once.
 * TRADEOFF: no card in port 2, and the card's timing left out: the game waits on the events, whatever they take */

#define CARD_VARIABLE "CRASH2_MEMORY_CARD"
#define CARD_FILE_NAME "memcard1.mcd"
#define DEVICE "bu00:"
#define PATH_BYTES 4096

/* The card's frames and blocks; block 0 the directory: frame 0 the header, frames 1 to 15 a block's entry each,
 * frames 16 to 35 the broken frames' list, frame 63 the write test */
#define FRAME_BYTES 128
#define CARD_FRAMES 1024
#define CARD_BYTES (FRAME_BYTES * CARD_FRAMES)
#define BLOCK_BYTES 0x2000
#define BLOCKS 16
#define FILE_BLOCKS (BLOCKS - 1)
#define HEADER_FRAME 0
#define BROKEN_LIST_FIRST 16
#define BROKEN_LIST_FRAMES 20
#define WRITE_TEST_FRAME 63
#define CHECKSUM_OFFSET (FRAME_BYTES - 1)
#define HEADER_ID "MC"
/* A directory entry: its state, the file's size (in its first block), the next block (FILE_BLOCKS of them, from 0),
 * the file's name. A file's first or only block is STATE_FIRST, the others STATE_MIDDLE and, at its end, STATE_LAST;
 * erased, each takes its STATE_DELETED_ state */
#define ENTRY_STATE 0x00
#define ENTRY_SIZE 0x04
#define ENTRY_NEXT 0x08
#define ENTRY_NAME 0x0A
#define NAME_BYTES 21
#define NO_NEXT 0xFFFF
#define NO_BROKEN_FRAME 0xFFFFFFFFu
#define BROKEN_FRAME_NONE_OFFSET 0x08
enum {
    STATE_FIRST = 0x51,
    STATE_MIDDLE = 0x52,
    STATE_LAST = 0x53,
    STATE_FREE = 0xA0,
    STATE_DELETED_FIRST = 0xA1,
    STATE_DELETED_MIDDLE = 0xA2,
    STATE_DELETED_LAST = 0xA3
};
/* A state's change on erasing: in use (0x5N) to deleted (0xAN) */
#define STATE_ERASED_DELTA (STATE_DELETED_FIRST - STATE_FIRST)
#define WORD_BYTES 4
#define HALF_BYTES 2
#define BYTE_BITS 8

/* open's access mode: read, write, create with the number of blocks in its high half */
#define OPEN_READ 0x1
#define OPEN_WRITE 0x2
#define OPEN_CREATE 0x200
#define OPEN_BLOCKS_SHIFT 16
#define FILE_DESCRIPTOR_BASE 2
#define FILES_OPEN 4
#define BIOS_FAILURE -1
#define BIOS_SUCCESS 1
#define BIOS_FALSE 0

/* libcard's events: the software's (SwCARD, _card_info and _card_load) and the hardware's (HwCARD, a frame's read or
 * write), each with its end: done (EvSpIOE), an error, a time-out, a new card */
#define SW_CARD 0xF4000001u
#define HW_CARD 0xF0000011u
#define EVENT_DONE 0x0004
#define EVENT_NEW_CARD 0x2000

/* firstfile's and nextfile's DIRENTRY. A name of 20 characters fills name, its end the attribute's first byte: the
 * game reads it as a string, and sorts its directory by swapping the names alone (decomp's Save_SortDirectory), so the
 * attribute stays 0 */
typedef struct {
    char name[20];
    int32_t attribute;
    int32_t size;
    uint32_t next;
    int32_t head;
    char system[4];
} DirectoryEntry;

typedef struct {
    int open;
    int block;
    uint32_t position, size;
    int mode;
} File;

static uint8_t s_Card[CARD_BYTES];
static FILE *s_File;
static int s_Loaded;
/* Until its first write since power-on the card reads as a new card, which _new_card lets the next frame's access
 * pass */
static int s_NewCard = 1;
static int s_IgnoreNewCard;
static File s_Files[FILES_OPEN];
static char s_Pattern[NAME_BYTES];
static int s_NextEntry;

static uint8_t *frame(int index) {
    return s_Card + index * FRAME_BYTES;
}

static uint8_t *entry(int block) {
    return frame(block + 1);
}

static void set_checksum(uint8_t *bytes) {
    uint8_t sum = 0;
    int index;

    for (index = 0; index < CHECKSUM_OFFSET; index++) {
        sum ^= bytes[index];
    }
    bytes[CHECKSUM_OFFSET] = sum;
}

static uint32_t little(const uint8_t *bytes, int count) {
    uint32_t value = 0;
    int index;

    for (index = count - 1; index >= 0; index--) {
        value = (value << BYTE_BITS) | bytes[index];
    }
    return value;
}

static void set_little(uint8_t *bytes, uint32_t value, int count) {
    int index;

    for (index = 0; index < count; index++) {
        bytes[index] = (uint8_t)(value >> (index * BYTE_BITS));
    }
}

/* A formatted card: the header, the free entries, the empty broken frames' list, the write test */
static void format_card(void) {
    int index;

    memset(s_Card, 0, BLOCK_BYTES);
    memcpy(frame(HEADER_FRAME), HEADER_ID, strlen(HEADER_ID));
    set_checksum(frame(HEADER_FRAME));
    for (index = 0; index < FILE_BLOCKS; index++) {
        entry(index)[ENTRY_STATE] = STATE_FREE;
        set_little(entry(index) + ENTRY_NEXT, NO_NEXT, HALF_BYTES);
        set_checksum(entry(index));
    }
    for (index = 0; index < BROKEN_LIST_FRAMES; index++) {
        set_little(frame(BROKEN_LIST_FIRST + index), NO_BROKEN_FRAME, WORD_BYTES);
        set_little(frame(BROKEN_LIST_FIRST + index) + BROKEN_FRAME_NONE_OFFSET, NO_NEXT, HALF_BYTES);
        set_checksum(frame(BROKEN_LIST_FIRST + index));
    }
    memcpy(frame(WRITE_TEST_FRAME), frame(HEADER_FRAME), FRAME_BYTES);
}

/* The card's frames to its file, from first for count frames */
static void save_frames(int first, int count) {
    if (fseek(s_File, (long)first * FRAME_BYTES, SEEK_SET) != 0 ||
        fwrite(frame(first), FRAME_BYTES, (size_t)count, s_File) != (size_t)count || fflush(s_File) != 0) {
        Host_Fatal("can't write the memory card's file");
    } else {
        /* Written */
    }
}

static void load(void) {
    const char *named = getenv(CARD_VARIABLE);
    char path[PATH_BYTES];

    s_Loaded = 1;
    if (named != NULL) {
        snprintf(path, sizeof(path), "%s", named);
    } else if (Host_PreferencePath(CARD_FILE_NAME, path, sizeof(path)) != 0) {
        Host_Fatal("no folder for the memory card (%s), and no %s set", SDL_GetError(), CARD_VARIABLE);
    } else {
        /* The preferences' card */
    }
    s_File = fopen(path, "r+b");
    if (s_File != NULL && fread(s_Card, CARD_BYTES, 1, s_File) == 1) {
        Host_Log("memory card: %s", path);
    } else if (s_File != NULL) {
        Host_Fatal("%s isn't a 128 KB memory card image", path);
    } else {
        s_File = fopen(path, "w+b");
        if (s_File == NULL) {
            Host_Fatal("can't create the memory card %s", path);
        } else {
            format_card();
            save_frames(0, CARD_FRAMES);
            Host_Log("memory card: %s, new and formatted", path);
        }
    }
}

static void ensure_loaded(void) {
    if (!s_Loaded) {
        load();
    } else {
        /* Loaded */
    }
}

/* A card's write clears its new-card flag */
static void written(int first, int count) {
    s_NewCard = 0;
    save_frames(first, count);
}

static int entry_state(int block) {
    return entry(block)[ENTRY_STATE];
}

static int is_free(int block) {
    return entry_state(block) >= STATE_FREE && entry_state(block) <= STATE_DELETED_LAST;
}

/* The block where the file of that name starts, or -1 */
static int find_file(const char *name) {
    int block;

    for (block = 0; block < FILE_BLOCKS; block++) {
        if (entry_state(block) == STATE_FIRST &&
            strncmp((const char *)entry(block) + ENTRY_NAME, name, NAME_BYTES - 1) == 0) {
            return block;
        } else {
            continue;
        }
    }
    return -1;
}

/* A "bu00:NAME" path's name, or NULL for another device */
static const char *name_of(const char *path) {
    return strncmp(path, DEVICE, strlen(DEVICE)) == 0 ? path + strlen(DEVICE) : NULL;
}

static int next_block(int block) {
    uint32_t next = little(entry(block) + ENTRY_NEXT, HALF_BYTES);

    return next < FILE_BLOCKS ? (int)next : -1;
}

/* A new file of blocks blocks, in the first free ones, chained in order */
static int create_file(const char *name, int blocks) {
    int chosen[FILE_BLOCKS];
    int count = 0;
    int block, index;

    for (block = 0; block < FILE_BLOCKS && count < blocks; block++) {
        if (is_free(block)) {
            chosen[count++] = block;
        } else {
            continue;
        }
    }
    if (blocks <= 0 || count < blocks || find_file(name) >= 0 || strlen(name) >= NAME_BYTES) {
        return -1;
    } else {
        /* Room for it */
    }
    for (index = 0; index < blocks; index++) {
        uint8_t *bytes = entry(chosen[index]);

        memset(bytes, 0, FRAME_BYTES);
        if (index == 0) {
            bytes[ENTRY_STATE] = STATE_FIRST;
            set_little(bytes + ENTRY_SIZE, (uint32_t)blocks * BLOCK_BYTES, WORD_BYTES);
            memcpy(bytes + ENTRY_NAME, name, strlen(name));
        } else if (index == blocks - 1) {
            bytes[ENTRY_STATE] = STATE_LAST;
        } else {
            bytes[ENTRY_STATE] = STATE_MIDDLE;
        }
        set_little(bytes + ENTRY_NEXT, index == blocks - 1 ? NO_NEXT : (uint32_t)chosen[index + 1], HALF_BYTES);
        set_checksum(bytes);
        written(chosen[index] + 1, 1);
    }
    return chosen[0];
}

static File *file_of(int fd) {
    int index = fd - FILE_DESCRIPTOR_BASE;

    if (index < 0 || index >= FILES_OPEN || !s_Files[index].open) {
        return NULL;
    } else {
        return &s_Files[index];
    }
}

/* The card's offset of a file's byte, through its chain of blocks, or -1 past its end */
static long card_offset(const File *file, uint32_t position) {
    int block = file->block;
    uint32_t skipped;

    for (skipped = position / BLOCK_BYTES; skipped > 0 && block >= 0; skipped--) {
        block = next_block(block);
    }
    if (block < 0 || position >= file->size) {
        return -1;
    } else {
        return (long)(block + 1) * BLOCK_BYTES + (long)(position % BLOCK_BYTES);
    }
}

/* The BIOS's file calls the game makes, which share their names with the C library's: the link wraps the game's
 * calls to them (CMakeLists.txt, --wrap) to these */
int __wrap_open(const char *path, int mode) {
    const char *name = name_of(path);
    int index, block;

    ensure_loaded();
    for (index = 0; index < FILES_OPEN && s_Files[index].open; index++) {
        /* The first free descriptor */
    }
    if (name == NULL || index >= FILES_OPEN) {
        Host_Log("open(\"%s\", 0x%X): no such device, or no descriptor left", path, mode);
        return BIOS_FAILURE;
    } else if (mode & OPEN_CREATE) {
        block = create_file(name, mode >> OPEN_BLOCKS_SHIFT);
    } else {
        block = find_file(name);
    }
    if (block < 0) {
        return BIOS_FAILURE;
    } else {
        s_Files[index].open = 1;
        s_Files[index].block = block;
        s_Files[index].position = 0;
        s_Files[index].size = little(entry(block) + ENTRY_SIZE, WORD_BYTES);
        s_Files[index].mode = mode;
        return FILE_DESCRIPTOR_BASE + index;
    }
}

/* read and write go by whole frames, within the file */
static int transfer(int fd, void *buffer, int bytes, int write) {
    File *file = file_of(fd);
    int done;

    if (file == NULL || bytes < 0 || bytes % FRAME_BYTES != 0 || !(file->mode & (write ? OPEN_WRITE : OPEN_READ))) {
        return BIOS_FAILURE;
    } else {
        /* A transfer it can make */
    }
    for (done = 0; done < bytes; done += FRAME_BYTES) {
        long offset = card_offset(file, file->position);

        if (offset < 0) {
            break;
        } else if (write) {
            memcpy(s_Card + offset, (uint8_t *)buffer + done, FRAME_BYTES);
            written((int)(offset / FRAME_BYTES), 1);
        } else {
            memcpy((uint8_t *)buffer + done, s_Card + offset, FRAME_BYTES);
        }
        file->position += FRAME_BYTES;
    }
    return done == bytes ? bytes : BIOS_FAILURE;
}

int __wrap_read(int fd, void *buffer, int bytes) {
    return transfer(fd, buffer, bytes, 0);
}

int __wrap_write(int fd, const void *buffer, int bytes) {
    return transfer(fd, (void *)buffer, bytes, 1);
}

int __wrap_close(int fd) {
    File *file = file_of(fd);

    if (file == NULL) {
        return BIOS_FAILURE;
    } else {
        file->open = 0;
        return fd;
    }
}

/* erase: the file's blocks marked deleted, free for another */
int erase(const char *path) {
    const char *name = name_of(path);
    int block;

    ensure_loaded();
    block = name == NULL ? -1 : find_file(name);
    if (block < 0) {
        return BIOS_FALSE;
    } else {
        /* Its chain from the first block */
    }
    while (block >= 0) {
        int next = next_block(block);
        uint8_t *bytes = entry(block);

        bytes[ENTRY_STATE] = (uint8_t)(bytes[ENTRY_STATE] + STATE_ERASED_DELTA);
        set_checksum(bytes);
        written(block + 1, 1);
        block = next;
    }
    return BIOS_SUCCESS;
}

int format(const char *path) {
    ensure_loaded();
    if (name_of(path) == NULL) {
        return BIOS_FALSE;
    } else {
        format_card();
        written(0, BLOCK_BYTES / FRAME_BYTES);
        return BIOS_SUCCESS;
    }
}

/* A name against firstfile's pattern: '?' any character, '*' the rest */
static int matches(const char *pattern, const char *name) {
    int index;

    for (index = 0; index < NAME_BYTES - 1; index++) {
        if (pattern[index] == '*') {
            return 1;
        } else if (pattern[index] == '\0' || name[index] == '\0') {
            return pattern[index] == name[index];
        } else if (pattern[index] != '?' && pattern[index] != name[index]) {
            return 0;
        } else {
            continue;
        }
    }
    return 1;
}

/* The next file from s_NextEntry that matches s_Pattern, in the directory's order */
static DirectoryEntry *next_match(DirectoryEntry *found) {
    for (; s_NextEntry < FILE_BLOCKS; s_NextEntry++) {
        const uint8_t *bytes = entry(s_NextEntry);
        const char *name = (const char *)bytes + ENTRY_NAME;

        if (bytes[ENTRY_STATE] == STATE_FIRST && matches(s_Pattern, name)) {
            memset(found, 0, sizeof(*found));
            memcpy(found->name, name, sizeof(found->name));
            found->size = (int32_t)little(bytes + ENTRY_SIZE, WORD_BYTES);
            found->head = s_NextEntry + 1;
            s_NextEntry++;
            return found;
        } else {
            continue;
        }
    }
    return NULL;
}

DirectoryEntry *firstfile(const char *pattern, DirectoryEntry *found) {
    const char *name = name_of(pattern);

    ensure_loaded();
    if (name == NULL) {
        return NULL;
    } else {
        snprintf(s_Pattern, sizeof(s_Pattern), "%s", name);
        s_NextEntry = 0;
        return next_match(found);
    }
}

DirectoryEntry *nextfile(DirectoryEntry *found) {
    return next_match(found);
}

/* libcard */
int InitCARD(int pad_enable) {
    (void)pad_enable;
    return BIOS_SUCCESS;
}

int StartCARD(void) {
    return BIOS_SUCCESS;
}

int StopCARD(void) {
    return BIOS_SUCCESS;
}

int _bu_init(void) {
    return BIOS_SUCCESS;
}

/* The card's state, by its software event: a new card, or there */
static void software_answer(void) {
    ensure_loaded();
    DeliverEvent(SW_CARD, s_NewCard ? EVENT_NEW_CARD : EVENT_DONE);
}

int _card_info(int channel) {
    (void)channel;
    software_answer();
    return BIOS_SUCCESS;
}

int _card_load(int channel) {
    (void)channel;
    software_answer();
    return BIOS_SUCCESS;
}

/* _card_clear: a write to the card, which clears its new-card flag */
int _card_clear(int channel) {
    (void)channel;
    ensure_loaded();
    written(WRITE_TEST_FRAME, 1);
    DeliverEvent(HW_CARD, EVENT_DONE);
    return BIOS_SUCCESS;
}

int _new_card(void) {
    s_IgnoreNewCard = 1;
    return BIOS_SUCCESS;
}

/* A frame's read or write, its end the hardware event: a new card unless _new_card came before */
static int frame_access(int channel, int index, uint8_t *buffer, int write) {
    int new_card = s_NewCard && !s_IgnoreNewCard;

    (void)channel;
    ensure_loaded();
    s_IgnoreNewCard = 0;
    if (index < 0 || index >= CARD_FRAMES) {
        return BIOS_FALSE;
    } else if (write) {
        memcpy(frame(index), buffer, FRAME_BYTES);
        written(index, 1);
    } else {
        memcpy(buffer, frame(index), FRAME_BYTES);
    }
    DeliverEvent(HW_CARD, new_card ? EVENT_NEW_CARD : EVENT_DONE);
    return BIOS_SUCCESS;
}

int _card_read(int channel, int index, uint8_t *buffer) {
    return frame_access(channel, index, buffer, 0);
}

int _card_write(int channel, int index, uint8_t *buffer) {
    return frame_access(channel, index, buffer, 1);
}
