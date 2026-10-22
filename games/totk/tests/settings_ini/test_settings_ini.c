/* settings.ini: parsing, environment precedence, %~dp0, in-place rewrite (runtime/win/settings_ini.c). */
#include "runtime/win/settings_ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char got[16][2][256]; static int ngot;
static void collect(const char *n, const char *v, void *u) { (void)u; if (ngot < 16) { snprintf(got[ngot][0], 256, "%s", n); snprintf(got[ngot][1], 256, "%s", v); ngot++; } }

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    static char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b;
}

int main(void) {
    settings_ini_parse("; comment\r\n[settings]\r\n  TOTK_A = 1 \r\n# x\nTOTK_B=a=b\nbad line\nTOTK_C=\nWITH SPACE=1\n=nameless\nTOTK_D=last", collect, NULL);
    CHECK(ngot == 4);
    CHECK(!strcmp(got[0][0], "TOTK_A") && !strcmp(got[0][1], "1"));
    CHECK(!strcmp(got[1][0], "TOTK_B") && !strcmp(got[1][1], "a=b"));
    CHECK(!strcmp(got[2][0], "TOTK_C") && !strcmp(got[2][1], ""));
    CHECK(!strcmp(got[3][0], "TOTK_D") && !strcmp(got[3][1], "last"));

    char dir[MAX_PATH], path[MAX_PATH + 32];
    GetTempPathA(sizeof dir, dir);
    snprintf(path, sizeof path, "%stotk_settings_test_%lu\\", dir, GetCurrentProcessId());
    CreateDirectoryA(path, NULL);
    strcat(path, "settings.ini");
    _putenv_s("TOTK_SETTINGS_INI", path);
    FILE *f = fopen(path, "wb");
    fputs("; test\r\nTOTK_T_FROMFILE=7\r\nTOTK_T_ENVWINS=file\r\nTOTK_T_DIR=%~dp0data\r\n", f); fclose(f);
    _putenv_s("TOTK_T_ENVWINS", "env");
    CHECK(settings_ini_load() == 2);
    CHECK(getenv("TOTK_T_FROMFILE") && !strcmp(getenv("TOTK_T_FROMFILE"), "7"));
    CHECK(!strcmp(getenv("TOTK_T_ENVWINS"), "env"));
    char want[MAX_PATH + 32]; snprintf(want, sizeof want, "%s", path); strcpy(strrchr(want, '\\') + 1, "data");
    CHECK(getenv("TOTK_T_DIR") && !strcmp(getenv("TOTK_T_DIR"), want));

    CHECK(settings_ini_set("TOTK_T_FROMFILE", "9") == 0);
    CHECK(settings_ini_set("TOTK_T_NEW", "x y") == 0);
    CHECK(settings_ini_set("TOTK_T_ENVWINS", NULL) == 0);
    char *t = slurp(path);
    CHECK(t && strstr(t, "; test\r\n") == t);
    CHECK(t && strstr(t, "TOTK_T_FROMFILE=9\r\n") && !strstr(t, "=7"));
    CHECK(t && strstr(t, "TOTK_T_NEW=x y\r\n"));
    CHECK(t && !strstr(t, "TOTK_T_ENVWINS"));
    CHECK(t && strstr(t, "TOTK_T_DIR=%~dp0data"));
    CHECK(!strcmp(getenv("TOTK_T_FROMFILE"), "9") && !getenv("TOTK_T_ENVWINS"));

    remove(path); *strrchr(path, '\\') = 0; RemoveDirectoryA(path);
    if (fails) return 1;
    puts("settings_ini: ok");
    return 0;
}
