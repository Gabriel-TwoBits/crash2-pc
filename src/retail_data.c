#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#elif !defined(__wasi__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "host_log.h"
#include "retail.h"
#include "retail_data.h"

/* The program holds none of the game's data: each run of its numbers is zeros at a label, which tools/host_data.py's
 * retail_bytes.s lists with its retail address and its bytes */
typedef struct {
    uint8_t *to;
    uint32_t address;
    uint32_t bytes;
} RetailRun;

extern const RetailRun g_RetailWritable[];
extern const uint32_t g_RetailWritableCount;
extern const RetailRun g_RetailReadOnly[];
extern const uint32_t g_RetailReadOnlyCount;

static void copy(const RetailRun *run, const uint8_t *image, uint32_t bytes) {
    uint32_t offset = run->address - RETAIL_CODE_START;

    if (run->address < RETAIL_CODE_START || offset > bytes || run->bytes > bytes - offset) {
        Host_Fatal("the executable has no %u bytes at 0x%08X for the game's data", run->bytes, run->address);
    } else {
        memcpy(run->to, image + offset, run->bytes);
    }
}

/* A read-only run's pages writable for its copy, then as they were */
static void copy_read_only(const RetailRun *run, const uint8_t *image, uint32_t bytes) {
#if defined(__wasi__)
    /* Spike: a module's memory has no protections */
    copy(run, image, bytes);
#elif defined(_WIN32)
    DWORD old;
    DWORD unused;

    if (!VirtualProtect(run->to, run->bytes, PAGE_READWRITE, &old)) {
        Host_Fatal("can't write the game's read-only data at %p", (void *)run->to);
    } else {
        copy(run, image, bytes);
    }
    if (!VirtualProtect(run->to, run->bytes, old, &unused)) {
        Host_Fatal("can't protect the game's read-only data at %p again", (void *)run->to);
    } else {
        /* As it was */
    }
#else
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)run->to & ~(page - 1);
    size_t length = (uintptr_t)run->to + run->bytes - start;

    if (mprotect((void *)start, length, PROT_READ | PROT_WRITE) != 0) {
        Host_Fatal("can't write the game's read-only data at %p", (void *)run->to);
    } else {
        copy(run, image, bytes);
    }
    /* The program's read-only segment holds no code (ld's separate code): read-only is what it was */
    if (mprotect((void *)start, length, PROT_READ) != 0) {
        Host_Fatal("can't protect the game's read-only data at %p again", (void *)run->to);
    } else {
        /* As it was */
    }
#endif
}

void Host_FillRetailData(const uint8_t *image, uint32_t bytes) {
    uint32_t index;

    for (index = 0; index < g_RetailWritableCount; index++) {
        copy(&g_RetailWritable[index], image, bytes);
    }
    for (index = 0; index < g_RetailReadOnlyCount; index++) {
        copy_read_only(&g_RetailReadOnly[index], image, bytes);
    }
}
