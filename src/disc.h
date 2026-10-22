#ifndef DISC_H
#define DISC_H

/* The disc image to play (disc.c): the first argument, else CRASH2_DISC, else the one chosen last, else one the
 * player chooses in the system's file dialog, remembered */
const char *Host_ChooseDisc(int argc, char **argv);

#endif /* DISC_H */
