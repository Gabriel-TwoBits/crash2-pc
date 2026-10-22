#ifndef RETAIL_H
#define RETAIL_H

/* The retail executable's layout (decomp/config/SCUS_941.54.yaml): its read-only data and code from RETAIL_CODE_START,
 * its data from RETAIL_DATA_START, its bss to RETAIL_BSS_END. The port's copy of the data and bss keeps their layout
 * from g_EidAlphabet, the data's first symbol (tools/check_layout.py) */
#define RETAIL_CODE_START 0x80010000u
#define RETAIL_DATA_START 0x8005B198u
#define RETAIL_BSS_END 0x8006F1F0u

extern char g_EidAlphabet[];

#endif /* RETAIL_H */
