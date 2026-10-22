/* Windows headless bring-up (C14): the runtime parts not ported yet. Each returns "no host replacement", so the guest
   SDK's own code runs and stops at the first IPC service the runtime does not serve, naming it. */
#include "runtime.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void *runtime_font_override(const char *name) { (void)name; return NULL; }    /* shared_font.c: needs the firmware fonts */
#ifdef TOTK_NO_MOVIE_HOST
void *runtime_movie_override(const char *name) { (void)name; return NULL; }   /* movie_host.c: built without libvpx */
void movie_host_tick(void) {}
#endif

/* Graphics diagnostics and toggles the input commands reach (NVN backend: C11). */
void dm_gfx_set_flag(const char *name, int v) { (void)name; (void)v; }
void taa_toggle(void) {}
int mfxup_output_enabled(void) { return 0; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_anomdump(void) {}
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
__attribute__((weak)) void dm_gfx_shot_at(long frames, const char *path) { (void)frames; (void)path; }   /* the Vulkan backend has its own */
__attribute__((weak)) void dm_gfx_window_shot(const char *path) { (void)path; }
/* game_patches_v143.c: Mach watchpoint diagnostics are Mac only. */
void runtime_diag_frm_watch(double secs, unsigned off, int in_x) { (void)secs; (void)off; (void)in_x; fprintf(stderr, "[totk] frmwatch: Mac only\n"); }
void runtime_diag_xpoke(unsigned off, float value) { (void)off; (void)value; fprintf(stderr, "[totk] xpoke: Mac only\n"); }
/* Headless: no controllers (gamepad_sdl.c joins with the SDL frontend). */
#ifdef TOTK_WIN_FRONTEND
int fps_overlay_toggle_hints(void) { return 0; }   /* no picture-side HUD on Windows yet */
#else
void runtime_gamepad_start(void) {}
void runtime_gamepad_read(RuntimeGamepadState *out) { memset(out, 0, sizeof *out); }
void runtime_gamepad_clear(void) {}
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)side; (void)amp_low; (void)freq_low; (void)amp_high; (void)freq_high;
}
#endif
/* hle_stack.c: host-stack wrappers do not exist under the JIT (host functions are host traps). */
void *runtime_hle_wrap(void *fn) { return fn; }
void runtime_hle_trace_set(int mode) { (void)mode; fprintf(stderr, "[totk] hletrace: not available with the JIT backend\n"); }
void *runtime_hle_wrap_named(void *fn, const char *name) { (void)name; return fn; }
