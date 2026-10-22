/* Windows controllers (SDL3 gamepad: XInput, GameInput, DualSense / DualShock / Switch Pro over HIDAPI) feed the same
   FullKey state as the keyboard, with the same contract as runtime/gamepad.m on the Mac (C9):
   - runtime_gamepad_read() returns the merged state of every controller (buttons OR-ed, each stick axis the stronger),
     plus every button pressed since the last read (a press shorter than one game frame is not lost), then clears those;
   - face buttons by position or label (gamepad_layout.h, gamepad_sdl_map.h), the player's layout file TOTK_CONTROLS
     with its hold / tap / toggle / aim modes (controller_map.h, gamepad_modes.h), the 0.12 stick deadzone;
   - runtime_gamepad_clear() forgets everything (focus loss).
   Events arrive on the SDL main thread through gamepad_sdl_event (runtime/frontend_win.c's loop), which also calls
   gamepad_sdl_tick so tap pulses end without a controller event (the Mac's dispatch_after). */
#if defined(_WIN32)
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "frontend_sdl.h"
#include "gamepad_sdl_map.h"
#include "gamepad_modes.h"

enum { MAX_PADS = 8 };
typedef struct { SDL_Gamepad *pad; SDL_JoystickID id; int by_position; } Pad;

static SDL_SpinLock stateLock;                 /* guards gamepadState, touchState, pressedEdges (any thread) */
static RuntimeGamepadState gamepadState, touchState;
static uint64_t pressedEdges;
/* main thread only */
static int started;
static Pad pads[MAX_PADS];
static GamepadModes modes;
static uint64_t tapWake;
static int mouseOn = -1;
static float mouseDx, mouseDy;
static uint64_t mouseButtons;                  /* physical bits */
static int32_t mouseRx, mouseRy;
static float mouseGain = 1200.0f;              /* stick units per pixel moved in one 10 ms pass */
static uint64_t lastTick;

static void load_controller_map(void) {
    gamepad_modes_init(&modes);
    const char *path = getenv("TOTK_CONTROLS");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "[totk-input] controller layout: default\n"); return; }
    static char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); int whole = feof(f); fclose(f);
    if (!whole) runtime_fail("controller layout file too large");
    text[n] = 0;
    const char *error = NULL; int line = controller_map_parse(text, modes.map, modes.modes, &error);
    if (line) { fprintf(stderr, "[totk-input] %s line %d: %s\n", path, line, error); runtime_fail("controller layout file"); }
    uint16_t defaults[CONTROLLER_BUTTONS]; uint8_t defaultModes[CONTROLLER_BUTTONS]; controller_map_default(defaults, defaultModes);
    int changed = 0; for (int i = 0; i < CONTROLLER_BUTTONS; i++) changed += modes.map[i] != defaults[i];
    fprintf(stderr, "[totk-input] controller layout: %s (%d buttons changed)\n", path, changed);
}

static int32_t axis(SDL_Gamepad *g, SDL_GamepadAxis a, int invert) {
    float v = SDL_GetGamepadAxis(g, a) / 32767.0f;
    return gamepad_stick_axis(invert ? -v : v);   /* SDL: y grows downward; the Switch (and GameController): upward */
}

static void refresh(void) {
    RuntimeGamepadState next = {0};
    uint64_t physical = mouseButtons;
    for (int i = 0; i < MAX_PADS; i++) {
        SDL_Gamepad *g = pads[i].pad;
        if (!g) continue;
        next.connected = 1;
        uint8_t down[SDL_GAMEPAD_BUTTON_COUNT];
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) down[b] = SDL_GetGamepadButton(g, (SDL_GamepadButton)b);
        int labels[4];
        for (int b = 0; b < 4; b++) labels[b] = SDL_GetGamepadButtonLabel(g, (SDL_GamepadButton)b);
        physical |= gamepad_sdl_physical(down, labels, SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER),
                                         SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER), pads[i].by_position);
        next.lx = gamepad_stronger_axis(next.lx, axis(g, SDL_GAMEPAD_AXIS_LEFTX, 0));
        next.ly = gamepad_stronger_axis(next.ly, axis(g, SDL_GAMEPAD_AXIS_LEFTY, 1));
        next.rx = gamepad_stronger_axis(next.rx, axis(g, SDL_GAMEPAD_AXIS_RIGHTX, 0));
        next.ry = gamepad_stronger_axis(next.ry, axis(g, SDL_GAMEPAD_AXIS_RIGHTY, 1));
    }
    next.rx = gamepad_stronger_axis(next.rx, mouseRx);
    next.ry = gamepad_stronger_axis(next.ry, mouseRy);
    next.buttons = gamepad_modes_apply(&modes, physical, next.lx || next.ly, next.connected, runtime_now_ns(), &tapWake);
    SDL_LockSpinlock(&stateLock);
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
    SDL_UnlockSpinlock(&stateLock);
}

static void attach(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) if (pads[i].pad && pads[i].id == id) return;
    int slot = -1;
    for (int i = 0; i < MAX_PADS; i++) if (!pads[i].pad) { slot = i; break; }
    if (slot < 0) { fprintf(stderr, "[totk-input] more than %d controllers: ignoring the new one\n", MAX_PADS); return; }
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) { fprintf(stderr, "[totk-input] gamepad %u could not be opened: %s\n", (unsigned)id, SDL_GetError()); return; }
    const char *name = SDL_GetGamepadName(g), *type = SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
    pads[slot] = (Pad){g, id, gamepad_face_by_position(type, name)};
    fprintf(stderr, "[totk-input] native gamepad connected: %s (%s), face buttons by %s\n", name ? name : "unknown",
            type ? type : "unknown", pads[slot].by_position ? "position" : "label");
    runtime_feedback_attach(g);
    refresh();
}
static void detach(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; i++) {
        if (!pads[i].pad || pads[i].id != id) continue;
        const char *name = SDL_GetGamepadName(pads[i].pad);
        fprintf(stderr, "[totk-input] native gamepad disconnected: %s\n", name ? name : "unknown");
        runtime_feedback_detach(pads[i].pad);
        SDL_CloseGamepad(pads[i].pad);
        pads[i] = (Pad){0};
    }
    refresh();
}

int gamepad_sdl_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_GAMEPAD_ADDED: attach(e->gdevice.which); return 1;
    case SDL_EVENT_GAMEPAD_REMOVED: detach(e->gdevice.which); return 1;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_REMAPPED: refresh(); return 1;
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: gamepad_feedback_sensor(&e->gsensor); return 1;
    default: return 0;
    }
}

int gamepad_sdl_mouse_enabled(void) {
    if (mouseOn < 0) {
        const char *e = getenv("TOTK_MOUSE"); mouseOn = e && !strcmp(e, "1");
        const char *g = getenv("TOTK_MOUSE_SENS");
        if (g) { char *end; double v = strtod(g, &end); if (end != g && !*end && v > 0 && v <= 100000) mouseGain = (float)v; }
        if (mouseOn) fprintf(stderr, "[totk-input] mouse: right stick (gain %.0f), left / right / middle button = r2 / l2 / r3\n", mouseGain);
    }
    return mouseOn;
}
void gamepad_sdl_mouse_motion(float dx, float dy) { mouseDx += dx; mouseDy += dy; }
void gamepad_sdl_mouse_button(int sdl_button, int down) {
    int bit = sdl_button == SDL_BUTTON_LEFT ? 9 : sdl_button == SDL_BUTTON_RIGHT ? 8 : sdl_button == SDL_BUTTON_MIDDLE ? 5 : -1;
    if (bit < 0) return;
    if (down) mouseButtons |= 1ULL << bit; else mouseButtons &= ~(1ULL << bit);
    refresh();
}
static int32_t mouse_axis(float d) {
    float v = d * mouseGain;
    return v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : (int32_t)v;
}

void gamepad_sdl_tick(uint64_t now) {
    if (!started) return;
    int changed = 0;
    if (tapWake && now >= tapWake) changed = 1;
    if (mouseOn > 0 && now - lastTick >= 10000000ull) {
        int32_t rx = mouse_axis(mouseDx), ry = mouse_axis(-mouseDy);   /* screen y grows downward */
        mouseDx = mouseDy = 0; lastTick = now;
        if (rx != mouseRx || ry != mouseRy) { mouseRx = rx; mouseRy = ry; changed = 1; }
    }
    if (changed) refresh();
    gamepad_feedback_tick(now);
}

void runtime_gamepad_start(void) {
    if (started) return;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) { fprintf(stderr, "[totk-input] SDL gamepad support unavailable: %s\n", SDL_GetError()); return; }
    started = 1;
    load_controller_map();
    gamepad_sdl_mouse_enabled();
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);   /* the ADDED events for these follow too; attach() ignores repeats */
    for (int i = 0; ids && i < n; i++) attach(ids[i]);
    SDL_free(ids);
}

void runtime_gamepad_clear(void) {
    runtime_feedback_quiet();
    /* Called from the main thread (focus loss) or the game's input code: the mode state is rebuilt by the next refresh,
       which runs on the main thread; only the shared state is reset here. */
    SDL_LockSpinlock(&stateLock);
    gamepadState = (RuntimeGamepadState){0};
    pressedEdges = 0;
    SDL_UnlockSpinlock(&stateLock);
    if (SDL_IsMainThread()) { gamepad_modes_reset(&modes); mouseButtons = 0; mouseRx = mouseRy = 0; mouseDx = mouseDy = 0; tapWake = 0; }
}

void runtime_touch_pad_set(const RuntimeGamepadState *s) {   /* on-screen controls (iOS); kept for the shared contract */
    SDL_LockSpinlock(&stateLock);
    pressedEdges |= s->buttons & ~touchState.buttons;
    touchState = *s;
    SDL_UnlockSpinlock(&stateLock);
}

int overlay_open(void) __attribute__((weak));   /* runtime/overlay: the game reads nothing while the menu is open */
void runtime_gamepad_read(RuntimeGamepadState *out) {
    if (overlay_open && overlay_open()) {
        SDL_LockSpinlock(&stateLock); int c = gamepadState.connected || touchState.connected; pressedEdges = 0; SDL_UnlockSpinlock(&stateLock);
        *out = (RuntimeGamepadState){0}; out->connected = c;
        return;
    }
    SDL_LockSpinlock(&stateLock);
    *out = gamepadState;
    out->buttons |= touchState.buttons | pressedEdges;
    out->lx = gamepad_stronger_axis(out->lx, touchState.lx); out->ly = gamepad_stronger_axis(out->ly, touchState.ly);
    out->rx = gamepad_stronger_axis(out->rx, touchState.rx); out->ry = gamepad_stronger_axis(out->ry, touchState.ry);
    if (touchState.connected) out->connected = 1;
    pressedEdges = 0;
    SDL_UnlockSpinlock(&stateLock);
}
#endif /* _WIN32 */
