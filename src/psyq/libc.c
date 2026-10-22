#include <string.h>

/* The PlayStation C library's functions the game calls that the host's lacks: BSD's bzero and bcopy, which glibc has
 * and Windows' C runtime hasn't */
#ifdef _WIN32
void bzero(void *buffer, size_t bytes) {
    memset(buffer, 0, bytes);
}

void bcopy(const void *source, void *destination, size_t bytes) {
    memmove(destination, source, bytes);
}
#endif
