#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc.h"
#include "host_log.h"
#include "preferences.h"

/* The disc image: the first argument, else CRASH2_DISC. Else, with a display, the image chosen last (its path in
 * disc.txt in the preferences) if it is still there, or one the player chooses in the system's file dialog (SDL3's:
 * the desktop's portal on Linux, the common dialog on Windows), which disc.txt then remembers. Headless, a disc must
 * be named; a dialog closed without one ends the program */

#define DISC_VARIABLE "CRASH2_DISC"
#define REMEMBERED_FILE "disc.txt"
#define PATH_BYTES 4096
#define WAIT_MS 10
#define NOT_CHOSEN 0
#define CHOSEN 1
#define CANCELLED 2
#define FAILED 3
#define TOO_LONG 4

static char s_Path[PATH_BYTES];
static SDL_AtomicInt s_Choice;

static const SDL_DialogFileFilter s_Filters[] = {
    {"Disc images (.cue, .iso)", "cue;iso"},
};

/* The dialog's answer, maybe from another thread: the first file, none (cancelled), a file whose path s_Path can't
 * hold, or no list (the dialog failed: its error in s_Path) */
static void on_chosen(void *userdata, const char *const *files, int filter) {
    (void)userdata;
    (void)filter;
    if (files == NULL) {
        snprintf(s_Path, sizeof(s_Path), "%s", SDL_GetError());
        SDL_SetAtomicInt(&s_Choice, FAILED);
    } else if (files[0] == NULL) {
        SDL_SetAtomicInt(&s_Choice, CANCELLED);
    } else if (strlen(files[0]) >= sizeof(s_Path)) {
        SDL_SetAtomicInt(&s_Choice, TOO_LONG);
    } else {
        strcpy(s_Path, files[0]);
        SDL_SetAtomicInt(&s_Choice, CHOSEN);
    }
}

/* The path the remembered file holds, if the image is there: s_Path, else NULL */
static const char *remembered(const char *remember) {
    FILE *file = fopen(remember, "r");
    FILE *image;

    if (file == NULL) {
        return NULL;
    } else if (fgets(s_Path, sizeof(s_Path), file) == NULL) {
        fclose(file);
        return NULL;
    } else {
        fclose(file);
        s_Path[strcspn(s_Path, "\r\n")] = '\0';
        image = fopen(s_Path, "rb");
    }
    if (image == NULL) {
        return NULL;
    } else {
        fclose(image);
        return s_Path;
    }
}

static void remember(const char *remember) {
    FILE *file = fopen(remember, "w");

    if (file == NULL) {
        Host_Log("can't remember the disc in %s", remember);
    } else {
        fprintf(file, "%s\n", s_Path);
        fclose(file);
    }
}

const char *Host_ChooseDisc(int argc, char **argv) {
    char remember_path[PATH_BYTES];
    int have_remember = Host_PreferencePath(REMEMBERED_FILE, remember_path, sizeof(remember_path)) == 0;
    const char *disc;

    if (argc > 1) {
        return argv[1];
    } else if ((disc = getenv(DISC_VARIABLE)) != NULL) {
        return disc;
    } else if (Host_Headless()) {
        Host_Fatal("usage: crash2 DISC.cue (or DISC.iso), or " DISC_VARIABLE " set to it");
    } else if (have_remember && (disc = remembered(remember_path)) != NULL) {
        return disc;
    } else if (!SDL_Init(SDL_INIT_VIDEO)) {
        Host_Fatal("no disc image named, and no display to choose one (%s)", SDL_GetError());
    } else {
        SDL_ShowOpenFileDialog(on_chosen, NULL, NULL, s_Filters, SDL_arraysize(s_Filters), NULL, false);
    }
    while (SDL_GetAtomicInt(&s_Choice) == NOT_CHOSEN) {
        SDL_PumpEvents();
        SDL_Delay(WAIT_MS);
    }
    if (SDL_GetAtomicInt(&s_Choice) == FAILED) {
        Host_Fatal("no file dialog to choose the disc image (%s): name it as the program's argument", s_Path);
    } else if (SDL_GetAtomicInt(&s_Choice) == TOO_LONG) {
        Host_Fatal("the disc image's path is longer than %d bytes: name it as the program's argument", PATH_BYTES - 1);
    } else if (SDL_GetAtomicInt(&s_Choice) == CANCELLED) {
        Host_Log("no disc image chosen");
        exit(EXIT_SUCCESS);
    } else if (have_remember) {
        remember(remember_path);
    } else {
        /* No folder to remember it in */
    }
    return s_Path;
}
