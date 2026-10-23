/* The module's imports (tools/wasm/module.py): SDL's functions, on the module's addresses, and the GPU renderer's
 * (src/render.c), native; and what the renderer reads of the module, through the module's exports */
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu_raster.h"
#include "host64.h"
#include "host_log.h"
#include "precise.h"
#include "psyq/host_psyq.h"
#include "render.h"
#include "settings.h"

/* SDL's objects the module holds, each a small block of the module's memory standing for the native one. A texture's
 * block holds what the module reads of it, SDL_Texture's public fields: its format, width, height and count */
#define HANDLES_MAX 64
#define HANDLE_BYTES 16
#define TEXTURE_FORMAT_OFFSET 0
#define TEXTURE_WIDTH_OFFSET 4
#define TEXTURE_HEIGHT_OFFSET 8
/* SDL_GetError's message, copied for the module */
#define ERROR_BYTES 1024
/* What the renderer's reads of the module hand back: Host_DisplayArea's 5 ints, Precise_Find's projections */
#define DISPLAY_AREA_VALUES 5
#define WORD_BYTES 4
#define SCRATCH_BYTES (CANDIDATES * sizeof(Projection) > DISPLAY_AREA_VALUES * WORD_BYTES ? \
                       CANDIDATES * sizeof(Projection) : DISPLAY_AREA_VALUES * WORD_BYTES)
/* A dialog's filter in the module: its name's and its pattern's addresses */
#define FILTER_BYTES 8
#define FILTER_PATTERN_OFFSET 4
#define DIALOG_FILTERS_MAX 4
#define DIALOG_FILES_MAX 1
#define MESSAGE_BYTES 1024
#define HEADLESS_VARIABLE "CRASH2_HEADLESS"

struct w2c_env {
    int unused;
};

/* An SDL_DialogFileCallback of the module, as wasm2c compiles it: its instance, then userdata, filelist, filter */
typedef void (*DialogCallback)(void *, uint32_t, uint32_t, uint32_t);

typedef struct {
    uint32_t address;
    void *native;
} Handle;

/* A file dialog's answer, from whichever thread SDL calls back on, handed to the module on its own (deliver_dialog) */
typedef struct {
    int pending;
    int answered;
    uint32_t callback;
    uint32_t userdata;
    int failed;
    char error[ERROR_BYTES];
    int count;
    char *files[DIALOG_FILES_MAX];
    int filter;
    char *names[DIALOG_FILTERS_MAX];
    char *patterns[DIALOG_FILTERS_MAX];
    SDL_DialogFileFilter filters[DIALOG_FILTERS_MAX];
} Dialog;

struct w2c_env g_Env;
static Handle s_Handles[HANDLES_MAX];
static uint32_t s_Error;
static uint32_t s_Keys;
static int s_KeyCount;
static uint32_t s_Scratch;
static Dialog s_Dialog;
static SDL_Mutex *s_DialogLock;

uint8_t *Module_Bytes(uint32_t address) {
    if (address == 0) {
        return NULL;
    } else {
        return w2c_crash2_memory(&g_Module)->data + address;
    }
}

static const char *text(uint32_t address) {
    return (const char *)Module_Bytes(address);
}

static uint32_t module_alloc(uint32_t bytes) {
    uint32_t address = w2c_crash2_malloc(&g_Module, bytes);

    if (address == 0) {
        Host_Fatal("no memory in the module for %u bytes", bytes);
    } else {
        memset(Module_Bytes(address), 0, bytes);
    }
    return address;
}

static uint32_t module_string(const char *string) {
    uint32_t bytes = (uint32_t)strlen(string) + 1;
    uint32_t address = module_alloc(bytes);

    memcpy(Module_Bytes(address), string, bytes);
    return address;
}

static uint32_t handle_new(void *native) {
    int index;

    if (native == NULL) {
        return 0;
    } else {
        for (index = 0; index < HANDLES_MAX && s_Handles[index].native != NULL; index++) {
            /* The first free one */
        }
    }
    if (index == HANDLES_MAX) {
        Host_Fatal("more than %d of SDL's objects in the module", HANDLES_MAX);
        return 0;
    } else {
        s_Handles[index].address = module_alloc(HANDLE_BYTES);
        s_Handles[index].native = native;
        return s_Handles[index].address;
    }
}

static Handle *handle_of(uint32_t address) {
    int index;

    for (index = 0; index < HANDLES_MAX; index++) {
        if (s_Handles[index].native != NULL && s_Handles[index].address == address) {
            return &s_Handles[index];
        } else {
            continue;
        }
    }
    Host_Fatal("the module names an SDL object it wasn't given, at 0x%08X", address);
    return NULL;
}

static void *native_of(uint32_t address) {
    if (address == 0) {
        return NULL;
    } else {
        return handle_of(address)->native;
    }
}

static void handle_drop(uint32_t address) {
    Handle *handle;

    if (address == 0) {
        return;
    } else {
        handle = handle_of(address);
    }
    w2c_crash2_free(&g_Module, handle->address);
    handle->native = NULL;
    handle->address = 0;
}

static uint32_t truth(bool value) {
    if (value) {
        return 1;
    } else {
        return 0;
    }
}

/* The dialog's answer, called by SDL on any thread: copied, for the module's thread */
static void SDLCALL on_dialog(void *userdata, const char *const *files, int filter) {
    (void)userdata;
    SDL_LockMutex(s_DialogLock);
    if (files == NULL) {
        s_Dialog.failed = 1;
        SDL_strlcpy(s_Dialog.error, SDL_GetError(), sizeof(s_Dialog.error));
    } else {
        for (s_Dialog.count = 0; s_Dialog.count < DIALOG_FILES_MAX && files[s_Dialog.count] != NULL; s_Dialog.count++) {
            s_Dialog.files[s_Dialog.count] = SDL_strdup(files[s_Dialog.count]);
        }
    }
    s_Dialog.filter = filter;
    s_Dialog.answered = 1;
    SDL_UnlockMutex(s_DialogLock);
}

/* The answer handed to the module's callback, on its own thread: a list of its own strings, NULL when the dialog
 * failed (its error SDL_GetError's) */
static void deliver_dialog(void) {
    wasm_rt_funcref_table_t *table;
    wasm_rt_funcref_t function;
    uint32_t list = 0;
    int index;

    if (s_DialogLock == NULL || !s_Dialog.pending) {
        return;
    } else {
        SDL_LockMutex(s_DialogLock);
    }
    if (!s_Dialog.answered) {
        SDL_UnlockMutex(s_DialogLock);
        return;
    } else if (s_Dialog.failed) {
        SDL_SetError("%s", s_Dialog.error);
    } else {
        list = module_alloc((uint32_t)(s_Dialog.count + 1) * WORD_BYTES);
        for (index = 0; index < s_Dialog.count; index++) {
            uint32_t file = module_string(s_Dialog.files[index]);

            memcpy(Module_Bytes(list + (uint32_t)index * WORD_BYTES), &file, WORD_BYTES);
            SDL_free(s_Dialog.files[index]);
        }
    }
    s_Dialog.pending = 0;
    SDL_UnlockMutex(s_DialogLock);
    table = w2c_crash2_0x5F_indirect_function_table(&g_Module);
    if (s_Dialog.callback >= table->size || table->data[s_Dialog.callback].func == NULL) {
        Host_Fatal("the module's dialog callback %u isn't in its table", s_Dialog.callback);
    } else {
        function = table->data[s_Dialog.callback];
        ((DialogCallback)function.func)(function.module_instance, s_Dialog.userdata, list, (uint32_t)s_Dialog.filter);
    }
    if (list != 0) {
        for (index = 0; index < s_Dialog.count; index++) {
            uint32_t file;

            memcpy(&file, Module_Bytes(list + (uint32_t)index * WORD_BYTES), WORD_BYTES);
            w2c_crash2_free(&g_Module, file);
        }
        w2c_crash2_free(&g_Module, list);
    } else {
        /* The dialog failed: no list */
    }
}

/* SDL */

u32 w2c_env_SDL_Init(struct w2c_env *env, u32 flags) {
    (void)env;
    return truth(SDL_Init(flags));
}

u32 w2c_env_SDL_InitSubSystem(struct w2c_env *env, u32 flags) {
    (void)env;
    return truth(SDL_InitSubSystem(flags));
}

u32 w2c_env_SDL_SetHint(struct w2c_env *env, u32 name, u32 value) {
    (void)env;
    return truth(SDL_SetHint(text(name), text(value)));
}

u32 w2c_env_SDL_GetError(struct w2c_env *env) {
    (void)env;
    if (s_Error == 0) {
        s_Error = module_alloc(ERROR_BYTES);
    } else {
        /* Its buffer made */
    }
    SDL_strlcpy((char *)Module_Bytes(s_Error), SDL_GetError(), ERROR_BYTES);
    return s_Error;
}

u64 w2c_env_SDL_GetTicksNS(struct w2c_env *env) {
    (void)env;
    return SDL_GetTicksNS();
}

void w2c_env_SDL_Delay(struct w2c_env *env, u32 ms) {
    (void)env;
    SDL_Delay(ms);
}

void w2c_env_SDL_DelayPrecise(struct w2c_env *env, u64 ns) {
    (void)env;
    SDL_DelayPrecise(ns);
}

void w2c_env_SDL_PumpEvents(struct w2c_env *env) {
    (void)env;
    SDL_PumpEvents();
    deliver_dialog();
}

/* SDL_Event: 128 bytes on any system, the events the module reads (quit, a gamepad's coming or going) of fixed-size
 * fields only */
u32 w2c_env_SDL_PollEvent(struct w2c_env *env, u32 event) {
    SDL_Event native;

    (void)env;
    deliver_dialog();
    if (event == 0) {
        return truth(SDL_PollEvent(NULL));
    } else if (SDL_PollEvent(&native)) {
        memcpy(Module_Bytes(event), &native, sizeof(native));
        return 1;
    } else {
        return 0;
    }
}

u32 w2c_env_SDL_GetAtomicInt(struct w2c_env *env, u32 atomic) {
    (void)env;
    return (u32)SDL_GetAtomicInt((SDL_AtomicInt *)Module_Bytes(atomic));
}

u32 w2c_env_SDL_SetAtomicInt(struct w2c_env *env, u32 atomic, u32 value) {
    (void)env;
    return (u32)SDL_SetAtomicInt((SDL_AtomicInt *)Module_Bytes(atomic), (int)value);
}

/* What SDL hands the module to free is the module's own (SDL_GetPrefPath, SDL_GetGamepads) */
void w2c_env_SDL_free(struct w2c_env *env, u32 memory) {
    (void)env;
    if (memory != 0) {
        w2c_crash2_free(&g_Module, memory);
    } else {
        /* SDL_free(NULL) does nothing */
    }
}

u32 w2c_env_SDL_GetPrefPath(struct w2c_env *env, u32 org, u32 app) {
    char *path = SDL_GetPrefPath(text(org), text(app));
    uint32_t copy;

    (void)env;
    if (path == NULL) {
        return 0;
    } else {
        copy = module_string(path);
        SDL_free(path);
        return copy;
    }
}

u32 w2c_env_SDL_ShowSimpleMessageBox(struct w2c_env *env, u32 flags, u32 title, u32 message, u32 window) {
    (void)env;
    return truth(SDL_ShowSimpleMessageBox(flags, text(title), text(message), native_of(window)));
}

void w2c_env_SDL_ShowOpenFileDialog(struct w2c_env *env, u32 callback, u32 userdata, u32 window, u32 filters,
                                    u32 count, u32 location, u32 many) {
    int index;

    (void)env;
    if (count > DIALOG_FILTERS_MAX) {
        Host_Fatal("a file dialog with %u filters, past %d", count, DIALOG_FILTERS_MAX);
    } else if (s_Dialog.pending) {
        Host_Fatal("a file dialog while one is open");
    } else {
        /* The one dialog */
    }
    if (s_DialogLock == NULL) {
        s_DialogLock = SDL_CreateMutex();
    } else {
        /* Made at the first dialog */
    }
    for (index = 0; index < DIALOG_FILTERS_MAX; index++) {
        SDL_free(s_Dialog.names[index]);
        SDL_free(s_Dialog.patterns[index]);
        s_Dialog.names[index] = NULL;
        s_Dialog.patterns[index] = NULL;
    }
    for (index = 0; index < (int)count; index++) {
        uint32_t name, pattern;

        memcpy(&name, Module_Bytes(filters + (uint32_t)index * FILTER_BYTES), WORD_BYTES);
        memcpy(&pattern, Module_Bytes(filters + (uint32_t)index * FILTER_BYTES + FILTER_PATTERN_OFFSET), WORD_BYTES);
        s_Dialog.names[index] = SDL_strdup(text(name));
        s_Dialog.patterns[index] = SDL_strdup(text(pattern));
        s_Dialog.filters[index].name = s_Dialog.names[index];
        s_Dialog.filters[index].pattern = s_Dialog.patterns[index];
    }
    s_Dialog.pending = 1;
    s_Dialog.answered = 0;
    s_Dialog.failed = 0;
    s_Dialog.count = 0;
    s_Dialog.callback = callback;
    s_Dialog.userdata = userdata;
    SDL_ShowOpenFileDialog(on_dialog, NULL, native_of(window), s_Dialog.filters, (int)count, text(location), many != 0);
}

u32 w2c_env_SDL_CreateWindow(struct w2c_env *env, u32 title, u32 width, u32 height, u64 flags) {
    (void)env;
    return handle_new(SDL_CreateWindow(text(title), (int)width, (int)height, flags));
}

void w2c_env_SDL_DestroyWindow(struct w2c_env *env, u32 window) {
    (void)env;
    SDL_DestroyWindow(native_of(window));
    handle_drop(window);
}

u32 w2c_env_SDL_SetWindowFullscreen(struct w2c_env *env, u32 window, u32 fullscreen) {
    (void)env;
    return truth(SDL_SetWindowFullscreen(native_of(window), fullscreen != 0));
}

u32 w2c_env_SDL_CreateRenderer(struct w2c_env *env, u32 window, u32 name) {
    (void)env;
    return handle_new(SDL_CreateRenderer(native_of(window), text(name)));
}

u32 w2c_env_SDL_SetRenderLogicalPresentation(struct w2c_env *env, u32 renderer, u32 width, u32 height, u32 mode) {
    (void)env;
    return truth(SDL_SetRenderLogicalPresentation(native_of(renderer), (int)width, (int)height,
                                                  (SDL_RendererLogicalPresentation)mode));
}

u32 w2c_env_SDL_CreateTexture(struct w2c_env *env, u32 renderer, u32 format, u32 access, u32 width, u32 height) {
    SDL_Texture *texture = SDL_CreateTexture(native_of(renderer), (SDL_PixelFormat)format, (SDL_TextureAccess)access,
                                             (int)width, (int)height);
    uint32_t handle = handle_new(texture);

    (void)env;
    if (handle != 0) {
        int32_t fields[] = {(int32_t)texture->format, texture->w, texture->h};

        memcpy(Module_Bytes(handle + TEXTURE_FORMAT_OFFSET), &fields[0], WORD_BYTES);
        memcpy(Module_Bytes(handle + TEXTURE_WIDTH_OFFSET), &fields[1], WORD_BYTES);
        memcpy(Module_Bytes(handle + TEXTURE_HEIGHT_OFFSET), &fields[2], WORD_BYTES);
    } else {
        /* No texture: the module's NULL */
    }
    return handle;
}

void w2c_env_SDL_DestroyTexture(struct w2c_env *env, u32 texture) {
    (void)env;
    SDL_DestroyTexture(native_of(texture));
    handle_drop(texture);
}

u32 w2c_env_SDL_SetTextureScaleMode(struct w2c_env *env, u32 texture, u32 mode) {
    (void)env;
    return truth(SDL_SetTextureScaleMode(native_of(texture), (SDL_ScaleMode)mode));
}

/* SDL_Rect and SDL_FRect: four 32-bit fields on any system */
u32 w2c_env_SDL_UpdateTexture(struct w2c_env *env, u32 texture, u32 rect, u32 pixels, u32 pitch) {
    (void)env;
    return truth(SDL_UpdateTexture(native_of(texture), (const SDL_Rect *)Module_Bytes(rect), Module_Bytes(pixels),
                                   (int)pitch));
}

u32 w2c_env_SDL_SetRenderDrawColor(struct w2c_env *env, u32 renderer, u32 red, u32 green, u32 blue, u32 alpha) {
    (void)env;
    return truth(SDL_SetRenderDrawColor(native_of(renderer), (Uint8)red, (Uint8)green, (Uint8)blue, (Uint8)alpha));
}

u32 w2c_env_SDL_RenderClear(struct w2c_env *env, u32 renderer) {
    (void)env;
    return truth(SDL_RenderClear(native_of(renderer)));
}

u32 w2c_env_SDL_RenderPresent(struct w2c_env *env, u32 renderer) {
    (void)env;
    return truth(SDL_RenderPresent(native_of(renderer)));
}

u32 w2c_env_SDL_RenderTexture(struct w2c_env *env, u32 renderer, u32 texture, u32 source, u32 destination) {
    (void)env;
    return truth(SDL_RenderTexture(native_of(renderer), native_of(texture), (const SDL_FRect *)Module_Bytes(source),
                                   (const SDL_FRect *)Module_Bytes(destination)));
}

/* SDL_AudioSpec: three 32-bit fields on any system. The module pushes its sound: no callback */
u32 w2c_env_SDL_OpenAudioDeviceStream(struct w2c_env *env, u32 device, u32 spec, u32 callback, u32 userdata) {
    (void)env;
    (void)userdata;
    if (callback != 0) {
        Host_Fatal("an audio stream with a callback in the module");
        return 0;
    } else {
        return handle_new(SDL_OpenAudioDeviceStream(device, (const SDL_AudioSpec *)Module_Bytes(spec), NULL, NULL));
    }
}

u32 w2c_env_SDL_ResumeAudioStreamDevice(struct w2c_env *env, u32 stream) {
    (void)env;
    return truth(SDL_ResumeAudioStreamDevice(native_of(stream)));
}

u32 w2c_env_SDL_PutAudioStreamData(struct w2c_env *env, u32 stream, u32 data, u32 bytes) {
    (void)env;
    return truth(SDL_PutAudioStreamData(native_of(stream), Module_Bytes(data), (int)bytes));
}

u32 w2c_env_SDL_GetAudioStreamQueued(struct w2c_env *env, u32 stream) {
    (void)env;
    return (u32)SDL_GetAudioStreamQueued(native_of(stream));
}

/* The keyboard's state, copied into the module at each call */
u32 w2c_env_SDL_GetKeyboardState(struct w2c_env *env, u32 count) {
    int keys;
    const bool *state = SDL_GetKeyboardState(&keys);

    (void)env;
    if (s_Keys == 0 || keys > s_KeyCount) {
        if (s_Keys != 0) {
            w2c_crash2_free(&g_Module, s_Keys);
        } else {
            /* The first call */
        }
        s_Keys = module_alloc((uint32_t)keys);
        s_KeyCount = keys;
    } else {
        /* Its buffer holds them */
    }
    memcpy(Module_Bytes(s_Keys), state, (size_t)keys);
    if (count != 0) {
        memcpy(Module_Bytes(count), &keys, WORD_BYTES);
    } else {
        /* Not asked */
    }
    return s_Keys;
}

/* The gamepads' IDs, in the module's memory, which it frees with SDL_free */
u32 w2c_env_SDL_GetGamepads(struct w2c_env *env, u32 count) {
    int found;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&found);
    uint32_t list;

    (void)env;
    if (gamepads == NULL) {
        return 0;
    } else {
        list = module_alloc((uint32_t)(found + 1) * (uint32_t)sizeof(SDL_JoystickID));
        memcpy(Module_Bytes(list), gamepads, (size_t)found * sizeof(SDL_JoystickID));
        SDL_free(gamepads);
    }
    if (count != 0) {
        memcpy(Module_Bytes(count), &found, WORD_BYTES);
    } else {
        /* Not asked */
    }
    return list;
}

u32 w2c_env_SDL_OpenGamepad(struct w2c_env *env, u32 id) {
    (void)env;
    return handle_new(SDL_OpenGamepad((SDL_JoystickID)id));
}

void w2c_env_SDL_CloseGamepad(struct w2c_env *env, u32 gamepad) {
    (void)env;
    SDL_CloseGamepad(native_of(gamepad));
    handle_drop(gamepad);
}

u32 w2c_env_SDL_GetGamepadID(struct w2c_env *env, u32 gamepad) {
    (void)env;
    return (u32)SDL_GetGamepadID(native_of(gamepad));
}

u32 w2c_env_SDL_GetGamepadAxis(struct w2c_env *env, u32 gamepad, u32 axis) {
    (void)env;
    return (u32)(int32_t)SDL_GetGamepadAxis(native_of(gamepad), (SDL_GamepadAxis)axis);
}

u32 w2c_env_SDL_GetGamepadButton(struct w2c_env *env, u32 gamepad, u32 button) {
    (void)env;
    return truth(SDL_GetGamepadButton(native_of(gamepad), (SDL_GamepadButton)button));
}

/* The GPU renderer: RasterState and RasterVertex are fixed-size fields on any system */

u32 w2c_env_Render_Start(struct w2c_env *env, u32 window) {
    (void)env;
    return (u32)Render_Start(native_of(window));
}

u32 w2c_env_Render_Active(struct w2c_env *env) {
    (void)env;
    return (u32)Render_Active();
}

u32 w2c_env_Render_Drawing(struct w2c_env *env) {
    (void)env;
    return (u32)Render_Drawing();
}

void w2c_env_Render_ApplySettings(struct w2c_env *env) {
    (void)env;
    Render_ApplySettings();
}

void w2c_env_Render_SetDrawingArea(struct w2c_env *env, u32 left, u32 top, u32 right, u32 bottom) {
    (void)env;
    Render_SetDrawingArea(left, top, right, bottom);
}

void w2c_env_Render_Polygon(struct w2c_env *env, u32 state, u32 count, u32 vertices) {
    (void)env;
    Render_Polygon((const RasterState *)Module_Bytes(state), (int)count, (const RasterVertex *)Module_Bytes(vertices));
}

void w2c_env_Render_Line(struct w2c_env *env, u32 state, u32 count, u32 vertices) {
    (void)env;
    Render_Line((const RasterState *)Module_Bytes(state), (int)count, (const RasterVertex *)Module_Bytes(vertices));
}

void w2c_env_Render_Rectangle(struct w2c_env *env, u32 state, u32 x, u32 y, u32 width, u32 height, u32 texcoord) {
    (void)env;
    Render_Rectangle((const RasterState *)Module_Bytes(state), (int32_t)x, (int32_t)y, (uint16_t)width,
                     (uint16_t)height, (uint16_t)texcoord);
}

void w2c_env_Render_Fill(struct w2c_env *env, u32 x, u32 y, u32 width, u32 height, u32 color) {
    (void)env;
    Render_Fill(x, y, width, height, color);
}

void w2c_env_Render_Copy(struct w2c_env *env, u32 from_x, u32 from_y, u32 to_x, u32 to_y, u32 width, u32 height) {
    (void)env;
    Render_Copy(from_x, from_y, to_x, to_y, width, height);
}

void w2c_env_Render_Load(struct w2c_env *env, u32 x, u32 y, u32 width, u32 height) {
    (void)env;
    Render_Load(x, y, width, height);
}

void w2c_env_Render_Present(struct w2c_env *env, u32 x, u32 y, u32 width, u32 height, u32 enabled, u32 pixels) {
    (void)env;
    Render_Present((int)x, (int)y, (int)width, (int)height, (int)enabled, (const uint32_t *)Module_Bytes(pixels));
}

void w2c_env_Render_Shot(struct w2c_env *env, u32 path) {
    (void)env;
    Render_Shot(text(path));
}

/* What the renderer reads of the module */

static uint32_t scratch(void) {
    if (s_Scratch == 0) {
        s_Scratch = module_alloc((uint32_t)SCRATCH_BYTES);
    } else {
        /* Made at the first read */
    }
    return s_Scratch;
}

int Host_Setting(int index) {
    return (int)w2c_crash2_Host_Setting(&g_Module, (u32)index);
}

const uint16_t *Host_Vram(void) {
    return (const uint16_t *)Module_Bytes(w2c_crash2_Host_Vram(&g_Module));
}

int Host_ResolutionScale(void) {
    return (int)w2c_crash2_Host_ResolutionScale(&g_Module);
}

void Host_DisplayArea(int *x, int *y, int *width, int *height, int *enabled) {
    uint32_t values = scratch();
    int *outputs[DISPLAY_AREA_VALUES] = {x, y, width, height, enabled};
    int index;

    w2c_crash2_Host_DisplayArea(&g_Module, values, values + WORD_BYTES, values + 2 * WORD_BYTES,
                                values + 3 * WORD_BYTES, values + 4 * WORD_BYTES);
    for (index = 0; index < DISPLAY_AREA_VALUES; index++) {
        memcpy(outputs[index], Module_Bytes(values + (uint32_t)index * WORD_BYTES), WORD_BYTES);
    }
}

int Precise_Find(uint32_t position, Projection *found) {
    uint32_t projections = scratch();
    int count = (int)w2c_crash2_Precise_Find(&g_Module, position, projections);

    memcpy(found, Module_Bytes(projections), (size_t)count * sizeof(Projection));
    return count;
}

/* The renderer's messages, on stderr as the module's */

void Host_Log(const char *format, ...) {
    va_list arguments;

    va_start(arguments, format);
    fputs("crash2-pc: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

void Host_Fatal(const char *format, ...) {
    va_list arguments;
    char message[MESSAGE_BYTES];

    va_start(arguments, format);
    vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    fprintf(stderr, "crash2-pc: fatal: %s\n", message);
    if (getenv(HEADLESS_VARIABLE) == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Crash Bandicoot 2", message, NULL);
    } else {
        /* stderr only */
    }
    abort();
}
