#ifndef RETAIL_DATA_H
#define RETAIL_DATA_H

#include <stdint.h>

/* The game's data the build leaves as zeros (tools/host_data.py), copied from the player's executable: image is its
 * image from RETAIL_CODE_START, bytes long. Before the game's first instruction */
void Host_FillRetailData(const uint8_t *image, uint32_t bytes);

#endif /* RETAIL_DATA_H */
