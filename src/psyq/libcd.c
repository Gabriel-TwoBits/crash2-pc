#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "host_log.h"
#include "psyq/host_psyq.h"

/* libcd on the player's disc image: a .cue with its .bin (MODE2/2352, Redump's dump) or a 2048-byte-sector .iso. The
 * game reads data only, by sector (decomp/docs/ROADMAP.md section 6, "The libraries"): a read completes at once.
 * TRADEOFF: reads are synchronous; the drive's timing (seek, double speed) only if the game is found to depend on it */

#define SECTOR_DATA_BYTES 2048
#define RAW_SECTOR_BYTES 2352
/* A MODE2 form 1 sector's user data starts after its sync, header and subheader */
#define MODE2_DATA_OFFSET 24
#define ISO_SECTOR_BYTES SECTOR_DATA_BYTES
/* A position's minute, second and sector (BCD) count from the disc's start, 2 seconds before the first data sector */
#define PREGAP_SECTORS 150
#define SECTORS_PER_SECOND 75
#define SECONDS_PER_MINUTE 60
#define BCD_BASE 10
#define BCD_SHIFT 4
#define BCD_LOW_MASK 0xF
#define PATH_BYTES 4096
/* CdControl's commands the game moves the position with (decomp/include/sdk.h, which the C library's headers clash
 * with here) */
#define CdlSetloc 0x02
#define CdlSeekL 0x15
/* CdControl's and CdSync's results (libcd's CdlComplete; the status byte with the motor on) */
#define CD_SUCCESS 1
#define CD_SYNC_COMPLETE 2
#define CD_STATUS_STANDBY 0x02
#define CD_READ_DONE 0
/* ISO 9660 (ECMA-119): the primary volume descriptor's sector and identifier, its root directory's record, and a
 * directory record's fields */
#define VOLUME_DESCRIPTOR_SECTOR 16
#define VOLUME_IDENTIFIER_OFFSET 1
#define VOLUME_IDENTIFIER "CD001"
#define ROOT_RECORD_OFFSET 156
#define RECORD_LENGTH 0
#define RECORD_EXTENT 2
#define RECORD_SIZE 10
#define RECORD_NAME_LENGTH 32
#define RECORD_NAME 33
#define BYTE_BITS 8
#define WORD_BYTES 4

typedef struct {
    unsigned char minute, second, sector, track;
} CdlLOC;

static FILE *s_Image;
static long s_SectorBytes;
static long s_DataOffset;
static int s_Position;

static int from_bcd(unsigned char value) {
    return (value >> BCD_SHIFT) * BCD_BASE + (value & BCD_LOW_MASK);
}

static unsigned char to_bcd(int value) {
    return (unsigned char)(((value / BCD_BASE) << BCD_SHIFT) | (value % BCD_BASE));
}

/* The .bin a .cue names, beside it */
static void open_cue(const char *cue) {
    FILE *file = fopen(cue, "r");
    char line[PATH_BYTES];
    char bin[PATH_BYTES];
    const char *slash = strrchr(cue, '/');
    int found = 0;

    if (file == NULL) {
        Host_Fatal("can't open the disc image %s", cue);
    } else {
        while (!found && fgets(line, sizeof(line), file) != NULL) {
            char *open_quote = strchr(line, '"');
            char *close_quote = open_quote == NULL ? NULL : strchr(open_quote + 1, '"');

            if (strstr(line, "FILE") != NULL && close_quote != NULL) {
                int directory = slash == NULL ? 0 : (int)(slash - cue + 1);

                snprintf(bin, sizeof(bin), "%.*s%.*s", directory, cue, (int)(close_quote - open_quote - 1),
                         open_quote + 1);
                found = 1;
            } else {
                /* Not the FILE line */
            }
        }
        fclose(file);
    }
    if (!found) {
        Host_Fatal("%s names no FILE", cue);
    } else {
        s_Image = fopen(bin, "rb");
        s_SectorBytes = RAW_SECTOR_BYTES;
        s_DataOffset = MODE2_DATA_OFFSET;
        if (s_Image == NULL) {
            Host_Fatal("can't open %s, which %s names", bin, cue);
        } else {
            Host_Log("disc: %s", bin);
        }
    }
}

void Host_OpenDisc(const char *path) {
    size_t length = strlen(path);
    const char *suffix = length >= strlen(".cue") ? path + length - strlen(".cue") : path;

    if (strcmp(suffix, ".cue") == 0 || strcmp(suffix, ".CUE") == 0) {
        open_cue(path);
    } else {
        s_Image = fopen(path, "rb");
        s_SectorBytes = ISO_SECTOR_BYTES;
        s_DataOffset = 0;
        if (s_Image == NULL) {
            Host_Fatal("can't open the disc image %s", path);
        } else {
            Host_Log("disc: %s", path);
        }
    }
}

int CdInit(void) {
    return CD_SUCCESS;
}

int CdSetDebug(int level) {
    (void)level;
    return 0;
}

CdlLOC *CdIntToPos(int sector, CdlLOC *position) {
    int absolute = sector + PREGAP_SECTORS;

    position->minute = to_bcd(absolute / (SECTORS_PER_SECOND * SECONDS_PER_MINUTE));
    position->second = to_bcd(absolute / SECTORS_PER_SECOND % SECONDS_PER_MINUTE);
    position->sector = to_bcd(absolute % SECTORS_PER_SECOND);
    position->track = 0;
    return position;
}

static int position_sector(const CdlLOC *position) {
    return (from_bcd(position->minute) * SECONDS_PER_MINUTE + from_bcd(position->second)) * SECTORS_PER_SECOND +
           from_bcd(position->sector) - PREGAP_SECTORS;
}

/* Setloc and SeekL move to the position; Stop and the rest leave it */
static int control(unsigned char command, const unsigned char *parameter, unsigned char *result) {
    switch (command) {
    case CdlSetloc:
    case CdlSeekL:
        s_Position = position_sector((const CdlLOC *)parameter);
        break;
    default:
        break;
    }
    if (result != NULL) {
        result[0] = CD_STATUS_STANDBY;
    } else {
        /* The caller wants no status */
    }
    return CD_SUCCESS;
}

int CdControl(unsigned char command, unsigned char *parameter, unsigned char *result) {
    return control(command, parameter, result);
}

int CdControlF(unsigned char command, unsigned char *parameter) {
    return control(command, parameter, NULL);
}

static void read_sectors(int first, int sectors, unsigned char *out) {
    int index;

    if (s_Image == NULL) {
        Host_Fatal("a read with no disc image");
    } else {
        for (index = 0; index < sectors; index++) {
            if (fseek(s_Image, (long)(first + index) * s_SectorBytes + s_DataOffset, SEEK_SET) != 0 ||
                fread(out + index * SECTOR_DATA_BYTES, SECTOR_DATA_BYTES, 1, s_Image) != 1) {
                Host_Fatal("can't read sector %d of the disc image", first + index);
            } else {
                /* Read */
            }
        }
    }
}

/* SECTORS sectors' user data from the position into BUFFER; mode's speed doesn't matter here */
int CdRead(int sectors, unsigned long *buffer, int mode) {
    (void)mode;
    read_sectors(s_Position, sectors, (unsigned char *)buffer);
    s_Position += sectors;
    return CD_SUCCESS;
}

static uint32_t little_word(const unsigned char *bytes) {
    uint32_t value = 0;
    int index;

    for (index = WORD_BYTES - 1; index >= 0; index--) {
        value = (value << BYTE_BITS) | bytes[index];
    }
    return value;
}

void *Host_ReadDiscFile(const char *name, unsigned int *bytes) {
    unsigned char sector[SECTOR_DATA_BYTES];
    unsigned char *directory;
    unsigned char *file = NULL;
    uint32_t extent, size, offset;
    size_t name_length = strlen(name);

    read_sectors(VOLUME_DESCRIPTOR_SECTOR, 1, sector);
    if (memcmp(sector + VOLUME_IDENTIFIER_OFFSET, VOLUME_IDENTIFIER, strlen(VOLUME_IDENTIFIER)) != 0) {
        Host_Fatal("the disc image has no ISO 9660 volume");
    } else {
        extent = little_word(sector + ROOT_RECORD_OFFSET + RECORD_EXTENT);
        size = little_word(sector + ROOT_RECORD_OFFSET + RECORD_SIZE);
    }
    directory = malloc(size + SECTOR_DATA_BYTES);
    if (directory == NULL) {
        Host_Fatal("no memory for the disc's root directory");
    } else {
        read_sectors((int)extent, (int)((size + SECTOR_DATA_BYTES - 1) / SECTOR_DATA_BYTES), directory);
    }
    for (offset = 0; offset < size && file == NULL;) {
        const unsigned char *record = directory + offset;

        if (record[RECORD_LENGTH] == 0) {
            /* No record across a sector's end: the next starts the next sector */
            offset = (offset / SECTOR_DATA_BYTES + 1) * SECTOR_DATA_BYTES;
        } else if (record[RECORD_NAME_LENGTH] == name_length &&
                   strncasecmp((const char *)record + RECORD_NAME, name, name_length) == 0) {
            *bytes = little_word(record + RECORD_SIZE);
            file = malloc(*bytes + SECTOR_DATA_BYTES);
            if (file == NULL) {
                Host_Fatal("no memory for the disc's %s", name);
            } else {
                read_sectors((int)little_word(record + RECORD_EXTENT),
                             (int)((*bytes + SECTOR_DATA_BYTES - 1) / SECTOR_DATA_BYTES), file);
            }
        } else {
            offset += record[RECORD_LENGTH];
        }
    }
    free(directory);
    if (file == NULL) {
        Host_Fatal("the disc has no %s", name);
    } else {
        /* Found */
    }
    return file;
}

int CdReadSync(int mode, unsigned char *result) {
    (void)mode;
    if (result != NULL) {
        result[0] = CD_STATUS_STANDBY;
    } else {
        /* The caller wants no status */
    }
    return CD_READ_DONE;
}

int CdSync(int mode, unsigned char *result) {
    (void)mode;
    if (result != NULL) {
        result[0] = CD_STATUS_STANDBY;
    } else {
        /* The caller wants no status */
    }
    return CD_SYNC_COMPLETE;
}

int CdDataSync(int mode) {
    (void)mode;
    return 0;
}
