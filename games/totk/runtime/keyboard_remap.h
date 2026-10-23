/* Keyboard rebinding (Windows): "key <key> = <Switch button | none>" lines in the controls file (TOTK_CONTROLS, the same
   file as the controller layout of controller_map.h, which skips these lines). A key with such a line drives that Switch
   button instead of its keymap_sdl.h meaning; "none" makes it do nothing. Keys without a line keep the default layout.
   Keys are named by position (US layout): a-z, 0-9, space, enter, tab, backspace, minus, equals, lbracket, rbracket,
   backslash, semicolon, quote, grave, comma, period, slash, up, down, left, right, lshift, lalt.
   Pure C without SDL headers: tests/input_win/test_keyboard_remap.c covers it. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "controller_map.h"   /* controller_switch_names: a b x y lstick rstick l r zl zr plus minus left up right down */

enum { KEYREMAP_KEEP = 0xFF, KEYREMAP_NONE = 16 };   /* per HID usage: KEEP = default meaning, NONE = unused, 0..15 a button */

typedef struct { const char *name; uint8_t usage; } keyremap_name;
static const keyremap_name keyremap_names[] = {
    {"space", 44}, {"enter", 40}, {"tab", 43}, {"backspace", 42}, {"minus", 45}, {"equals", 46}, {"lbracket", 47},
    {"rbracket", 48}, {"backslash", 49}, {"semicolon", 51}, {"quote", 52}, {"grave", 53}, {"comma", 54}, {"period", 55},
    {"slash", 56}, {"right", 79}, {"left", 80}, {"down", 81}, {"up", 82}, {"lshift", 225}, {"lalt", 226}};

/* HID usage of a key name (case-insensitive), or 0. */
static inline unsigned keyremap_usage(const char *s, size_t n) {
    if (n == 1 && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) return 4u + (unsigned)((*s | 0x20) - 'a');
    if (n == 1 && *s >= '1' && *s <= '9') return 30u + (unsigned)(*s - '1');
    if (n == 1 && *s == '0') return 39;
    for (size_t i = 0; i < sizeof keyremap_names / sizeof *keyremap_names; i++)
        if (strlen(keyremap_names[i].name) == n && !strncasecmp(keyremap_names[i].name, s, n)) return keyremap_names[i].usage;
    return 0;
}
/* Name of a HID usage (the first form keyremap_usage accepts), or NULL. buf holds a one-letter name. */
static inline const char *keyremap_name_of(unsigned usage, char buf[2]) {
    if (usage >= 4 && usage <= 29) { buf[0] = (char)('a' + usage - 4); buf[1] = 0; return buf; }
    if (usage >= 30 && usage <= 38) { buf[0] = (char)('1' + usage - 30); buf[1] = 0; return buf; }
    if (usage == 39) { buf[0] = '0'; buf[1] = 0; return buf; }
    for (size_t i = 0; i < sizeof keyremap_names / sizeof *keyremap_names; i++) if (keyremap_names[i].usage == usage) return keyremap_names[i].name;
    return NULL;
}
/* The default key (HID usage) of each Switch button (keymap_sdl.h + input.c button()); 0 = none by default. */
static inline unsigned keyremap_default_key(int button) {
    static const uint8_t keys[16] = {13 /*a: J*/, 14 /*b: K*/, 12 /*x: I*/, 24 /*y: U*/, 29 /*lstick: Z*/, 6 /*rstick: C*/,
        20 /*l: Q*/, 8 /*r: E*/, 226 /*zl: Left Alt*/, 225 /*zr: Left Shift*/, 40 /*plus: Enter*/, 43 /*minus: Tab*/,
        9 /*left: F*/, 23 /*up: T*/, 11 /*right: H*/, 10 /*down: G*/};
    return button >= 0 && button < 16 ? keys[button] : 0;
}

/* Reads the key lines of text into table[256] (start from all KEYREMAP_KEEP). Returns 0, or the 1-based number of the
   first bad key line with *error set; table is then unchanged. Other lines are ignored. */
static inline int keyremap_parse(const char *text, uint8_t table[256], const char **error) {
    uint8_t next[256]; memcpy(next, table, sizeof next);
    int line = 0;
    for (const char *p = text; p && *p;) {
        line++;
        const char *end = strchr(p, '\n'), *stop = end ? end : p + strlen(p), *hash = (const char *)memchr(p, '#', (size_t)(stop - p));
        if (hash) stop = hash;
        while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        while (stop > p && (stop[-1] == ' ' || stop[-1] == '\t' || stop[-1] == '\r')) stop--;
        if (stop - p >= 4 && !strncasecmp(p, "key", 3) && (p[3] == ' ' || p[3] == '\t')) {
            const char *k = p + 4; while (k < stop && (*k == ' ' || *k == '\t')) k++;
            const char *ke = k; while (ke < stop && *ke != ' ' && *ke != '\t' && *ke != '=') ke++;
            const char *eq = (const char *)memchr(ke, '=', (size_t)(stop - ke));
            if (!eq) { *error = "expected \"key <key> = <Switch button | none>\""; return line; }
            const char *v = eq + 1; while (v < stop && (*v == ' ' || *v == '\t')) v++;
            const char *ve = v; while (ve < stop && *ve != ' ' && *ve != '\t') ve++;
            unsigned usage = keyremap_usage(k, (size_t)(ke - k));
            if (!usage) { *error = "unknown key (a-z 0-9 space enter tab backspace up down left right lshift lalt and punctuation names)"; return line; }
            int target = (ve - v) == 4 && !strncasecmp(v, "none", 4) ? KEYREMAP_NONE
                       : controller_name_index(controller_switch_names, CONTROLLER_SWITCH_BUTTONS, v, (size_t)(ve - v));
            if (target < 0) { *error = "unknown Switch button (a b x y l r zl zr plus minus lstick rstick up down left right none)"; return line; }
            next[usage] = (uint8_t)target;
        }
        p = end ? end + 1 : NULL;
    }
    memcpy(table, next, sizeof next);
    return 0;
}

/* The button a key drives now (0..15), or -1 for none. */
static inline int keyremap_button_of(const uint8_t table[256], unsigned usage) {
    if (usage < 256 && table[usage] != KEYREMAP_KEEP) return table[usage] == KEYREMAP_NONE ? -1 : table[usage];
    for (int b = 0; b < 16; b++) if (keyremap_default_key(b) == usage) return b;
    return -1;
}
/* The key that drives button now (first found), or 0. */
static inline unsigned keyremap_key_of(const uint8_t table[256], int button) {
    unsigned d = keyremap_default_key(button);
    if (d && keyremap_button_of(table, d) == button) return d;
    for (unsigned u = 1; u < 256; u++) if (table[u] == button) return u;
    return 0;
}
/* Binds key to button and gives the button's previous key the button the new key had (a swap: nothing is left
   without a key unless the new key had none). */
static inline void keyremap_bind(uint8_t table[256], int button, unsigned key) {
    unsigned old = keyremap_key_of(table, button);
    int had = keyremap_button_of(table, key);
    if (old == key) return;
    table[key] = (uint8_t)button;
    if (old) table[old] = had >= 0 ? (uint8_t)had : KEYREMAP_NONE;
    for (unsigned u = 1; u < 256; u++) if (table[u] != KEYREMAP_KEEP && table[u] < 16 && keyremap_default_key(table[u]) == u) table[u] = KEYREMAP_KEEP;   /* back to its default: no line */
}
