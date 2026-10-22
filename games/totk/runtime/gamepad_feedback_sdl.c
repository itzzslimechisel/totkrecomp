/* Windows controller rumble and motion for the game's vibration devices and six-axis sensor (runtime/hid_feedback.c),
   the SDL3 counterpart of runtime/gamepad_feedback.m (C9).
   Rumble: the game's two vibration devices (left, right Joy-Con) drive the controller's two motors: left -> the low
   frequency (left) motor, right -> the high frequency (right) motor, the strength of each the bands' combined amplitude
   times TOTK_RUMBLE_GAIN (gamepad_modes.h gamepad_rumble_intensity, the Mac's intensity). The game's threads only store
   the latest value; the main loop's tick sends it at most 100 times a second (SDL_RumbleGamepad, 250 ms duration,
   renewed every 100 ms while it lasts), and a value counts as silence once the game has sent nothing for 250 ms, so the
   controller never keeps buzzing alone. Frequency is not reproduced (XInput motors have none).
   Motion (TOTK_GYRO): the first controller with an accelerometer and a gyroscope streams both; each gyroscope sample goes to
   runtime/motion_fusion.c with the latest acceleration (m/s^2 -> G; rad/s as is). SDL's axes: x right, y up, z toward the
   player; TOTK_GYRO_AXES remaps them if a controller disagrees. */
#if defined(_WIN32)
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "frontend_sdl.h"
#include "gamepad_modes.h"

enum { RUMBLE_TICK_MS = 10, RUMBLE_STALE_MS = 250, RUMBLE_DURATION_MS = 250, RUMBLE_RENEW_MS = 100 };

static SDL_SpinLock wantLock;                       /* any thread */
static float want[2][4] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
static uint64_t wantAt[2], wantCount;
/* main thread */
static SDL_Gamepad *rumblePad, *motionPad;
static float gain = 1.0f; static int gainRead;
void gamepad_feedback_set_gain(float g) { if (g > 0 && g <= 4) { gain = g; gainRead = 1; } }   /* in-game menu (TOTK_RUMBLE_GAIN) */
static float sent[2] = {-1, -1};
static uint64_t sentAt, lastTick, wantCountLogged, lastLog;
static float acc[3]; static int accSeen;

void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    if (side > 1) return;
    uint64_t now = runtime_now_ns();
    SDL_LockSpinlock(&wantLock);
    want[side][0] = al; want[side][1] = fl; want[side][2] = ah; want[side][3] = fh; wantAt[side] = now; wantCount++;
    SDL_UnlockSpinlock(&wantLock);
}
void runtime_feedback_quiet(void) {
    SDL_LockSpinlock(&wantLock); wantAt[0] = wantAt[1] = 0; SDL_UnlockSpinlock(&wantLock);
}

static int has_rumble(SDL_Gamepad *g) {
    return SDL_GetBooleanProperty(SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

void runtime_feedback_attach(SDL_Gamepad *g) {
    if (!gainRead) {
        gainRead = 1;
        const char *s = getenv("TOTK_RUMBLE_GAIN");
        if (s) { char *end; double v = strtod(s, &end); if (end == s || *end || !(v > 0) || v > 4) runtime_fail("TOTK_RUMBLE_GAIN must be a number in (0, 4]"); gain = (float)v; }
    }
    if (!rumblePad && has_rumble(g)) { rumblePad = g; sent[0] = sent[1] = -1; fprintf(stderr, "[rumble] controller motors: %s\n", SDL_GetGamepadName(g)); }
    if (!motionPad && runtime_gyro_enabled() && SDL_GamepadHasSensor(g, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(g, SDL_SENSOR_ACCEL)) {
        if (SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_ACCEL, true) && SDL_SetGamepadSensorEnabled(g, SDL_SENSOR_GYRO, true)) {
            motionPad = g; accSeen = 0;
            runtime_motion_deadzone_default(1.2); runtime_motion_live(1);
            fprintf(stderr, "[totk-input] motion sensor: %s (%.0f Hz)\n", SDL_GetGamepadName(g), SDL_GetGamepadSensorDataRate(g, SDL_SENSOR_GYRO));
        }
    }
}
void runtime_feedback_detach(SDL_Gamepad *g) {
    if (g == rumblePad) { SDL_RumbleGamepad(g, 0, 0, 0); rumblePad = NULL; }
    if (g == motionPad) { motionPad = NULL; runtime_motion_live(0); }
}

void gamepad_feedback_sensor(const SDL_GamepadSensorEvent *e) {
    if (!motionPad || e->which != SDL_GetGamepadID(motionPad)) return;
    if (e->sensor == SDL_SENSOR_ACCEL) { for (int i = 0; i < 3; i++) acc[i] = e->data[i] / SDL_STANDARD_GRAVITY; accSeen = 1; return; }
    if (e->sensor != SDL_SENSOR_GYRO || !accSeen) return;
    const double a[3] = {acc[0], acc[1], acc[2]}, gy[3] = {e->data[0], e->data[1], e->data[2]};
    runtime_motion_feed(a, gy, runtime_now_ns());
}

void gamepad_feedback_tick(uint64_t now) {
    if (now - lastTick < RUMBLE_TICK_MS * 1000000ull) return;
    lastTick = now;
    if (!rumblePad) return;
    float v[2][4]; uint64_t at[2], count;
    SDL_LockSpinlock(&wantLock); memcpy(v, want, sizeof v); at[0] = wantAt[0]; at[1] = wantAt[1]; count = wantCount; SDL_UnlockSpinlock(&wantLock);
    float s[2];
    for (int side = 0; side < 2; side++) {
        s[side] = at[side] && now - at[side] < RUMBLE_STALE_MS * 1000000ull ? gamepad_rumble_intensity(v[side], gain) : 0;
        if (s[side] < 0.02f) s[side] = 0;
    }
    int changed = fabsf(s[0] - sent[0]) > 0.01f || fabsf(s[1] - sent[1]) > 0.01f;
    int renew = (s[0] > 0 || s[1] > 0) && now - sentAt >= RUMBLE_RENEW_MS * 1000000ull;
    if (changed || renew) {
        if (!SDL_RumbleGamepad(rumblePad, (Uint16)(s[0] * 65535.0f), (Uint16)(s[1] * 65535.0f), RUMBLE_DURATION_MS)) {
            static int warned; if (!warned++) fprintf(stderr, "[rumble] SDL_RumbleGamepad failed: %s\n", SDL_GetError());
        }
        sent[0] = s[0]; sent[1] = s[1]; sentAt = now;
    }
    if (!lastLog) lastLog = now;
    if (now - lastLog >= 30000000000ull) {
        if (count != wantCountLogged) fprintf(stderr, "[rumble] last 30 s: %llu values from the game\n", (unsigned long long)(count - wantCountLogged));
        wantCountLogged = count; lastLog = now;
    }
}
#endif /* _WIN32 */
