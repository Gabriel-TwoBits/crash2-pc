#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "game_clock.h"
#include "host_log.h"
#include "psyq/host_psyq.h"

/* libcd on the player's disc image: a .cue with its .bin (MODE2/2352, Redump's dump) or a 2048-byte-sector .iso. The
 * game reads data only, by sector (decomp/docs/ROADMAP.md section 6, "The libraries").
 *
 * On a run paced as the PlayStation's (game_clock.c's pace), the drive takes its time as PCSX-Redux models it: a
 * command's acknowledge, a read's start, seek and sectors at the mode's speed, and the Pause libcd sends after a read's
 * last sector, which keeps CdSync(1) at 0 until it completes; a sector reaches the buffer when it is read, at a libcd
 * call or a vertical blank after. A wait on the drive (CdSync(0), CdReadSync(0), CdDataSync(0)) passes the time to its
 * end, and a poll that finds it busy passes as long as the game's polling loop takes on the PlayStation, so the blanks
 * come during a load as they do there (docs/ROADMAP.md, milestone 14). Otherwise every delay is 0: a read completes at
 * once. TRADEOFF: the drive's timing only on paced runs, where it can be compared with the PlayStation's; in play a
 * load is instant.
 *
 * CRASH2_CD_LOG=FILE logs the game's calls in tools/cd_log.lua's format: a line a call, its game frame, the CPU's
 * cycles at the call and at its return (events.c's Host_Cycles), the function, its arguments and its result, a call
 * repeated alike in a game frame one line with its count after an x. */

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
#define CdlStop 0x08
#define CdlSeekL 0x15
/* The mode's double speed bit (CdlModeSpeed) */
#define MODE_SPEED 0x80
/* CdControl's and CdSync's results (libcd's CdlComplete; the status byte with the motor on) */
#define CD_SUCCESS 1
#define CD_SYNC_NO_INTERRUPT 0
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
/* The drive's timing in microseconds, as PCSX-Redux models it (src/core/cdrom.cc, at its commit 8d405b66): a command's
 * acknowledge; ReadN's start before its seek; a seek, per sector of distance and after it, for a read or for SeekL
 * (after SeekL's own start); a speed change's spin; a sector at single and at double speed; Pause while reading; Stop
 * with the motor on */
#define ACKNOWLEDGE_US 797
#define READ_START_US 20000
#define SEEK_US_PER_SECTOR 3
#define SEEK_READ_US 1000
#define SEEK_US 167000
#define SEEKL_START_US 15000
#define SPEED_CHANGE_US 650000
#define SECTOR_SINGLE_US 13333
#define SECTOR_DOUBLE_US 6666
#define PAUSE_SINGLE_US 70000
#define PAUSE_DOUBLE_US 35000
#define STOP_US 400000
#define CPU_HZ 33868800ULL
#define US_PER_SECOND 1000000ULL
/* The game's polling loop on the PlayStation, in CPU cycles: some 90 polls a sector at double speed
 * (tools/cd_log.lua) */
#define POLL_CYCLES 2500
#define LOG_VARIABLE "CRASH2_CD_LOG"
#define LOG_LINE_BYTES 160

typedef struct {
    unsigned char minute, second, sector, track;
} CdlLOC;

static FILE *s_Image;
static long s_SectorBytes;
static long s_DataOffset;
/* Setloc's target, and the sector the head is at */
static int s_Position;
static int s_Head;

/* Whether the drive takes its time (a paced run), libcd's mode, the drive's speed, and whether a seek has yet to spin
 * to a new one */
static int s_Timed;
static int s_Mode;
static int s_DoubleSpeed;
static int s_SpeedChanged;
/* When the last command's sync completes (CdSync), in the CPU's cycles */
static unsigned long long s_CommandDone;
/* The read in flight: its buffer, first sector, length, the sectors delivered and when the next one is read */
static int s_Reading;
static unsigned char *s_ReadBuffer;
static int s_ReadFirst;
static int s_ReadSectors;
static int s_ReadDelivered;
static unsigned long long s_ReadNext;

static FILE *s_Log;
/* The call held back while it repeats alike: its game frame, the cycles at the first's call and the last's return,
 * the call, its result and its count */
static int s_HeldFrame;
static unsigned long long s_HeldStart;
static unsigned long long s_HeldEnd;
static char s_HeldWhat[LOG_LINE_BYTES];
static int s_HeldResult;
static int s_HeldCount;

static int from_bcd(unsigned char value) {
    return (value >> BCD_SHIFT) * BCD_BASE + (value & BCD_LOW_MASK);
}

static unsigned char to_bcd(int value) {
    return (unsigned char)(((value / BCD_BASE) << BCD_SHIFT) | (value % BCD_BASE));
}

/* A path's last separator, '/' or Windows' '\\', or NULL */
static const char *last_separator(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');

    if (slash == NULL || (backslash != NULL && backslash > slash)) {
        return backslash;
    } else {
        return slash;
    }
}

/* The .bin a .cue names, beside it */
static void open_cue(const char *cue) {
    FILE *file = fopen(cue, "r");
    char line[PATH_BYTES];
    char bin[PATH_BYTES];
    const char *slash = last_separator(cue);
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

static void flush_log(void) {
    if (s_HeldCount == 0) {
        return;
    } else if (s_HeldCount > 1) {
        fprintf(s_Log, "%d %llu %llu %s -> %d x%d\n", s_HeldFrame, s_HeldStart, s_HeldEnd, s_HeldWhat, s_HeldResult,
                s_HeldCount);
    } else {
        fprintf(s_Log, "%d %llu %llu %s -> %d\n", s_HeldFrame, s_HeldStart, s_HeldEnd, s_HeldWhat, s_HeldResult);
    }
    s_HeldCount = 0;
}

static void close_log(void) {
    flush_log();
    fclose(s_Log);
}

/* A game's call that started at the CPU's cycle START and returned RESULT, described as FORMAT says */
static void log_call(unsigned long long start, int result, const char *format, ...) {
    char what[LOG_LINE_BYTES];
    va_list arguments;
    int frame = Host_GameFrame();

    if (s_Log == NULL) {
        return;
    } else {
        va_start(arguments, format);
        vsnprintf(what, sizeof(what), format, arguments);
        va_end(arguments);
    }
    if (s_HeldCount > 0 && frame == s_HeldFrame && result == s_HeldResult && strcmp(what, s_HeldWhat) == 0) {
        s_HeldCount++;
    } else {
        flush_log();
        s_HeldFrame = frame;
        s_HeldStart = start;
        s_HeldResult = result;
        s_HeldCount = 1;
        snprintf(s_HeldWhat, sizeof(s_HeldWhat), "%s", what);
    }
    s_HeldEnd = Host_Cycles();
}

int CdInit(void) {
    const char *log = getenv(LOG_VARIABLE);

    s_Timed = Host_GameClockPaced();
    if (log == NULL || s_Log != NULL) {
        return CD_SUCCESS;
    } else {
        s_Log = fopen(log, "wb");
    }
    if (s_Log == NULL) {
        Host_Fatal("%s: can't write %s", LOG_VARIABLE, log);
    } else {
        atexit(close_log);
    }
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

/* MICROSECONDS of the drive's time in the CPU's cycles: none when the drive takes no time */
static unsigned long long drive_cycles(unsigned long long microseconds) {
    if (s_Timed) {
        return microseconds * CPU_HZ / US_PER_SECOND;
    } else {
        return 0;
    }
}

/* A seek from sector FROM to TO, AFTER_US once there, and the spin to a new speed if one is due */
static unsigned long long seek_cycles(int from, int to, unsigned long long after_us) {
    unsigned long long microseconds = (unsigned long long)abs(to - from) * SEEK_US_PER_SECTOR + after_us;

    if (s_SpeedChanged) {
        s_SpeedChanged = 0;
        microseconds += SPEED_CHANGE_US;
    } else {
        /* At the speed already */
    }
    return drive_cycles(microseconds);
}

static unsigned long long sector_cycles(void) {
    return drive_cycles(s_DoubleSpeed ? SECTOR_DOUBLE_US : SECTOR_SINGLE_US);
}

/* The sectors read by now, into the read's buffer; after the last, libcd's Pause */
static void catch_up(void) {
    unsigned long long now = Host_Cycles();

    while (s_Reading && s_ReadNext <= now) {
        read_sectors(s_ReadFirst + s_ReadDelivered, 1, s_ReadBuffer + s_ReadDelivered * SECTOR_DATA_BYTES);
        s_ReadDelivered++;
        s_Head = s_ReadFirst + s_ReadDelivered;
        if (s_ReadDelivered == s_ReadSectors) {
            s_Reading = 0;
            s_CommandDone =
                s_ReadNext + drive_cycles(ACKNOWLEDGE_US + (s_DoubleSpeed ? PAUSE_DOUBLE_US : PAUSE_SINGLE_US));
        } else {
            s_ReadNext += sector_cycles();
        }
    }
}

void Host_CdVerticalBlank(void) {
    catch_up();
}

/* The CPU waits on the drive until its cycle CYCLE, the sectors and blanks on the way delivered */
static void wait_until(unsigned long long cycle) {
    unsigned long long now = Host_Cycles();

    if (now < cycle) {
        Host_Wait(cycle - now);
    } else {
        /* Already there */
    }
    catch_up();
}

/* A poll that found the drive busy: the game's loop goes round before the next */
static void poll(void) {
    if (s_Timed) {
        Host_Wait(POLL_CYCLES);
    } else {
        /* No time passes */
    }
}

/* libcd sends a command once the last one has completed: the cycle it goes */
static unsigned long long issue(void) {
    catch_up();
    wait_until(s_CommandDone);
    return Host_Cycles();
}

/* Setloc sets the position, SeekL moves the head there; the others take their acknowledge, Stop its spin down too */
static int control(unsigned char command, const unsigned char *parameter, unsigned char *result, int acknowledged) {
    unsigned long long now = issue();

    switch (command) {
    case CdlSetloc:
        s_Position = position_sector((const CdlLOC *)parameter);
        s_CommandDone = now + drive_cycles(ACKNOWLEDGE_US);
        break;
    case CdlSeekL:
        s_Position = position_sector((const CdlLOC *)parameter);
        s_CommandDone = now + drive_cycles(ACKNOWLEDGE_US + SEEKL_START_US) + seek_cycles(s_Head, s_Position, SEEK_US);
        s_Head = s_Position;
        break;
    case CdlStop:
        s_CommandDone = now + drive_cycles(ACKNOWLEDGE_US + STOP_US);
        break;
    default:
        s_CommandDone = now + drive_cycles(ACKNOWLEDGE_US);
        break;
    }
    if (acknowledged) {
        wait_until(now + drive_cycles(ACKNOWLEDGE_US));
    } else {
        /* CdControlF returns at once */
    }
    if (result != NULL) {
        result[0] = CD_STATUS_STANDBY;
    } else {
        /* The caller wants no status */
    }
    return CD_SUCCESS;
}

/* A position's sector for the log, -1 without one */
static int logged_position(unsigned char command, const unsigned char *parameter) {
    if (parameter != NULL && (command == CdlSetloc || command == CdlSeekL)) {
        return position_sector((const CdlLOC *)parameter);
    } else {
        return -1;
    }
}

static void log_control(const char *name, unsigned long long start, unsigned char command,
                        const unsigned char *parameter, int done) {
    int position = logged_position(command, parameter);

    if (position >= 0) {
        log_call(start, done, "%s %x %d", name, command, position);
    } else {
        log_call(start, done, "%s %x", name, command);
    }
}

int CdControl(unsigned char command, unsigned char *parameter, unsigned char *result) {
    unsigned long long start = Host_Cycles();
    int done = control(command, parameter, result, 1);

    log_control("CdControl", start, command, parameter, done);
    return done;
}

int CdControlF(unsigned char command, unsigned char *parameter) {
    unsigned long long start = Host_Cycles();
    int done = control(command, parameter, NULL, 0);

    log_control("CdControlF", start, command, parameter, done);
    return done;
}

/* SECTORS sectors' user data from the position into BUFFER at mode's speed: libcd's Setmode when the mode changes, its
 * acknowledge waited for, then ReadN, whose sectors come in as catch_up reads them */
int CdRead(int sectors, unsigned long *buffer, int mode) {
    unsigned long long start = Host_Cycles();
    int doubled = (mode & MODE_SPEED) != 0;
    unsigned long long now = issue();

    if (mode == s_Mode) {
        /* No Setmode */
    } else if (doubled != s_DoubleSpeed) {
        s_Mode = mode;
        s_DoubleSpeed = doubled;
        s_SpeedChanged = 1;
        wait_until(now + drive_cycles(ACKNOWLEDGE_US));
    } else {
        s_Mode = mode;
        wait_until(now + drive_cycles(ACKNOWLEDGE_US));
    }
    now = Host_Cycles();
    s_ReadBuffer = (unsigned char *)buffer;
    s_ReadFirst = s_Position;
    s_ReadSectors = sectors;
    s_ReadDelivered = 0;
    s_Reading = sectors > 0;
    s_ReadNext = now + drive_cycles(ACKNOWLEDGE_US + READ_START_US) + seek_cycles(s_Head, s_Position, SEEK_READ_US) +
                 sector_cycles();
    s_CommandDone = now + drive_cycles(ACKNOWLEDGE_US);
    s_Position += sectors;
    catch_up();
    log_call(start, CD_SUCCESS, "CdRead %d %x", sectors, mode);
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

static void standby(unsigned char *result) {
    if (result != NULL) {
        result[0] = CD_STATUS_STANDBY;
    } else {
        /* The caller wants no status */
    }
}

/* The read's sectors still to come: CdReadSync(0) waits for them, CdReadSync(1) tells them */
int CdReadSync(int mode, unsigned char *result) {
    unsigned long long start = Host_Cycles();
    int left;

    catch_up();
    if (mode == 0) {
        while (s_Reading) {
            wait_until(s_ReadNext);
        }
    } else if (s_Reading) {
        left = s_ReadSectors - s_ReadDelivered;
        poll();
        standby(result);
        log_call(start, left, "CdReadSync %d", mode);
        return left;
    } else {
        /* Done */
    }
    standby(result);
    log_call(start, CD_READ_DONE, "CdReadSync %d", mode);
    return CD_READ_DONE;
}

/* The last command's sync: CdSync(0) waits for it, CdSync(1) answers CdlNoIntr while it runs */
int CdSync(int mode, unsigned char *result) {
    unsigned long long start = Host_Cycles();
    int done = CD_SYNC_COMPLETE;

    catch_up();
    if (mode == 0) {
        wait_until(s_CommandDone);
    } else if (Host_Cycles() < s_CommandDone) {
        poll();
        done = CD_SYNC_NO_INTERRUPT;
    } else {
        /* Complete */
    }
    standby(result);
    log_call(start, done, "CdSync %d", mode);
    return done;
}

/* The read's data in memory: CdDataSync(0) waits for it, CdDataSync(1) answers 1 while it comes */
int CdDataSync(int mode) {
    unsigned long long start = Host_Cycles();
    int busy = 0;

    catch_up();
    if (mode == 0) {
        while (s_Reading) {
            wait_until(s_ReadNext);
        }
    } else if (s_Reading) {
        poll();
        busy = 1;
    } else {
        /* Done */
    }
    log_call(start, busy, "CdDataSync %d", mode);
    return busy;
}
