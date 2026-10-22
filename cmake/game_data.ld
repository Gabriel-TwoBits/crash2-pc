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
