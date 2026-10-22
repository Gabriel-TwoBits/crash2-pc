/* cmake/game_data.ld for 32-bit Windows' PE: the game's data in the retail image's order and alignment, before the
 * host's .data (each game file's small data in its own .data$NAME section, -fdata-sections' COFF names), and the rest
 * of the PlayStation's RAM window, to 0x80800000, held by the library, which main.c makes unreadable. The C's names
 * take a leading underscore here. tools/check_layout.py checks the result */
SECTIONS
{
    /* A PE section starts on a page (SectionAlignment), which keeps the retail data's 8-byte alignment */
    .gdata ALIGN(0x1000) : SUBALIGN(4)
    {
        *game.dir/*(.data .data$*)
    }
}
INSERT BEFORE .data;

SECTIONS
{
    .wrest (NOLOAD) : ALIGN(0x1000)
    {
        /* Symbols relative to the section, which COFF holds; 0x80800000 is 8 MB-aligned */
        ___window_rest_start = .;
        . = ALIGN(0x800000);
        ___window_rest_end = .;
    }
}
INSERT AFTER .bss;
