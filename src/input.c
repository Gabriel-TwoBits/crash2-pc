#include <SDL3/SDL.h>

#include "input.h"

/* The pad's buttons (input.h) from the host's keyboard and first gamepad, set while pressed. Keyboard: the arrows, X
 * cross, Z square, C circle, V triangle, Enter start, Backspace select, A and S L1 and R1, Q and W L2 and R2. A
 * gamepad's buttons where the PlayStation's sit, its left stick as the directions */

/* A stick past half its travel counts as the direction */
#define STICK_THRESHOLD 16384

typedef struct {
    SDL_Scancode key;
    unsigned int button;
} KeyBinding;

typedef struct {
    SDL_GamepadButton gamepad;
    unsigned int button;
} PadBinding;

static const KeyBinding s_Keys[] = {
    {SDL_SCANCODE_UP, PAD_UP},         {SDL_SCANCODE_DOWN, PAD_DOWN},   {SDL_SCANCODE_LEFT, PAD_LEFT},
    {SDL_SCANCODE_RIGHT, PAD_RIGHT},   {SDL_SCANCODE_X, PAD_CROSS},     {SDL_SCANCODE_Z, PAD_SQUARE},
    {SDL_SCANCODE_C, PAD_CIRCLE},      {SDL_SCANCODE_V, PAD_TRIANGLE},  {SDL_SCANCODE_RETURN, PAD_START},
    {SDL_SCANCODE_BACKSPACE, PAD_SELECT}, {SDL_SCANCODE_A, PAD_L1},     {SDL_SCANCODE_S, PAD_R1},
    {SDL_SCANCODE_Q, PAD_L2},          {SDL_SCANCODE_W, PAD_R2},
};

static const PadBinding s_Buttons[] = {
    {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_UP},      {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_LEFT},  {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_RIGHT},
    {SDL_GAMEPAD_BUTTON_SOUTH, PAD_CROSS},     {SDL_GAMEPAD_BUTTON_WEST, PAD_SQUARE},
    {SDL_GAMEPAD_BUTTON_EAST, PAD_CIRCLE},     {SDL_GAMEPAD_BUTTON_NORTH, PAD_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_START, PAD_START},     {SDL_GAMEPAD_BUTTON_BACK, PAD_SELECT},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R1},
};

static SDL_Gamepad *s_Gamepad;
static int s_Enabled;

void Host_InputStart(void) {
    s_Enabled = 1;
}

/* Another gamepad still connected, if any, in place of the one removed */
static void open_another(SDL_JoystickID removed) {
    int count;
    int index;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&count);

    if (gamepads == NULL) {
        return;
    } else {
        for (index = 0; index < count && s_Gamepad == NULL; index++) {
            if (gamepads[index] != removed) {
                s_Gamepad = SDL_OpenGamepad(gamepads[index]);
            } else {
                continue;
            }
        }
        SDL_free(gamepads);
    }
}

void Host_InputEvent(const SDL_Event *event) {
    if (event->type == SDL_EVENT_GAMEPAD_ADDED && s_Gamepad == NULL) {
        s_Gamepad = SDL_OpenGamepad(event->gdevice.which);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED && s_Gamepad != NULL &&
               SDL_GetGamepadID(s_Gamepad) == event->gdevice.which) {
        SDL_CloseGamepad(s_Gamepad);
        s_Gamepad = NULL;
        open_another(event->gdevice.which);
    } else {
        /* Not a gamepad's coming or going */
    }
}

static unsigned int stick(void) {
    int x = SDL_GetGamepadAxis(s_Gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    int y = SDL_GetGamepadAxis(s_Gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    unsigned int buttons = 0;

    if (x < -STICK_THRESHOLD) {
        buttons |= PAD_LEFT;
    } else if (x > STICK_THRESHOLD) {
        buttons |= PAD_RIGHT;
    } else {
        /* Centred across */
    }
    if (y < -STICK_THRESHOLD) {
        buttons |= PAD_UP;
    } else if (y > STICK_THRESHOLD) {
        buttons |= PAD_DOWN;
    } else {
        /* Centred up and down */
    }
    return buttons;
}

unsigned int Host_InputButtons(void) {
    const bool *keys;
    unsigned int buttons = 0;
    size_t index;

    if (!s_Enabled) {
        return 0;
    } else {
        keys = SDL_GetKeyboardState(NULL);
    }
    for (index = 0; index < SDL_arraysize(s_Keys); index++) {
        if (keys[s_Keys[index].key]) {
            buttons |= s_Keys[index].button;
        } else {
            /* Not held */
        }
    }
    if (s_Gamepad != NULL) {
        for (index = 0; index < SDL_arraysize(s_Buttons); index++) {
            if (SDL_GetGamepadButton(s_Gamepad, s_Buttons[index].gamepad)) {
                buttons |= s_Buttons[index].button;
            } else {
                /* Not held */
            }
        }
        if (SDL_GetGamepadAxis(s_Gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > STICK_THRESHOLD) {
            buttons |= PAD_L2;
        } else {
            /* L2 released */
        }
        if (SDL_GetGamepadAxis(s_Gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > STICK_THRESHOLD) {
            buttons |= PAD_R2;
        } else {
            /* R2 released */
        }
        buttons |= stick();
    } else {
        /* No gamepad */
    }
    return buttons;
}
