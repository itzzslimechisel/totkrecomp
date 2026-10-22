/* settings_ini.c - <package>\settings.ini shared by the launcher and the game (settings_ini.h). */
#include "settings_ini.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_path[MAX_PATH * 2];
static char g_pkg[MAX_PATH * 2];   /* package folder with a trailing backslash (for %~dp0) */

const char *settings_ini_path(void) {
    if (g_path[0]) return g_path;
    const char *e = getenv("TOTK_SETTINGS_INI");
    if (e && *e) {
        snprintf(g_path, sizeof g_path, "%s", e);
        snprintf(g_pkg, sizeof g_pkg, "%s", e);
        char *s = strrchr(g_pkg, '\\'), *f = strrchr(g_pkg, '/'); if (f > s) s = f;
        if (s) s[1] = 0; else g_pkg[0] = 0;
        return g_path;
    }
    char exe[MAX_PATH * 2]; DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return NULL;
    for (int up = 0; up < 2; up++) { char *s = strrchr(exe, '\\'); if (!s) return NULL; *s = 0; }   /* ...\bin\totk-win.exe -> ... */
    snprintf(g_pkg, sizeof g_pkg, "%s\\", exe);
    snprintf(g_path, sizeof g_path, "%ssettings.ini", g_pkg);
    return g_path;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b) { size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; }
    fclose(f);
    return b;
}

/* One setting line: trimmed name (no spaces), value up to the end of the line (trailing blanks and CR dropped). */
static int parse_line(const char *l, size_t len, char *name, size_t nn, char *val, size_t vn) {
    while (len && (l[0] == ' ' || l[0] == '\t')) { l++; len--; }
    while (len && (l[len - 1] == '\r' || l[len - 1] == ' ' || l[len - 1] == '\t')) len--;
    if (!len || l[0] == ';' || l[0] == '#' || l[0] == '[') return 0;
    const char *eq = memchr(l, '=', len); if (!eq) return 0;
    size_t a = (size_t)(eq - l); while (a && (l[a - 1] == ' ' || l[a - 1] == '\t')) a--;
    if (!a || a >= nn) return 0;
    for (size_t i = 0; i < a; i++) if (l[i] == ' ' || l[i] == '\t') return 0;
    const char *v = eq + 1; size_t b = len - (size_t)(v - l);
    while (b && (*v == ' ' || *v == '\t')) { v++; b--; }
    if (b >= vn) return 0;
    memcpy(name, l, a); name[a] = 0; memcpy(val, v, b); val[b] = 0;
    return 1;
}

void settings_ini_parse(const char *text, void (*fn)(const char *, const char *, void *), void *user) {
    char name[128], val[2048];
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n'); size_t len = e ? (size_t)(e - p) : strlen(p);
        if (parse_line(p, len, name, sizeof name, val, sizeof val)) fn(name, val, user);
        p = e ? e + 1 : NULL;
    }
}

static void expand(const char *in, char *out, size_t on) {   /* %~dp0 -> the package folder */
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < on;) {
        if (!strncmp(p, "%~dp0", 5)) { size_t k = strlen(g_pkg); if (o + k >= on) break; memcpy(out + o, g_pkg, k); o += k; p += 5; }
        else out[o++] = *p++;
    }
    out[o] = 0;
}

static void load_one(const char *name, const char *value, void *user) {
    int *n = user;
    if (getenv(name)) return;   /* the environment wins */
    char v[4096]; expand(value, v, sizeof v);
    if (*v && !_putenv_s(name, v)) (*n)++;
}

int settings_ini_load(void) {
    const char *p = settings_ini_path(); if (!p) return -1;
    char *t = read_all(p); if (!t) return -1;
    int n = 0;
    settings_ini_parse(t, load_one, &n);
    free(t);
    return n;
}

int settings_ini_set(const char *name, const char *value) {
    const char *p = settings_ini_path(); if (!p || !name || !*name) return -1;
    char *t = read_all(p);
    size_t cap = (t ? strlen(t) : 0) + strlen(name) + (value ? strlen(value) : 0) + 256;
    char *o = malloc(cap); if (!o) { free(t); return -1; }
    size_t on = 0; int done = 0;
    char n[128], v[2048];
    for (const char *q = t; q && *q;) {
        const char *e = strchr(q, '\n'); size_t len = e ? (size_t)(e - q) : strlen(q);
        if (parse_line(q, len, n, sizeof n, v, sizeof v) && !strcmp(n, name)) {
            if (value && *value && !done) on += (size_t)sprintf(o + on, "%s=%s\r\n", name, value);
            done = 1;
        } else {
            memcpy(o + on, q, len); on += len;
            if (e) o[on++] = '\n';
        }
        q = e ? e + 1 : NULL;
    }
    if (!done && value && *value) {
        if (on && o[on - 1] != '\n') { o[on++] = '\r'; o[on++] = '\n'; }
        if (!t) on += (size_t)sprintf(o + on, "; Tears of the Kingdom settings, shared by the launcher and the in-game menu.\r\n");
        on += (size_t)sprintf(o + on, "%s=%s\r\n", name, value);
    }
    free(t);
    char tmp[sizeof g_path + 8]; snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(o, 1, on, f) == on; if (f) ok &= fclose(f) == 0;
    free(o);
    if (!ok || !MoveFileExA(tmp, p, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return -1; }
    if (value && *value) { char x[4096]; expand(value, x, sizeof x); _putenv_s(name, x); } else _putenv_s(name, "");
    return 0;
}
