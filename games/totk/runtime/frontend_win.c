/* Native Windows window and VI service adapter (C9): the SDL3 counterpart of runtime/frontend.m, same runtime_front_*
   surface. The main thread owns the window and the event loop; the game runs on a thread of its own (8 MiB stack), as on
   the Mac. What is here:
   - one resizable, high-DPI window (Vulkan-capable: SDL_WINDOW_VULKAN; front_vk_surface / front_vk_instance_extensions in
     host/front.h hand the C11 backend its surface), default 1920x1080 scaled to the screen, TOTK_WINDOW_SIZE=<w>x<h>;
   - keyboard -> runtime_input_key with the Mac key codes (keymap_sdl.h), so the keyboard layout is the Mac one;
     controllers and the optional mouse (TOTK_MOUSE=1) through runtime/gamepad_sdl.c;
   - focus: gaining or losing it tells the game (runtime_oe_push_message(15)), losing it clears every held key/button;
   - close: the process ends with the window, waiting up to 15 s for a save in progress (runtime_fs_save_busy);
   - vsync: a thread signals the game's VI vsync events once per desktop composition (DwmFlush);
   - boot / warm-up progress and the text entry for nn::swkbd shown in the window title (no AppKit-style overlay yet:
     the picture-side HUD is the backend's fps_overlay). */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#define SDL_MAIN_HANDLED   /* the runtime's own main (loader.c) calls runtime_front_run: no SDL_main */
#include <SDL3/SDL_main.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "game_version.h"
#include "keymap_sdl.h"
#include "frontend_sdl.h"
#include "../third_party/d2r_gfx/host/front.h"

void hle_signal_system_event(void *);

static SDL_Window *window;
static int windowVulkan;
static uint64_t bootStartNS;
static _Atomic uint64_t presentedFrames;
static _Atomic int warmPending, focused, windowVisible = -1;
static _Atomic uint32_t drawableW, drawableH;
static SDL_Mutex *uiLock;                 /* title text and the text entry state */
static char titleBase[256], titleStatus[256];
static int titleDirty;
static SDL_SpinLock vsyncLock;
static void *vsyncEvents[16];
static unsigned vsyncCount;

/* ---------- host/front.h ---------- */
void *front_metal_layer(void) { return NULL; }   /* no Metal on Windows: the NVN backend uses front_vk_surface */
void *front_sdl_window(void) { return window; }
void *front_win32_hwnd(void) {
    return window ? SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}
const char *const *front_vk_instance_extensions(uint32_t *count) {
    *count = 0;
    if (!window || !windowVulkan) return NULL;
    Uint32 n = 0; const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&n);
    if (ext) *count = n;
    return ext;
}
uint64_t front_vk_surface(void *vk_instance) {
    if (!window || !windowVulkan || !vk_instance) return 0;
    VkSurfaceKHR s = 0;
    if (!SDL_Vulkan_CreateSurface(window, (VkInstance)vk_instance, NULL, &s)) {
        fprintf(stderr, "[totk] Vulkan surface creation failed: %s\n", SDL_GetError()); return 0;
    }
    return (uint64_t)(uintptr_t)s;
}
int front_drawable_size(uint32_t *w, uint32_t *h) {
    *w = atomic_load(&drawableW); *h = atomic_load(&drawableH);
    return *w && *h;
}
uint64_t front_game_res(int docked) { (void)docked; return (720ULL << 32) | 1280; }

static void set_status(const char *text) {   /* any thread: shown in the title until the first frame (main loop applies it) */
    if (!uiLock) return;
    SDL_LockMutex(uiLock);
    snprintf(titleStatus, sizeof titleStatus, "%s", text ? text : "");
    titleDirty = 1;
    SDL_UnlockMutex(uiLock);
}
void front_frame_presented(void) {
    if (atomic_fetch_add_explicit(&presentedFrames, 1, memory_order_relaxed) == 0) {
        fprintf(stderr, "[totk-boot] %.3f first presented frame\n", (runtime_now_ns() - bootStartNS) / 1e9);
        if (!atomic_load(&warmPending)) set_status(NULL);
    }
}

/* ---------- boot progress (title bar) ---------- */
static double boot_seconds(void) { return bootStartNS ? (runtime_now_ns() - bootStartNS) / 1e9 : 0; }
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    static const char *last;
    if (stage != last) { fprintf(stderr, "[totk-boot] %.3f %s\n", boot_seconds(), stage ? stage : "startup"); last = stage; }
    if (atomic_load(&presentedFrames)) return;
    char s[256];
    if (total) snprintf(s, sizeof s, "%s  %.1f / %.1f GB", stage ? stage : "Starting", done / 1e9, total / 1e9);
    else snprintf(s, sizeof s, "%s  (working, please wait)", stage ? stage : "Starting");
    set_status(s);
}
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) {
    static const char *last;
    if (stage != last) { fprintf(stderr, "[totk-boot] %.3f %s\n", boot_seconds(), stage ? stage : "startup"); last = stage; }
    if (atomic_load(&presentedFrames)) return;
    char s[256]; snprintf(s, sizeof s, "%s  %llu / %llu", stage ? stage : "Starting", (unsigned long long)done, (unsigned long long)total);
    set_status(s);
}
void runtime_boot_status(const char *stage, const char *detail) {
    if (bootStartNS) fprintf(stderr, "[totk-boot] %.3f %s — %s\n", boot_seconds(), stage ? stage : "startup", detail ? detail : "");
    if (atomic_load(&presentedFrames)) return;
    char s[256]; snprintf(s, sizeof s, "%s — %s", stage ? stage : "Starting", detail ? detail : "Working, please wait");
    set_status(s);
}
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total) {
    atomic_store(&warmPending, 1);
    double percent = total ? (double)done / (double)total * 100.0 : 100.0; if (percent > 100) percent = 100;
    char s[256];
    snprintf(s, sizeof s, "Warming shaders and pipelines %.0f%% · shaders %llu/%llu · pipelines %llu/%llu", percent,
             (unsigned long long)shaders, (unsigned long long)shader_total, (unsigned long long)pipelines, (unsigned long long)pipeline_total);
    set_status(s);
}
void runtime_warm_done(void) {
    atomic_store(&warmPending, 0);
    if (atomic_load(&presentedFrames)) set_status(NULL);
}

/* TOTK_ASSUME_FOCUS=1 (unattended tests): the game is told it is focused and the window does not take the keyboard. */
static int assume_focus(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_ASSUME_FOCUS"); v = e && atoi(e); } return v; }
int runtime_front_focus(void) { return 1; }   /* as on the Mac: the game keeps running unfocused; input is cleared instead */
int runtime_front_window_visible(void) { return atomic_load(&windowVisible); }
void runtime_front_request_close(void) { SDL_Event e = {0}; e.type = SDL_EVENT_QUIT; SDL_PushEvent(&e); }
void runtime_shortcut_help_toggle(void) { int mode = fps_overlay_toggle_hints(); fprintf(stderr, "[totk] overlay mode %d\n", mode); }

/* ---------- text entry (nn::swkbd, runtime/swkbd_host.c) ----------
   The calling guest thread waits; the main thread takes the keyboard from the game while the entry is open: typed text
   (IME included, SDL text input events) is filtered as on the Mac (disallowed characters, the length limit in UTF-16
   units), Backspace deletes, Return confirms (at least min_units, not empty), Escape cancels. The prompt and the text
   are shown in the window title. */
static SDL_Condition *textCv;
static int textInputOn;   /* main thread: SDL text input started for the entry */
static struct {
    int pending, active, done, ok;
    int max_units, min_units, digits_only, secure; uint32_t disabled;
    char prompt[256], buf[1024];
} text;

static int text_allowed(uint32_t c) {
    int digit = c >= '0' && c <= '9'; uint32_t d = text.disabled;
    if (text.digits_only && !digit) return 0;
    if (c < 0x20 || c == 0x7f) return 0;
    if ((d & 1u << 1 && c == ' ') || (d & 1u << 2 && c == '@') || (d & 1u << 3 && c == '%') || (d & 1u << 4 && c == '/') ||
        (d & 1u << 5 && c == '\\') || (d & 1u << 6 && digit)) return 0;
    return 1;
}
static int utf16_units(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) n += (*p >= 0xF0) ? 2 : 1;
    }
    return n;
}
static void text_append(const char *in) {   /* uiLock held */
    size_t len = strlen(text.buf);
    for (const char *p = in; *p;) {
        const char *start = p; uint32_t c = SDL_StepUTF8(&p, NULL);
        if (!c) break;
        size_t n = (size_t)(p - start);
        if (!text_allowed(c)) continue;
        int units = c >= 0x10000 ? 2 : 1;
        if (text.max_units > 0 && utf16_units(text.buf) + units > text.max_units) break;
        if (len + n >= sizeof text.buf) break;
        memcpy(text.buf + len, start, n); len += n; text.buf[len] = 0;
    }
    titleDirty = 1;
}
static void text_backspace(void) {   /* uiLock held */
    size_t len = strlen(text.buf);
    while (len && (text.buf[len - 1] & 0xC0) == 0x80) len--;
    if (len) len--;
    text.buf[len] = 0; titleDirty = 1;
}
static void text_finish(int ok) {   /* uiLock held */
    if (!text.active) return;
    if (ok && (utf16_units(text.buf) < text.min_units || !text.buf[0])) return;   /* the Mac beeps: stay open */
    text.active = 0; text.done = 1; text.ok = ok; titleDirty = 1;
    SDL_BroadcastCondition(textCv);
}
static void text_activate(void) {   /* main thread, uiLock held */
    text.pending = 0; text.active = 1; titleDirty = 1;
    runtime_input_clear();
    SDL_StartTextInput(window); textInputOn = 1;
    if (!assume_focus()) { SDL_RestoreWindow(window); SDL_RaiseWindow(window); }
    fprintf(stderr, "[totk] keyboard: text entry shown in the title bar (%s)\n", text.prompt);
}
void runtime_front_text_test(const char *op, const char *t) {
    if (!uiLock) return;
    SDL_LockMutex(uiLock);
    if (!text.active) fprintf(stderr, "[totk] keyboard test: no text entry is open\n");
    else {
        if (!strcmp(op, "type")) text_append(t ? t : "");
        else if (!strcmp(op, "enter")) text_finish(1);
        else if (!strcmp(op, "esc")) text_finish(0);
        fprintf(stderr, "[totk] keyboard test: %s -> field \"%s\"\n", op, text.buf);
    }
    SDL_UnlockMutex(uiLock);
}
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size) {
    if (!window || !uiLock || !req || !out || !out_size) { fprintf(stderr, "[totk] keyboard: no game window for text entry; cancelled\n"); return 0; }
    SDL_LockMutex(uiLock);
    while (text.pending || text.active || text.done) SDL_WaitCondition(textCv, uiLock);   /* one entry at a time */
    snprintf(text.prompt, sizeof text.prompt, "%s", req->prompt ? req->prompt : "");
    text.buf[0] = 0; text.max_units = req->max_units; text.min_units = req->min_units; text.digits_only = req->digits_only;
    text.secure = req->secure; text.disabled = req->disabled; text.done = 0; text.ok = 0;
    if (req->initial) text_append(req->initial);
    text.pending = 1;
    SDL_UnlockMutex(uiLock);
    SDL_Event wake = {0}; wake.type = SDL_EVENT_USER; SDL_PushEvent(&wake);
    SDL_LockMutex(uiLock);
    while (!text.done) SDL_WaitCondition(textCv, uiLock);
    int ok = text.ok;
    if (ok) snprintf(out, out_size, "%s", text.buf);
    text.done = 0;
    SDL_BroadcastCondition(textCv);
    SDL_UnlockMutex(uiLock);
    return ok;
}

/* ---------- window title ---------- */
static void apply_title(void) {   /* main thread */
    char t[1400];
    SDL_LockMutex(uiLock);
    if (!titleDirty) { SDL_UnlockMutex(uiLock); return; }
    titleDirty = 0;
    if (text.active) {
        char shown[1024]; snprintf(shown, sizeof shown, "%s", text.buf);
        if (text.secure) { size_t n = strlen(shown); memset(shown, '*', n); }
        snprintf(t, sizeof t, "%s: %s_   (Return confirms, Esc cancels)", text.prompt[0] ? text.prompt : "Text", shown);
    } else if (titleStatus[0]) snprintf(t, sizeof t, "%s — %s", titleBase, titleStatus);
    else snprintf(t, sizeof t, "%s", titleBase);
    SDL_UnlockMutex(uiLock);
    SDL_SetWindowTitle(window, t);
}

/* ---------- VI vsync: one signal per desktop composition ---------- */
static DWORD WINAPI vsync_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        if (FAILED(DwmFlush())) Sleep(16);   /* composition off (remote sessions): about 60 Hz */
        SDL_LockSpinlock(&vsyncLock);
        for (unsigned i = 0; i < vsyncCount; i++) hle_signal_system_event(vsyncEvents[i]);
        SDL_UnlockSpinlock(&vsyncLock);
    }
    return 0;
}

static void sync_drawable(void) {
    int w = 0, h = 0;
    if (!SDL_GetWindowSizeInPixels(window, &w, &h) || w < 1 || h < 1) return;
    if ((uint32_t)w == atomic_load(&drawableW) && (uint32_t)h == atomic_load(&drawableH)) return;
    atomic_store(&drawableW, (uint32_t)w); atomic_store(&drawableH, (uint32_t)h);
    fprintf(stderr, "[totk] window drawable %dx%d (scale %.2f)\n", w, h, SDL_GetWindowDisplayScale(window));
}

static void set_focus(int on) {
    if (atomic_exchange(&focused, on) == on) return;
    if (!on) runtime_input_clear();
    if (gamepad_sdl_mouse_enabled()) SDL_SetWindowRelativeMouseMode(window, on);
    runtime_oe_push_message(15);
    fprintf(stderr, "[totk] window %s focus\n", on ? "gained" : "lost");
}

static DWORD WINAPI close_waiter(void *arg) {
    (void)arg;
    uint64_t t0 = runtime_now_ns();
    while (runtime_fs_save_busy() && runtime_now_ns() - t0 < 15000000000ull) Sleep(20);
    fprintf(stderr, "[totk] save %s after %.2f s; quitting\n", runtime_fs_save_busy() ? "still in progress (gave up)" : "done", (runtime_now_ns() - t0) / 1e9);
    _exit(0);
}
static void close_window(void) {
    static int closing;
    if (closing++) return;
    if (!runtime_fs_save_busy()) { fprintf(stderr, "[totk] window closed; graceful guest shutdown pending\n"); _exit(0); }
    fprintf(stderr, "[totk] window closed while the game is saving: quitting when the save is done\n");
    SDL_HideWindow(window);
    HANDLE t = CreateThread(NULL, 0, close_waiter, NULL, 0, NULL);
    if (t) CloseHandle(t); else _exit(0);
}

static void handle_key(const SDL_KeyboardEvent *k) {
    int down = k->type == SDL_EVENT_KEY_DOWN;
    if (down && !k->repeat && (k->scancode == SDL_SCANCODE_F11 || (k->scancode == SDL_SCANCODE_RETURN && (k->mod & SDL_KMOD_ALT)))) {
        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));   /* borderless desktop fullscreen */
        return;
    }
    SDL_LockMutex(uiLock);
    int typing = text.active;
    if (typing && down) {
        if (k->scancode == SDL_SCANCODE_RETURN || k->scancode == SDL_SCANCODE_KP_ENTER) text_finish(1);
        else if (k->scancode == SDL_SCANCODE_ESCAPE) text_finish(0);
        else if (k->scancode == SDL_SCANCODE_BACKSPACE) text_backspace();
    }
    SDL_UnlockMutex(uiLock);
    if (typing) return;   /* the entry has the keyboard */
    unsigned code = keymap_mac_from_hid((unsigned)k->scancode);
    if (code != KEYMAP_NONE) runtime_input_key(code, down);
}

/* The in-game menu (runtime/overlay): it takes keyboard / mouse input while open; weak so the frontend tests link without it. */
int overlay_event(const SDL_Event *e) __attribute__((weak));
void overlay_init(SDL_Window *w) __attribute__((weak));
static void handle_event(const SDL_Event *e) {
    if (overlay_event && overlay_event(e)) return;
    if (gamepad_sdl_event(e)) return;
    switch (e->type) {
    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: close_window(); break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: handle_key(&e->key); break;
    case SDL_EVENT_TEXT_INPUT:
        SDL_LockMutex(uiLock); if (text.active) text_append(e->text.text); SDL_UnlockMutex(uiLock); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: set_focus(1); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: set_focus(0); break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: sync_drawable(); break;
    case SDL_EVENT_WINDOW_OCCLUDED: case SDL_EVENT_WINDOW_HIDDEN: case SDL_EVENT_WINDOW_MINIMIZED:
        atomic_store(&windowVisible, 0); fprintf(stderr, "[totk] window covered or minimized: no presents until it is visible again\n"); break;
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_SHOWN: case SDL_EVENT_WINDOW_RESTORED:
        if (atomic_exchange(&windowVisible, 1) != 1) fprintf(stderr, "[totk] window visible\n");
        break;
    case SDL_EVENT_MOUSE_MOTION: if (gamepad_sdl_mouse_enabled() && atomic_load(&focused)) gamepad_sdl_mouse_motion(e->motion.xrel, e->motion.yrel); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if (gamepad_sdl_mouse_enabled() && atomic_load(&focused)) gamepad_sdl_mouse_button(e->button.button, e->button.down);
        break;
    default: break;
    }
}

/* Creates the window (no game thread, no loop): runtime_front_run's first half, also used by tests. 1 on success. */
int front_window_create(int hidden) {
    if (window) return 1;
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_APP_NAME, GAME_NAME);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { fprintf(stderr, "[totk] SDL video initialization failed: %s\n", SDL_GetError()); return 0; }
    if (!uiLock) { uiLock = SDL_CreateMutex(); textCv = SDL_CreateCondition(); }
    int ww = 1920, wh = 1080;
    { const char *e = getenv("TOTK_WINDOW_SIZE"); int w = 0, h = 0; if (e && sscanf(e, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 8192 && h <= 8192) { ww = w; wh = h; } }
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 40) {
        double k = (double)usable.w / ww; if ((double)(usable.h - 40) / wh < k) k = (double)(usable.h - 40) / wh;
        if (k < 1) { ww = (int)(ww * k); wh = (int)(wh * k); }
    }
    const char *owner = getenv("TOTK_SLOT_OWNER"), *purpose = getenv("TOTK_SLOT_PURPOSE");
    if (owner && *owner && strcmp(owner, "user"))
        snprintf(titleBase, sizeof titleBase, "Test session (%s) Ver. %s: %s — save copy, the script closes it", owner, TOTK_GAME_VERSION, purpose && *purpose ? purpose : "agent test");
    else snprintf(titleBase, sizeof titleBase, GAME_NAME "  Windows — Ver. %s", TOTK_GAME_VERSION);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (hidden ? SDL_WINDOW_HIDDEN : 0);
    window = SDL_CreateWindow(titleBase, ww, wh, flags | SDL_WINDOW_VULKAN);
    windowVulkan = window != NULL;
    if (!window) {   /* no Vulkan loader (vulkan-1.dll): still a window, the backend will report the missing driver */
        fprintf(stderr, "[totk] Vulkan window unavailable (%s): plain window\n", SDL_GetError());
        window = SDL_CreateWindow(titleBase, ww, wh, flags);
    }
    if (!window) { fprintf(stderr, "[totk] window creation failed: %s\n", SDL_GetError()); return 0; }
    { const char *fs = getenv("TOTK_FULLSCREEN"); if (fs && !strcmp(fs, "1") && !hidden) SDL_SetWindowFullscreen(window, true); }   /* start in borderless fullscreen (launcher setting) */
    sync_drawable();
    if (overlay_init) overlay_init(window);
    return 1;
}

/* TOTK_INPUT_FILE "sdlkey <scancode>" (runtime/input.c): a key press and release pushed into the SDL queue (thread-safe), so
   scripted tests reach the in-game menu without giving the window the keyboard focus. */
int runtime_front_sdl_key(unsigned scancode) {
    if (!window) return 0;
    SDL_Event e; memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = (SDL_Scancode)scancode; e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)scancode, SDL_KMOD_NONE, false);
    e.key.down = true; e.key.windowID = SDL_GetWindowID(window);
    if (!SDL_PushEvent(&e)) return 0;
    e.type = SDL_EVENT_KEY_UP; e.key.down = false;
    return SDL_PushEvent(&e) ? 1 : 0;
}

static DWORD WINAPI guest_main(void *callback) { ((void (*)(void))callback)(); return 0; }

void runtime_front_run(void (*callback)(void)) {
    bootStartNS = runtime_now_ns();
    if (!front_window_create(0)) runtime_fail("native window creation");
    runtime_gamepad_start();
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);
    if (!assume_focus()) SDL_RaiseWindow(window);
    atomic_store(&focused, 1);
    if (gamepad_sdl_mouse_enabled()) SDL_SetWindowRelativeMouseMode(window, true);
    titleDirty = 1;
    HANDLE vs = CreateThread(NULL, 0, vsync_thread, NULL, 0, NULL);
    if (!vs) runtime_fail("native vsync thread creation");
    CloseHandle(vs);
    HANDLE guest = CreateThread(NULL, 8u << 20, guest_main, (void *)callback, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!guest) runtime_fail("native game thread creation");
    CloseHandle(guest);
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 5)) {
            handle_event(&e);
            while (SDL_PollEvent(&e)) handle_event(&e);
        }
        SDL_LockMutex(uiLock);
        if (text.pending && !text.active) text_activate();
        if (!text.active && textInputOn) { SDL_StopTextInput(window); textInputOn = 0; }
        SDL_UnlockMutex(uiLock);
        apply_title();
        gamepad_sdl_tick(runtime_now_ns());
    }
}

/* ---------- nn::vi ---------- */
static int viReady;
static unsigned displayIdentity, layerIdentity;
static void vi_initialize(void) { if (!window) runtime_fail("VI requires the native window"); viReady = 1; fprintf(stderr, "[totk] native VI display initialized\n"); }
static uint32_t vi_open(void **out) { if (!viReady || !out) return 0xf601; *out = &displayIdentity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &displayIdentity) return 0xf601; *out = &layerIdentity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layerIdentity) return 0xf601; *out = window; return 0; }   /* the backend's native window: the SDL window */
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &displayIdentity) return 0xf601;
    hle_bind_system_event(ev, 1);
    SDL_LockSpinlock(&vsyncLock);
    int full = vsyncCount >= 16; if (!full) vsyncEvents[vsyncCount++] = ev;
    SDL_UnlockSpinlock(&vsyncLock);
    if (full) runtime_fail("VI display event limit");
    return 0;
}
#ifdef GAME_BOTW
static uint32_t vi_scaling_mode(void *layer, int mode) { (void)layer; (void)mode; return 0; }
#endif
void *runtime_front_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", vi_initialize}, {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer}, {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync}
#ifdef GAME_BOTW
        , {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_scaling_mode}
#endif
    };
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
#endif /* _WIN32 */
