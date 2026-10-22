// Vulkan NVN backend, game glue for totk-win (Windows x86-64 bring-up): the window comes from the runtime frontend, PNG screenshots of
// presented frames (TOTK_VK_SHOT=<dir>, TOTK_VK_SHOT_EVERY=<n presents>, default 60), and the hooks the NVN layer
// expects from the Metal side that the null backend (nvn_null.c) stubs too.
#include "nvn_vk.h"
#include "../../host/front.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The window belongs to the runtime frontend (runtime/frontend_win.c: SDL3 window, event loop on the main thread, input,
// close, resize, F11). This backend only asks it for the Vulkan instance extensions and a surface.
static void *surface(void *inst) { return (void *)(uintptr_t)front_vk_surface(inst); }
// Pool uploads skip memory the game has not written (nvn_dirty.c write protection): see vk_pools_sync.
#include "nvn_dirty.h"
static const nvn_vk_dirty_hooks g_dirty_hooks = {nvn_dirty_now, nvn_dirty_since, nvn_dirty_protect};
void nvn_vk_frontend_hook(void) {
    if (!getenv("TOTK_VK_NO_DIRTY")) nvn_vk_set_dirty_hooks(&g_dirty_hooks);
    if (getenv("TOTK_VK_NO_WINDOW")) { VKLOG("TOTK_VK_NO_WINDOW: rendering offscreen"); return; }
    uint32_t n = 0; const char *const *ext = front_vk_instance_extensions(&n);
    if (ext && n) nvn_vk_set_window(surface, ext, n);
    else VKLOG("frontend window has no Vulkan surface support (rendering offscreen)");
}

// ---------- PNG writer (stored deflate blocks: no compression library) ----------
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) {
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
    c = ~c; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 255] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), d, n); uint8_t t[4]; be32(t, c); fwrite(t, 1, 4, f);
}
int nvn_vk_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0; chunk(f, "IHDR", ih, 13);
    size_t raw = (size_t)(w * 4 + 1) * h, nblk = (raw + 65534) / 65535;
    uint8_t *z = malloc(2 + raw + nblk * 5 + 4), *o = z; uint32_t a = 1, b = 0;
    *o++ = 0x78; *o++ = 0x01;
    uint8_t *rows = malloc(raw);
    for (uint32_t y = 0; y < h; y++) { rows[(size_t)y * (w * 4 + 1)] = 0; memcpy(rows + (size_t)y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
    for (size_t i = 0; i < raw; i++) { a = (a + rows[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw; off += 65535) {
        uint16_t n = (uint16_t)(raw - off > 65535 ? 65535 : raw - off);
        *o++ = off + n >= raw; *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8); *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        memcpy(o, rows + off, n); o += n;
    }
    be32(o, (b << 16) | a); o += 4;
    chunk(f, "IDAT", z, (uint32_t)(o - z)); chunk(f, "IEND", NULL, 0);
    free(z); free(rows); fclose(f);
    return 1;
}

uint64_t g_draw_skip[16];
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }
