#include <stdint.h>
#include <string.h>

#include "between.h"
#include "game/gfx.h"
#include "gool.h"
#include "settings.h"

/* 60 frames a second (settings.h): the game's logic at its 30, as on the PlayStation, and a frame of the picture alone
 * between two of its (decomp's FRAME_BETWEEN), with the camera and each object halfway from where the game frame
 * before had them to where this one has them. The frame between runs the game's own drawing (the view, the world,
 * the objects, its end's fade and bands) and none of its logic, then the camera and the objects go back.
 * The game draws a frame into the buffer it builds, and shows the one it drew before; the frame between is drawn into
 * the buffer being built when the game frame ends, and the host shows the frames in their order rather than where
 * the game puts the display: the one between, then the game's. Each takes a vertical blank, the game's logic its
 * two. TRADEOFF: the models' animations step at 30, their frames not blended; the game's halfway poses would be its
 * models' vertices blended */

/* An object halfway: its position, angles and scale, words at 0x60 to 0x80 (decomp's include/game/object.h) */
#define OBJECT_POSE 0x60
#define POSE_WORDS 9
#define ANGLE_WORDS_FIRST 3
#define ANGLE_WORDS_LAST 5
/* Angles go round in 4096; a move further than these is a jump, not blended */
#define ANGLE_TURN 4096
#define ANGLE_HALF_TURN 2048
#define ANGLE_MOST 1024
#define MOVE_MOST 0x100000
#define TRACKED 512
#define TRACKED_MASK (TRACKED - 1)
#define POINTER_SHIFT 4
/* The camera: its position and angles, six words from g_CameraX (decomp's config/symbol_addrs.txt) */
#define CAMERA_WORDS 6
#define CAMERA_ANGLES_FIRST 3
#define FRAME_SHOWN_QUEUE 2

typedef struct {
    void *object;
    void *exe;
    uint32_t seen;
    int blended;
    int32_t before[POSE_WORDS];
    int32_t now[POSE_WORDS];
} Tracked;

/* The game's (decomp's) */
extern int32_t g_CameraX[CAMERA_WORDS];
extern GfxFrame *g_BuildBuffer[];
extern GfxFrame *g_DrawBuffer;
/* The game frames left before the game draws again, and the frame's flags, whose FLAGS_COUNTERS_STOPPED the memory
 * card's screens set (decomp's Gfx_EndFrame) */
extern int32_t D_800675AC;
extern int32_t D_8006CB94;
/* Whether the game is paused, its pause screen's opening and closing too (decomp's Sys_MainLoop) */
extern int32_t g_Paused;
#define FLAGS_COUNTERS_STOPPED 0x200000
void Gfx_ResetFrame(GfxFrame *frame);
int VSync(int mode);
void *PutDrawEnv(void *env);
int DrawOTag(uint32_t *table);

static Tracked s_Tracked[TRACKED];
static uint32_t s_Seen;
static int32_t s_CameraBefore[CAMERA_WORDS];
static int32_t s_CameraNow[CAMERA_WORDS];
static int s_CameraKnown;
static int s_Between;
/* The game frame drawn, waiting for the frame between it and the one before; the frames to show, in their order */
static int16_t s_DrawnX, s_DrawnY;
static int16_t s_ShownX[FRAME_SHOWN_QUEUE], s_ShownY[FRAME_SHOWN_QUEUE];
static int s_ShownCount;

int Host_FrameBetween(void) {
    return s_Between;
}

static int angle_word(int word) {
    return word >= ANGLE_WORDS_FIRST && word <= ANGLE_WORDS_LAST;
}

/* Halfway from before to now, angles the short way round; now itself if the move is a jump */
static int32_t halfway(int32_t before, int32_t now, int angle, int *jumped) {
    int32_t apart = now - before;

    if (angle) {
        apart = ((apart + ANGLE_HALF_TURN) & (ANGLE_TURN - 1)) - ANGLE_HALF_TURN;
        *jumped |= apart > ANGLE_MOST || apart < -ANGLE_MOST;
    } else {
        *jumped |= apart > MOVE_MOST || apart < -MOVE_MOST;
    }
    return before + apart / 2;
}

static Tracked *tracked(void *object) {
    uint32_t first = ((uint32_t)(uintptr_t)object >> POINTER_SHIFT) & TRACKED_MASK;
    uint32_t probe;

    for (probe = 0; probe < TRACKED; probe++) {
        Tracked *slot = &s_Tracked[(first + probe) & TRACKED_MASK];

        if (slot->object == object || slot->seen + 1 < s_Seen) {
            return slot;
        } else {
            continue;
        }
    }
    return NULL;
}

/* An object at a game frame's end: its pose kept, and set halfway from the one before if it was there then */
static int track(void *object, void *argument) {
    void *exe = (void *)(uintptr_t)GOOL_WORD(object, OBJECT_EXE);
    Tracked *slot = tracked(object);
    int jumped = 0;
    int word;

    (void)argument;
    if (slot == NULL) {
        return 0;
    } else if (slot->object != object || slot->exe != exe || slot->seen + 1 != s_Seen) {
        /* New here: nothing to blend from */
        slot->object = object;
        slot->exe = exe;
        memcpy(slot->now, (unsigned char *)object + OBJECT_POSE, sizeof(slot->now));
        slot->seen = s_Seen;
        slot->blended = 0;
        return 0;
    } else {
        memcpy(slot->before, slot->now, sizeof(slot->now));
        memcpy(slot->now, (unsigned char *)object + OBJECT_POSE, sizeof(slot->now));
        slot->seen = s_Seen;
    }
    for (word = 0; word < POSE_WORDS; word++) {
        int32_t value = halfway(slot->before[word], slot->now[word], angle_word(word), &jumped);

        GOOL_WORD(object, OBJECT_POSE + word * WORD_BYTES) = value;
    }
    if (jumped) {
        memcpy((unsigned char *)object + OBJECT_POSE, slot->now, sizeof(slot->now));
    } else {
        /* Halfway */
    }
    slot->blended = !jumped;
    return 0;
}

static void camera_halfway(void) {
    int jumped = 0;
    int32_t halfway_camera[CAMERA_WORDS];
    int word;

    memcpy(s_CameraBefore, s_CameraNow, sizeof(s_CameraNow));
    memcpy(s_CameraNow, g_CameraX, sizeof(s_CameraNow));
    if (!s_CameraKnown) {
        s_CameraKnown = 1;
        return;
    } else {
        for (word = 0; word < CAMERA_WORDS; word++) {
            halfway_camera[word] = halfway(s_CameraBefore[word], s_CameraNow[word], word >= CAMERA_ANGLES_FIRST,
                                           &jumped);
        }
    }
    if (!jumped) {
        memcpy(g_CameraX, halfway_camera, sizeof(halfway_camera));
    } else {
        /* A cut: the camera where it is */
    }
}

void Host_FrameEnded(void) {
    s_DrawnX = g_DrawBuffer->disp.disp.x;
    s_DrawnY = g_DrawBuffer->disp.disp.y;
    if (g_Settings[SETTING_FRAME_RATE].value == 0 || D_800675AC != 0 || (D_8006CB94 & FLAGS_COUNTERS_STOPPED) ||
        g_Paused != 0) {
        /* Off; or a game frame that drew nothing (the game leaves the buffers as they are while a level opens); or
         * the memory card's screens, whose game frames the game runs a vertical blank each, at 60 already; or the
         * pause, whose picture shrinks into its corner from the frame shown before, which a frame between redrawn
         * would alternate with: the display the game's own */
        s_ShownCount = 0;
        return;
    } else {
        s_Seen++;
        Obj_ForEachList((void *)Obj_ForEachDescendant, (void *)track, NULL);
        camera_halfway();
        s_Between = 1;
    }
}

static int restore(void *object, void *argument) {
    Tracked *slot = tracked(object);

    (void)argument;
    if (slot != NULL && slot->object == object && slot->seen == s_Seen && slot->blended) {
        memcpy((unsigned char *)object + OBJECT_POSE, slot->now, sizeof(slot->now));
    } else {
        /* Left as the game frame had it */
    }
    return 0;
}

void Host_FrameBetweenShown(void) {
    GfxFrame *frame = g_BuildBuffer[0];

    VSync(0);
    PutDrawEnv(&frame->draw);
    DrawOTag(frame->ot);
    Gfx_ResetFrame(frame);
    Obj_ForEachList((void *)Obj_ForEachDescendant, (void *)restore, NULL);
    memcpy(g_CameraX, s_CameraNow, sizeof(s_CameraNow));
    /* The frame between, then the game frame it leads to */
    s_ShownX[0] = frame->disp.disp.x;
    s_ShownY[0] = frame->disp.disp.y;
    s_ShownX[1] = s_DrawnX;
    s_ShownY[1] = s_DrawnY;
    s_ShownCount = FRAME_SHOWN_QUEUE;
    s_Between = 0;
}

int Host_ShownArea(int *x, int *y) {
    if (s_ShownCount == 0) {
        return 0;
    } else {
        *x = s_ShownX[0];
        *y = s_ShownY[0];
        s_ShownX[0] = s_ShownX[1];
        s_ShownY[0] = s_ShownY[1];
        s_ShownCount--;
        return 1;
    }
}
