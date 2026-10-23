/* Spike: what the headless module needs of SDL, the GPU renderer, the low RAM's handler and the sampler. A headless
 * run on the software picture calls only the first group; the others abort if reached */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "low_ram.h"
#include "render.h"
#include "sampler.h"

#define NS_PER_SECOND 1000000000ull

static void unreached(const char *name) {
    fprintf(stderr, "spike: %s reached\n", name);
    abort();
}

/* Called headless */
Uint64 SDL_GetTicksNS(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (Uint64)now.tv_sec * NS_PER_SECOND + (Uint64)now.tv_nsec;
}
int SDL_GetAtomicInt(SDL_AtomicInt *a) { return a->value; }
int SDL_SetAtomicInt(SDL_AtomicInt *a, int v) { int old = a->value; a->value = v; return old; }
void SDL_free(void *mem) { free(mem); }
const char *SDL_GetError(void) { return "no SDL in the spike"; }
bool SDL_Init(SDL_InitFlags flags) { (void)flags; return false; }
bool SDL_InitSubSystem(SDL_InitFlags flags) { (void)flags; return false; }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
char *SDL_GetPrefPath(const char *org, const char *app) { (void)org; (void)app; return strdup("./"); }
void SDL_Delay(Uint32 ms) { (void)ms; }
void SDL_DelayPrecise(Uint64 ns) { (void)ns; }
void SDL_PumpEvents(void) {}
bool SDL_PollEvent(SDL_Event *event) { (void)event; return false; }
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window) {
    (void)flags; (void)title; (void)message; (void)window; return false;
}
const bool *SDL_GetKeyboardState(int *numkeys) {
    static bool keys[SDL_SCANCODE_COUNT];

    if (numkeys != NULL) {
        *numkeys = SDL_SCANCODE_COUNT;
    } else {
        /* Not asked */
    }
    return keys;
}
SDL_JoystickID *SDL_GetGamepads(int *count) {
    if (count != NULL) {
        *count = 0;
    } else {
        /* Not asked */
    }
    return NULL;
}

/* A window's, a gamepad's or the sound device's: not headless */
void SDL_CloseGamepad(SDL_Gamepad *g) { (void)g; unreached(__func__); }
SDL_Renderer *SDL_CreateRenderer(SDL_Window *w, const char *n) { (void)w; (void)n; unreached(__func__); return NULL; }
SDL_Texture *SDL_CreateTexture(SDL_Renderer *r, SDL_PixelFormat f, SDL_TextureAccess a, int w, int h) {
    (void)r; (void)f; (void)a; (void)w; (void)h; unreached(__func__); return NULL;
}
SDL_Window *SDL_CreateWindow(const char *t, int w, int h, SDL_WindowFlags f) { (void)t; (void)w; (void)h; (void)f; return NULL; }
void SDL_DestroyTexture(SDL_Texture *t) { (void)t; unreached(__func__); }
void SDL_DestroyWindow(SDL_Window *w) { (void)w; unreached(__func__); }
int SDL_GetAudioStreamQueued(SDL_AudioStream *s) { (void)s; unreached(__func__); return 0; }
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis a) { (void)g; (void)a; unreached(__func__); return 0; }
bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b) { (void)g; (void)b; unreached(__func__); return false; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) { (void)g; unreached(__func__); return 0; }
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID d, const SDL_AudioSpec *s, SDL_AudioStreamCallback c,
                                           void *u) { (void)d; (void)s; (void)c; (void)u; return NULL; }
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID i) { (void)i; unreached(__func__); return NULL; }
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *b, int l) { (void)s; (void)b; (void)l; unreached(__func__); return false; }
bool SDL_RenderClear(SDL_Renderer *r) { (void)r; unreached(__func__); return false; }
bool SDL_RenderPresent(SDL_Renderer *r) { (void)r; unreached(__func__); return false; }
bool SDL_RenderTexture(SDL_Renderer *r, SDL_Texture *t, const SDL_FRect *s, const SDL_FRect *d) {
    (void)r; (void)t; (void)s; (void)d; unreached(__func__); return false;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { (void)s; unreached(__func__); return false; }
bool SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 a, Uint8 b, Uint8 c, Uint8 d) {
    (void)r; (void)a; (void)b; (void)c; (void)d; unreached(__func__); return false;
}
bool SDL_SetRenderLogicalPresentation(SDL_Renderer *r, int w, int h, SDL_RendererLogicalPresentation m) {
    (void)r; (void)w; (void)h; (void)m; unreached(__func__); return false;
}
bool SDL_SetTextureScaleMode(SDL_Texture *t, SDL_ScaleMode m) { (void)t; (void)m; unreached(__func__); return false; }
bool SDL_SetWindowFullscreen(SDL_Window *w, bool f) { (void)w; (void)f; unreached(__func__); return false; }
void SDL_ShowOpenFileDialog(SDL_DialogFileCallback c, void *u, SDL_Window *w, const SDL_DialogFileFilter *f, int n,
                            const char *l, bool m) { (void)c; (void)u; (void)w; (void)f; (void)n; (void)l; (void)m; unreached(__func__); }
bool SDL_UpdateTexture(SDL_Texture *t, const SDL_Rect *r, const void *p, int pitch) {
    (void)t; (void)r; (void)p; (void)pitch; unreached(__func__); return false;
}

/* The GPU renderer: none, the software picture alone */
int Render_Start(SDL_Window *window) { (void)window; return 0; }
int Render_Active(void) { return 0; }
int Render_Drawing(void) { return 0; }
void Render_ApplySettings(void) {}
void Render_SetDrawingArea(uint32_t l, uint32_t t, uint32_t r, uint32_t b) { (void)l; (void)t; (void)r; (void)b; }
void Render_Polygon(const RasterState *s, int c, const RasterVertex *v) { (void)s; (void)c; (void)v; }
void Render_Line(const RasterState *s, int c, const RasterVertex *v) { (void)s; (void)c; (void)v; }
void Render_Rectangle(const RasterState *s, int32_t x, int32_t y, uint16_t w, uint16_t h, uint16_t t) {
    (void)s; (void)x; (void)y; (void)w; (void)h; (void)t;
}
void Render_Fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void Render_Copy(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t w, uint32_t h) {
    (void)a; (void)b; (void)c; (void)d; (void)w; (void)h;
}
void Render_Load(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { (void)x; (void)y; (void)w; (void)h; }
void Render_Present(int x, int y, int w, int h, int e, const uint32_t *p) { (void)x; (void)y; (void)w; (void)h; (void)e; (void)p; }
void Render_Shot(const char *path) { (void)path; }

/* The low RAM is the module's memory, readable as any: no handler. No sampler */
void Host_InstallLowRam(void) {}
void Host_StartSampler(void) {}

/* host_text.h's bounds of the port's own code: a module's code is no memory */
char __executable_start[1];
char __etext[1];
