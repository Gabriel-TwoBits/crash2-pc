#include "host_log.h"

/* The BIOS's file calls the game makes on the memory card ("bu00:"), which share their names with the C library's: the
 * link wraps the game's calls to them (CMakeLists.txt, --wrap) to these.
 * TRADEOFF: no card yet, every call failing; the card in files is docs/ROADMAP.md's milestone 7 */
#define BIOS_FAILURE -1

int __wrap_open(const char *path, int mode) {
    Host_Log("open(\"%s\", 0x%X): no memory card yet", path, mode);
    return BIOS_FAILURE;
}

int __wrap_read(int fd, void *buffer, int bytes) {
    (void)buffer;
    Host_Log("read(%d, %d bytes): no memory card yet", fd, bytes);
    return BIOS_FAILURE;
}

int __wrap_write(int fd, const void *buffer, int bytes) {
    (void)buffer;
    Host_Log("write(%d, %d bytes): no memory card yet", fd, bytes);
    return BIOS_FAILURE;
}

int __wrap_close(int fd) {
    Host_Log("close(%d): no memory card yet", fd);
    return BIOS_FAILURE;
}
