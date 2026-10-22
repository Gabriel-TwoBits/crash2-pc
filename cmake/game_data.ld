/* The game's data in the retail image's order and alignment: the split's data, each game file's small data (each
 * object 4-aligned, as the PlayStation's GCC 2.7.2 placed them, its -fdata-sections one section each), and the bss in
 * .data (tools/host_data.py). Inserted before the host's own .data; tools/check_layout.py checks the result */
SECTIONS
{
    .game_data ALIGN(8) : SUBALIGN(4)
    {
        *game.dir/*(.data .data.*)
    }
}
INSERT BEFORE .data;

/* The rest of the PlayStation's RAM window, to 0x80800000 (src/host.h), held by the program: the C library's
 * allocations start past it, and main.c makes it unreadable, so that the game's access past its image faults */
SECTIONS
{
    .window_rest (NOLOAD) : ALIGN(0x1000)
    {
        __window_rest_start = ABSOLUTE(.);
        . += 0x80800000 - __window_rest_start;
        __window_rest_end = ABSOLUTE(.);
    }
}
INSERT AFTER .bss;
