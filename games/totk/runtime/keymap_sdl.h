/* Windows keyboard -> the Mac virtual key codes runtime/input.c understands (C9). runtime/frontend_win.c gets SDL3
   scancodes, which are USB HID keyboard usage ids (SDL_SCANCODE_A = 4 ...), and passes runtime_input_key the code the
   Mac's NSEvent.keyCode (kVK_*) gives for the key in the same place, so the layout is the Mac one by position
   (input.c button(): J A, K / Space B, I X, U Y, Z / C stick clicks, Q L, E R, Left Alt ZL (Mac Option), Left Shift ZR,
   Return Plus, Tab Minus (Esc opens the in-game menu, runtime/overlay), F T H G D-pad, W A S D left stick, arrows right stick; 1-5 the shortcut keys).
   Pure C without SDL headers: tests/input_win/test_keymap.c covers it. 0xFF = no Mac key (ignored). */
#pragma once
#include <stdint.h>

enum { KEYMAP_NONE = 0xFF };

static inline unsigned keymap_mac_from_hid(unsigned usage) {
    /* letters: HID 4 (A) .. 29 (Z) */
    static const uint8_t letters[26] = {
        0 /*A*/, 11 /*B*/, 8 /*C*/, 2 /*D*/, 14 /*E*/, 3 /*F*/, 5 /*G*/, 4 /*H*/, 34 /*I*/, 38 /*J*/, 40 /*K*/, 37 /*L*/, 46 /*M*/,
        45 /*N*/, 31 /*O*/, 35 /*P*/, 12 /*Q*/, 15 /*R*/, 1 /*S*/, 17 /*T*/, 32 /*U*/, 9 /*V*/, 13 /*W*/, 7 /*X*/, 16 /*Y*/, 6 /*Z*/};
    /* digits: HID 30 (1) .. 39 (0) */
    static const uint8_t digits[10] = {18 /*1*/, 19 /*2*/, 20 /*3*/, 21 /*4*/, 23 /*5*/, 22 /*6*/, 26 /*7*/, 28 /*8*/, 25 /*9*/, 29 /*0*/};
    if (usage >= 4 && usage <= 29) return letters[usage - 4];
    if (usage >= 30 && usage <= 39) return digits[usage - 30];
    switch (usage) {
    case 40: return 36;    /* Return */
    case 41: return 53;    /* Escape */
    case 42: return 51;    /* Backspace (Mac Delete) */
    case 43: return 48;    /* Tab */
    case 44: return 49;    /* Space */
    case 45: return 27;    /* - */
    case 46: return 24;    /* = */
    case 47: return 33;    /* [ */
    case 48: return 30;    /* ] */
    case 49: return 42;    /* \ */
    case 51: return 41;    /* ; */
    case 52: return 39;    /* ' */
    case 53: return 50;    /* ` */
    case 54: return 43;    /* , */
    case 55: return 47;    /* . */
    case 56: return 44;    /* / */
    case 79: return 124;   /* Right arrow */
    case 80: return 123;   /* Left arrow */
    case 81: return 125;   /* Down arrow */
    case 82: return 126;   /* Up arrow */
    case 225: return 56;   /* Left Shift: ZR */
    case 226: return 58;   /* Left Alt (Mac Option): ZL */
    /* Other modifiers (Ctrl, Win, Caps Lock, the right-hand Shift / Alt) reach the game on neither platform: the Mac
       frontend forwards only kVK_Shift and kVK_Option from flagsChanged. */
    default: return KEYMAP_NONE;
    }
}
