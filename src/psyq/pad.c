#include <stddef.h>

#include "psyq/host_psyq.h"

/* libapi's pad: the two buffers InitPAD gets, which the BIOS fills at each vertical blank. Port 1 holds a digital pad
 * (status 0, its kind and size 0x41, then its buttons, a bit each, 0 while pressed); port 2 holds none (status 0xFF).
 * TRADEOFF: no button is ever pressed; the host's gamepads and keyboard are milestone 7 */

#define PAD_CONNECTED 0x00
#define PAD_NONE 0xFF
#define PAD_DIGITAL 0x41
#define BUTTONS_RELEASED 0xFF
#define PAD_STATUS 0
#define PAD_KIND 1
#define PAD_BUTTONS_LOW 2
#define PAD_BUTTONS_HIGH 3
#define PAD_DIGITAL_BYTES 4

static unsigned char *s_Port1;
static unsigned char *s_Port2;
static int s_Port1Bytes;
static int s_Port2Bytes;
static int s_Started;

int InitPAD(unsigned char *port1, int port1_bytes, unsigned char *port2, int port2_bytes) {
    s_Port1 = port1;
    s_Port1Bytes = port1_bytes;
    s_Port2 = port2;
    s_Port2Bytes = port2_bytes;
    return 1;
}

int StartPAD(void) {
    s_Started = 1;
    Host_PadVerticalBlank();
    return 1;
}

void StopPAD(void) {
    s_Started = 0;
}

void ChangeClearPAD(int mode) {
    (void)mode;
}

void Host_PadVerticalBlank(void) {
    if (!s_Started || s_Port1 == NULL || s_Port1Bytes < PAD_DIGITAL_BYTES) {
        return;
    } else {
        s_Port1[PAD_STATUS] = PAD_CONNECTED;
        s_Port1[PAD_KIND] = PAD_DIGITAL;
        s_Port1[PAD_BUTTONS_LOW] = BUTTONS_RELEASED;
        s_Port1[PAD_BUTTONS_HIGH] = BUTTONS_RELEASED;
    }
    if (s_Port2 != NULL && s_Port2Bytes > 0) {
        s_Port2[PAD_STATUS] = PAD_NONE;
    } else {
        /* No second buffer */
    }
}
