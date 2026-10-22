#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gool.h"
#include "host.h"
#include "host_log.h"
#include "menus.h"
#include "render.h"
#include "settings.h"
#include "video.h"

/* The options screen, in the game's own menus. The title's menu (TitOC, state 30) gains a third item, OPTIONS, and
 * the pause menu's options (DispC, state 15) one before DONE, PC OPTIONS: either opens the port's settings
 * (settings.h) in the menu's place, a row each showing its value and DONE, in the menu's font. The menu's own code
 * moves its cursor over the rows, with its sounds; the host takes the choice (X) and the sides (left and right),
 * which change a row's value. The menus' GOOL code is patched where the game loads it, a word at a time, each word
 * one of the host's operations (decomp's GOOL_HOST_OPERATION) that does the word's own work besides: an entry is
 * patched only if each of those words holds the instruction it replaces */

/* The host's operations' opcode, past the game's 79 (decomp's 80039D2C.c), the operation in its low bits */
#define HOST_OPCODE 0xFFu
#define OPCODE_SHIFT 24
#define OPERATION_MASK 0xFFFFFFu
#define HOST_WORD(operation) ((HOST_OPCODE << OPCODE_SHIFT) | (uint32_t)(operation))

enum {
    TITLE_ITEMS = 1,
    TITLE_CHOICE,
    TITLE_SIDES,
    TITLE_TEXTS,
    PAUSE_ITEM,
    PAUSE_CHOICE,
    PAUSE_SIDES,
    PAUSE_REFRESH
};

/* A menu's items step by 256 in its cursor and count, as the GOOL code's fixed point has them */
#define ITEM 256
/* The registers the menus keep their items in: the cursor, the count, the text a row is spawned with, the pause
 * options' CTR row */
#define CURSOR 41
#define ITEM_COUNT 61
#define TEXT_FRAME 57
#define CENTRE_ROW 63
/* A line's height between a menu's rows, in the texts' units */
#define LINE 4096

/* TitOC's code: the menu's items set at the state's start (MOVE r61 <- 512), the choice pushed (PUSH r41), the PAL
 * menu's language item tested (EQUAL 512, r41), the texts' subroutine's end (RETURN); the branch past the choice,
 * which drops it; the menu's text animation (NEW GAME, LOAD GAME) */
#define TITLE_NAME "TitOC"
#define TITLE_CODE_WORDS 1825
#define TITLE_ITEMS_WORD 1705
#define TITLE_CHOICE_WORD 1750
#define TITLE_SIDES_WORD 1767
#define TITLE_TEXTS_WORD 1823
#define TITLE_CHOSEN_WORD 1766
#define TITLE_ANIMATION_WORD 330
#define TITLE_OPTIONS_ITEM 2
#define TITLE_ITEM_COUNT 3
/* The rows from half a line above NEW GAME's, clear of the logo, as many as fit below it */
#define TITLE_FIRST_ROW_Y (LINE / 2)
#define TITLE_VISIBLE_ROWS 4

/* DispC's code as the warp rooms and levels load it (the title's, its pause menu unused, has it 94 words further on):
 * DONE's text set (MOVE r57 <- DONE), the choice's branch (BRANCH_IF r41), the CTR row's sides tested (EQUAL r63,
 * r41), its rows' refresh tested (AND 32, status_a); where the choices go, the word after them, the pause menu's own
 * GO_TO_STATE 15 (the options started again); the menus' text animation */
#define PAUSE_NAME "DispC"
#define PAUSE_CODE_WORDS 2958
#define PAUSE_ITEM_WORD 1201
#define PAUSE_CHOICE_WORD 1269
#define PAUSE_SIDES_WORD 1293
#define PAUSE_REFRESH_WORD 1311
#define PAUSE_CHOICES_WORD 1274
#define PAUSE_CHOSEN_WORD 1293
#define PAUSE_REOPEN_WORD 1090
#define PAUSE_ANIMATION_WORD 739
/* DONE's text, string 50 of the menus' animation; the status bit with which the menu leaves its rows' refresh */
#define DONE_FRAME (50 * ITEM)
#define REFRESH_STATUS 0x20
/* The pause options' rows laid out as its own (item 2 at the menu's height), as many as the panel holds; the port's
 * across all of the panel, the picture of Crash's face too, for their names and values. Not above the panel: the game
 * leaves the paused picture as it is, and a text drawn there stays after it */
#define PAUSE_MIDDLE_ROW 2
#define PAUSE_FIRST_ROW_Y (PAUSE_MIDDLE_ROW * LINE)
#define PAUSE_ROWS_MIDDLE (LINE * 13 / 4)
#define PAUSE_VISIBLE_ROWS 5


/* DispC's text object (SPAWN_FORCED, subtype 19, 6 arguments: item, animation, frame, across, height, 0), the event a
 * menu's texts end on, the title's tests of left and right pressed */
#define SPAWN_TEXT 0x466044C1u
#define TEXT_END_EVENT 0xC00
#define LEFT_PRESSED 0x64000u
#define RIGHT_PRESSED 0x24000u
/* Triangle or circle pressed, which close the options as DONE does (the menus' X is 0x81040: the pad's buttons with
 * their two bytes swapped, triangle 0x10 and circle 0x20) */
#define BACK_PRESSED 0x81030u

/* A text animation (type 4): its header, the strings' count in it, then the strings */
#define ANIMATION_HEADER_BYTES 12
#define ANIMATION_COUNT_OFFSET 2
#define TEXT_BYTES 48
/* A row's name and value between the font's arrows, as the pause options' CTR */
#define ROW_FORMAT "~sx800~<~sx400~%s: %s~sx800~>"
#define ANIMATION_WORDS ((ANIMATION_HEADER_BYTES + TEXT_BYTES) / WORD_BYTES)
#define ROWS_MAX (SETTING_COUNT + 1)
/* Room for an event's arguments, none here (the game's events take up to 7) */
#define EVENT_ARGUMENTS 8

typedef struct {
    int word;
    uint32_t original;
    int operation;
} Patch;

typedef struct {
    const char *name;
    int code_words;
    const Patch *patches;
    int count;
    int refused; /* logged once as not the code patched */
} PatchedEntry;

static const Patch s_TitlePatches[] = {
    {TITLE_ITEMS_WORD, 0x11802E3Du, TITLE_ITEMS},
    {TITLE_CHOICE_WORD, 0x16BE0E29u, TITLE_CHOICE},
    {TITLE_SIDES_WORD, 0x04802E29u, TITLE_SIDES},
    {TITLE_TEXTS_WORD, 0x31894000u, TITLE_TEXTS},
};

static const Patch s_PausePatches[] = {
    {PAUSE_ITEM_WORD, 0x11832E39u, PAUSE_ITEM},
    {PAUSE_CHOICE_WORD, 0x331A4004u, PAUSE_CHOICE},
    {PAUSE_SIDES_WORD, 0x04E3FE29u, PAUSE_SIDES},
    {PAUSE_REFRESH_WORD, 0x07A02E1Au, PAUSE_REFRESH},
};

#define PATCH_COUNT(patches) ((int)(sizeof(patches) / sizeof((patches)[0])))

static PatchedEntry s_Entries[] = {
    {TITLE_NAME, TITLE_CODE_WORDS, s_TitlePatches, PATCH_COUNT(s_TitlePatches), 0},
    {PAUSE_NAME, PAUSE_CODE_WORDS, s_PausePatches, PATCH_COUNT(s_PausePatches), 0},
};

/* The game's operations and events (decomp's 80038700.c, 8001CEDC.c, 80036E30.c) */
void Gool_OpSpawn(void *object, uint32_t word);
int32_t Gool_CascadeEvent(int32_t object, int32_t root, int32_t event, int32_t link, int32_t count, int32_t arguments);
int32_t Gool_OpTestPad(uint32_t test, int32_t pad);

/* The texts the host spawns, each one string; the menus' header copied in */
typedef struct {
    uint32_t words[ANIMATION_WORDS];
} TextAnimation;

static TextAnimation s_TitleOptions;
static TextAnimation s_PauseOptions;
static TextAnimation s_Rows[ROWS_MAX];
/* The settings the rows show, those this run has a use for, then DONE */
static int s_RowSettings[ROWS_MAX];
static int s_RowCount;

/* The menu the options screen is open in (NULL when closed), its kind (TITLE_ITEMS or PAUSE_ITEM) */
static void *s_Menu;
static int s_MenuKind;
/* The first of the rows shown */
static int s_FirstRow;
/* PC OPTIONS's item in the pause options, and whether their next start (from the options screen) puts the cursor
 * back on it */
static int32_t s_PauseItem = -1;
static int s_BackToPauseItem;

static void push(void *object, int32_t value) {
    int32_t *sp = (int32_t *)(uintptr_t)GOOL_WORD(object, OBJECT_SP);

    *sp = value;
    GOOL_WORD(object, OBJECT_SP) = (int32_t)(uintptr_t)(sp + 1);
}

/* The object's code on from a word of its entry's */
static void jump(void *object, int word) {
    void *exe = (void *)(uintptr_t)GOOL_WORD(object, OBJECT_EXE);

    GOOL_WORD(object, OBJECT_PC) = (int32_t)(uintptr_t)(GOOL_ITEM(exe, ENTRY_CODE_ITEM) + word);
}

/* A text animation of one string, its header the menu's (at word of the object's animations) */
static void write_text(TextAnimation *animation, void *object, int word, const char *text) {
    void *exe = (void *)(uintptr_t)GOOL_WORD(object, OBJECT_EXE);
    uint16_t one = 1;

    memset(animation, 0, sizeof(*animation));
    memcpy(animation->words, GOOL_ITEM(exe, ENTRY_ANIMATIONS_ITEM) + word, ANIMATION_HEADER_BYTES);
    memcpy((unsigned char *)animation->words + ANIMATION_COUNT_OFFSET, &one, sizeof(one));
    snprintf((char *)animation->words + ANIMATION_HEADER_BYTES, TEXT_BYTES, "%s", text);
}

/* A text the menu's own code would spawn: its item, its animation's frame, its height */
static void spawn_text(void *object, int32_t item, const void *animation, int32_t frame, int32_t across,
                       int32_t height) {
    push(object, item);
    push(object, (int32_t)(uintptr_t)animation);
    push(object, frame);
    push(object, across);
    push(object, height);
    push(object, 0);
    Gool_OpSpawn(object, SPAWN_TEXT);
}

static void end_texts(void *object) {
    int32_t arguments[EVENT_ARGUMENTS];

    Gool_CascadeEvent((int32_t)(uintptr_t)object, (int32_t)(uintptr_t)object, TEXT_END_EVENT, 0, 0,
                      (int32_t)(uintptr_t)arguments);
}

static int animation_word(void) {
    return s_MenuKind == TITLE_ITEMS ? TITLE_ANIMATION_WORD : PAUSE_ANIMATION_WORD;
}

static int done_row(void) {
    return s_RowCount - 1;
}

/* Whether the options show a setting: the GPU renderer's only with it there to draw */
static int shown(int setting) {
    switch (setting) {
    case SETTING_RESOLUTION:
    case SETTING_STEADY:
    case SETTING_PERSPECTIVE:
        return Render_Active();
    default:
        return 1;
    }
}

static void write_row(void *object, int row) {
    char text[TEXT_BYTES];

    if (row == done_row()) {
        snprintf(text, sizeof(text), "DONE");
    } else {
        const Setting *setting = &g_Settings[s_RowSettings[row]];

        snprintf(text, sizeof(text), ROW_FORMAT, setting->label, setting->values[setting->value]);
    }
    write_text(&s_Rows[row], object, animation_word(), text);
}

/* A shown row's height by its place among the shown, and the rows' middle across */
static int32_t row_height(int slot) {
    if (s_MenuKind == TITLE_ITEMS) {
        return TITLE_FIRST_ROW_Y - slot * LINE;
    } else {
        return PAUSE_FIRST_ROW_Y - slot * LINE;
    }
}

static int32_t rows_middle(void) {
    if (s_MenuKind == TITLE_ITEMS) {
        return 0;
    } else {
        return PAUSE_ROWS_MIDDLE;
    }
}

static int visible_rows(void) {
    if (s_MenuKind == TITLE_ITEMS) {
        return TITLE_VISIBLE_ROWS;
    } else {
        return PAUSE_VISIBLE_ROWS;
    }
}

/* The settings' rows shown at once: the menu's room less DONE's, which stays in view below them */
static int visible_settings(void) {
    return visible_rows() - 1;
}

/* The settings' rows from the first shown, as many as there is room for, then DONE: each a text of its own item, which
 * the cursor lights as it does the menu's */
static void show_rows(void *object) {
    int slot;

    end_texts(object);
    for (slot = 0; slot < visible_settings() && s_FirstRow + slot < done_row(); slot++) {
        int row = s_FirstRow + slot;

        spawn_text(object, row * ITEM, &s_Rows[row], 0, rows_middle(), row_height(slot));
    }
    spawn_text(object, done_row() * ITEM, &s_Rows[done_row()], 0, rows_middle(), row_height(slot));
}

/* The settings' rows shown moved to keep the cursor's in view (DONE always is) */
static void follow_cursor(void *object) {
    int row = GOOL_REGISTER(object, CURSOR) / ITEM;

    if (row == done_row()) {
        return;
    } else if (row < s_FirstRow) {
        s_FirstRow = row;
        show_rows(object);
    } else if (row >= s_FirstRow + visible_settings()) {
        s_FirstRow = row - visible_settings() + 1;
        show_rows(object);
    } else {
        /* In view */
    }
}

static void open_options(void *object, int kind) {
    int setting;
    int row;

    s_Menu = object;
    s_MenuKind = kind;
    s_FirstRow = 0;
    s_RowCount = 0;
    for (setting = 0; setting < SETTING_COUNT; setting++) {
        if (shown(setting)) {
            s_RowSettings[s_RowCount++] = setting;
        } else {
            continue;
        }
    }
    s_RowCount++;
    for (row = 0; row < s_RowCount; row++) {
        write_row(object, row);
    }
    show_rows(object);
    GOOL_REGISTER(object, CURSOR) = 0;
    GOOL_REGISTER(object, ITEM_COUNT) = s_RowCount * ITEM;
}

/* A setting's value moved by step (1 or -1), round its values, and shown */
static void change(void *object, int row, int step) {
    Setting *setting = &g_Settings[s_RowSettings[row]];

    setting->value = (setting->value + step + setting->count) % setting->count;
    write_row(object, row);
    Host_ApplySettings();
    Host_SaveSettings();
}

/* The cursor's row's value moved by the side pressed */
static void sides(void *object) {
    int row = GOOL_REGISTER(object, CURSOR) / ITEM;

    if (row < 0 || row >= done_row()) {
        return;
    } else if (Gool_OpTestPad(LEFT_PRESSED, 0)) {
        change(object, row, -1);
    } else if (Gool_OpTestPad(RIGHT_PRESSED, 0)) {
        change(object, row, 1);
    } else {
        /* Neither side */
    }
}

static void spawn_title_options(void *object) {
    write_text(&s_TitleOptions, object, TITLE_ANIMATION_WORD, "OPTIONS");
    spawn_text(object, TITLE_OPTIONS_ITEM * ITEM, &s_TitleOptions, 0, 0, -TITLE_OPTIONS_ITEM * LINE);
}

/* The title menu's texts, as its own code spawns NEW GAME and LOAD GAME, and OPTIONS */
static void spawn_title_texts(void *object) {
    void *exe = (void *)(uintptr_t)GOOL_WORD(object, OBJECT_EXE);
    const uint32_t *animation = GOOL_ITEM(exe, ENTRY_ANIMATIONS_ITEM) + TITLE_ANIMATION_WORD;
    int item;

    for (item = 0; item < TITLE_OPTIONS_ITEM; item++) {
        spawn_text(object, item * ITEM, animation, item * ITEM, 0, -item * LINE);
    }
    spawn_title_options(object);
}

/* The options screen closed: the menu as it was, its cursor on the item that opened it */
static void close_options(void *object) {
    s_Menu = NULL;
    if (s_MenuKind == TITLE_ITEMS) {
        end_texts(object);
        spawn_title_texts(object);
        GOOL_REGISTER(object, ITEM_COUNT) = TITLE_ITEM_COUNT * ITEM;
        GOOL_REGISTER(object, CURSOR) = TITLE_OPTIONS_ITEM * ITEM;
    } else {
        /* The pause options started again, which ends the rows */
        s_BackToPauseItem = 1;
        jump(object, PAUSE_REOPEN_WORD);
    }
}

/* X on a row: DONE closes the screen, another row's value moves on */
static void choose(void *object) {
    int row = GOOL_REGISTER(object, CURSOR) / ITEM;

    if (row == done_row()) {
        close_options(object);
    } else if (row >= 0 && row < done_row()) {
        change(object, row, 1);
    } else {
        /* Off the rows: nothing to choose */
    }
}

/* Each operation does its word's work and the options screen's */

static void title_items(void *object) {
    if (s_Menu == object) {
        s_Menu = NULL;
    } else {
        /* Not open in this menu */
    }
    GOOL_REGISTER(object, ITEM_COUNT) = TITLE_ITEM_COUNT * ITEM;
}

static void title_choice(void *object) {
    int32_t cursor = GOOL_REGISTER(object, CURSOR);

    push(object, cursor);
    if (s_Menu == object) {
        choose(object);
        jump(object, TITLE_CHOSEN_WORD);
    } else if (cursor == TITLE_OPTIONS_ITEM * ITEM) {
        open_options(object, TITLE_ITEMS);
        jump(object, TITLE_CHOSEN_WORD);
    } else {
        /* NEW GAME or LOAD GAME, the game's own */
    }
}

static void title_sides(void *object) {
    if (s_Menu == object) {
        /* The title's wait for the attract demo held off while the screen is open */
        GOOL_WORD(object, OBJECT_STATE_TIME) = GOOL_WORD(object, OBJECT_FRAME_TIME);
        follow_cursor(object);
        sides(object);
        if (Gool_OpTestPad(BACK_PRESSED, 0)) {
            close_options(object);
        } else {
            /* Still open */
        }
    } else {
        /* The menu's own */
    }
    /* The PAL menu's language item, which this menu hasn't */
    push(object, 0);
}

static void title_texts(void *object) {
    /* On to the RETURN after */
    spawn_title_options(object);
}

static void pause_item(void *object) {
    int32_t item = GOOL_REGISTER(object, ITEM_COUNT);

    if (s_Menu == object) {
        s_Menu = NULL;
    } else {
        /* Not open in this menu */
    }
    write_text(&s_PauseOptions, object, PAUSE_ANIMATION_WORD, "PC OPTIONS");
    spawn_text(object, item, &s_PauseOptions, 0, 0, (item / ITEM - PAUSE_MIDDLE_ROW) * -LINE);
    s_PauseItem = item;
    GOOL_REGISTER(object, ITEM_COUNT) = item + ITEM;
    GOOL_REGISTER(object, TEXT_FRAME) = DONE_FRAME;
}

static void pause_choice(void *object) {
    int32_t cursor = GOOL_REGISTER(object, CURSOR);

    if (s_Menu == object) {
        choose(object);
        if (s_Menu == object) {
            jump(object, PAUSE_CHOSEN_WORD);
        } else {
            /* Closed: on to the options' start */
        }
    } else if (cursor == s_PauseItem) {
        open_options(object, PAUSE_ITEM);
        jump(object, PAUSE_CHOSEN_WORD);
    } else if (cursor != 0) {
        jump(object, PAUSE_CHOICES_WORD);
    } else {
        /* Item 0, the sound's options: the next word */
    }
}

static void pause_sides(void *object) {
    if (s_Menu == object && Gool_OpTestPad(BACK_PRESSED, 0)) {
        /* The pause options started again, the word's value left unpushed: nothing after it runs */
        close_options(object);
    } else if (s_Menu == object) {
        follow_cursor(object);
        sides(object);
        push(object, 0);
    } else {
        push(object, GOOL_REGISTER(object, CENTRE_ROW) == GOOL_REGISTER(object, CURSOR));
    }
}

static void pause_refresh(void *object) {
    if (s_Menu == object) {
        /* The rows it refreshes are gone */
        push(object, 1);
    } else {
        if (s_BackToPauseItem) {
            s_BackToPauseItem = 0;
            GOOL_REGISTER(object, CURSOR) = s_PauseItem;
        } else {
            /* The cursor where the menu has it */
        }
        push(object, GOOL_WORD(object, OBJECT_STATUS_A) & REFRESH_STATUS);
    }
}

void Host_GoolOperation(struct GoolObject *object, unsigned int word) {
    switch (word & OPERATION_MASK) {
    case TITLE_ITEMS:
        title_items(object);
        break;
    case TITLE_CHOICE:
        title_choice(object);
        break;
    case TITLE_SIDES:
        title_sides(object);
        break;
    case TITLE_TEXTS:
        title_texts(object);
        break;
    case PAUSE_ITEM:
        pause_item(object);
        break;
    case PAUSE_CHOICE:
        pause_choice(object);
        break;
    case PAUSE_SIDES:
        pause_sides(object);
        break;
    case PAUSE_REFRESH:
        pause_refresh(object);
        break;
    default:
        Host_Fatal("GOOL operation %08X: none of the host's", word);
        break;
    }
}

/* An entry's code of the size patched, if each word holds what the patch replaces; left if each holds the patch
 * already */
static void patch_entry(void *exe, PatchedEntry *entry) {
    uint32_t *code = GOOL_ITEM(exe, ENTRY_CODE_ITEM);
    uint32_t *after = GOOL_ITEM(exe, ENTRY_CODE_ITEM + 1);
    int patched = 0;
    int original = 0;
    int index;

    if (after - code != entry->code_words) {
        /* Another entry of the name's (the title's DispC) */
        return;
    } else {
        /* The code patched, or code like it */
    }
    for (index = 0; index < entry->count; index++) {
        const Patch *patch = &entry->patches[index];

        if (code[patch->word] == HOST_WORD(patch->operation)) {
            patched++;
        } else if (code[patch->word] == patch->original) {
            original++;
        } else {
            /* Neither */
        }
    }
    if (patched == entry->count) {
        return;
    } else if (original == entry->count) {
        for (index = 0; index < entry->count; index++) {
            code[entry->patches[index].word] = HOST_WORD(entry->patches[index].operation);
        }
    } else if (!entry->refused) {
        entry->refused = 1;
        Host_Log("%s: not the code the options screen patches, its menu left as it is", entry->name);
    } else {
        /* Logged */
    }
}

static int patch_object(void *object, void *argument) {
    void *exe = (void *)(uintptr_t)GOOL_WORD(object, OBJECT_EXE);
    char name[EID_NAME_BYTES];
    size_t index;

    (void)argument;
    if (exe == NULL) {
        return 0;
    } else {
        Host_EidName((uint32_t)GOOL_WORD(exe, ENTRY_EID), name);
    }
    for (index = 0; index < sizeof(s_Entries) / sizeof(s_Entries[0]); index++) {
        if (strcmp(name, s_Entries[index].name) == 0) {
            patch_entry(exe, &s_Entries[index]);
        } else {
            continue;
        }
    }
    return 0;
}

void Host_PatchMenus(void) {
    Obj_ForEachList((void *)Obj_ForEachDescendant, (void *)patch_object, NULL);
}
