/* Keyboard rebinding lines of the controls file (runtime/keyboard_remap.h). */
#include "keyboard_remap.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void fresh(uint8_t t[256]) { memset(t, KEYREMAP_KEEP, 256); }

int main(void) {
    uint8_t t[256]; const char *err = NULL; char nb[2];
    fresh(t);
    CHECK(keyremap_usage("j", 1) == 13 && keyremap_usage("J", 1) == 13 && keyremap_usage("0", 1) == 39 && keyremap_usage("1", 1) == 30);
    CHECK(keyremap_usage("Space", 5) == 44 && keyremap_usage("lshift", 6) == 225 && keyremap_usage("f13", 3) == 0);
    CHECK(!strcmp(keyremap_name_of(13, nb), "j") && !strcmp(keyremap_name_of(225, nb), "lshift") && keyremap_name_of(200, nb) == NULL);
    /* defaults: J = a, K = b, Enter = plus, Left Shift = zr */
    CHECK(keyremap_button_of(t, 13) == 0 && keyremap_button_of(t, 14) == 1 && keyremap_button_of(t, 40) == 10 && keyremap_button_of(t, 225) == 9);
    CHECK(keyremap_button_of(t, 7) == -1);   /* D: left stick in the game, no button */
    CHECK(keyremap_key_of(t, 0) == 13);
    /* parse */
    CHECK(keyremap_parse("# note\nsouth = a\nkey l = a\nKEY\tj = none   # J off\n", t, &err) == 0);
    CHECK(keyremap_button_of(t, 15) == 0 && keyremap_button_of(t, 13) == -1 && keyremap_key_of(t, 0) == 15);
    uint8_t before[256]; memcpy(before, t, 256);
    CHECK(keyremap_parse("key j = a\nkey nosuch = b\n", t, &err) == 2 && err && !memcmp(before, t, 256));
    CHECK(keyremap_parse("key j = q\n", t, &err) == 1);
    CHECK(keyremap_parse("key j a\n", t, &err) == 1);
    /* bind swaps: A to K (K was B) -> B gets A's old key */
    fresh(t);
    keyremap_bind(t, 0, 14);
    CHECK(keyremap_button_of(t, 14) == 0 && keyremap_button_of(t, 13) == 1 && keyremap_key_of(t, 1) == 13);
    /* binding back restores the defaults (no lines left) */
    keyremap_bind(t, 0, 13);
    int lines = 0; for (int u = 0; u < 256; u++) lines += t[u] != KEYREMAP_KEEP;
    CHECK(keyremap_button_of(t, 13) == 0 && keyremap_button_of(t, 14) == 1 && lines == 0);
    /* a key without a button takes one: the old key is left unused */
    fresh(t);
    keyremap_bind(t, 0, 7);
    CHECK(keyremap_button_of(t, 7) == 0 && keyremap_button_of(t, 13) == -1);
    if (fails) return 1;
    puts("keyboard_remap: ok");
    return 0;
}
