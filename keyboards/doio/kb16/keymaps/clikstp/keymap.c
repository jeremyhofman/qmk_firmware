/* Copyright 2022 DOIO
 * Copyright 2022 HorrorTroll <https://github.com/HorrorTroll>
 * Copyright 2026 jeremyhofman
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * CLIKSTP keymap for the DOIO KB16 rev2.
 *
 * Everything goes to the CLIKSTP app over Raw HID; the pad itself types nothing.
 *   - Encoders: turns and presses, as before (controls 0 = left, 1 = right, 2 = big).
 *   - Keys 2..16: press and release (controls 17..31; key n is control 15 + n).
 *   - Key 1: the RGB layer. Hold for momentary, double-tap to latch on, double-tap again to leave.
 *     In the RGB layer nothing is sent to the app: the encoders and keys adjust the lighting.
 *   - Hold keys 13 and 16 together for 3 seconds: bootloader (for flashing), from the normal layer.
 *   - The OLED shows the app's text (the selected tool, and a readout) while the app is connected,
 *     "RGB" in the RGB layer, and the stock layer display otherwise.
 * Packet layouts are in docs/pad-protocol.md and must match HidProtocol in the app.
 */

#include QMK_KEYBOARD_H
#include <string.h>

// OLED: the stock layer display, used when the app isn't connected
#include "lib/layer_status/layer_status.h"

#include "raw_hid.h"

#define HID_PACKET_SIZE 32

enum layer_names {
    _BASE,
    _RGB,
};

// Message types (byte 0) and input events. Keep in step with HidProtocol in the app.
enum { MSG_INPUT = 0x01, MSG_FEEDBACK = 0x02, MSG_HEARTBEAT = 0x03, MSG_DISPLAY = 0x04 };
enum { EVT_CW = 1, EVT_CCW = 2, EVT_PRESS = 3, EVT_RELEASE = 4 };

// Key n (1..16) reports as control (KEY_CONTROL_BASE - 1 + n), i.e. 16..31.
#define KEY_CONTROL_BASE 16

// The app counts as connected while heartbeats or display text keep arriving.
#define HOST_TIMEOUT_MS 3000
// Hold keys 13 and 16 this long to enter the bootloader.
#define BOOT_HOLD_MS 3000

// Custom keycodes: the three encoder presses, then keys 2..16 (key 1 is the layer key).
enum custom_keycodes {
    ENC_L = QK_USER_0,
    ENC_R,
    ENC_BIG,
    PK_2, PK_3, PK_4, PK_5, PK_6, PK_7, PK_8, PK_9,
    PK_10, PK_11, PK_12, PK_13, PK_14, PK_15, PK_16,
};

static uint8_t key_number(uint16_t keycode) {
    return (keycode >= PK_2 && keycode <= PK_16) ? (uint8_t)(keycode - PK_2 + 2) : 0;
}

static void send_input(uint8_t control, uint8_t event, uint8_t count) {
    uint8_t data[HID_PACKET_SIZE] = {0};
    data[0] = MSG_INPUT;
    data[1] = control;
    data[2] = event;
    data[3] = count;
    raw_hid_send(data, HID_PACKET_SIZE);
}

// --- From the app: display text and heartbeat ----------------------------------------------------

#define DISPLAY_COLS 21   // 128 px / 6 px per character
static char     display_text[2][DISPLAY_COLS + 1];
static bool     display_dirty = true;
static uint32_t host_seen     = 0;
static bool     host_ever     = false;

static bool host_alive(void) {
    return host_ever && timer_elapsed32(host_seen) < HOST_TIMEOUT_MS;
}

static void note_host(void) {
    host_seen = timer_read32();
    host_ever = true;
}

void raw_hid_receive(uint8_t *data, uint8_t length) {
    if (length < 1) return;
    switch (data[0]) {
        case MSG_HEARTBEAT:
            note_host();
            break;

        case MSG_DISPLAY: {
            // [0]=MSG_DISPLAY [1]=line (0 = tool, 1 = readout) [2..]=text, NUL-terminated or up to the packet end
            if (length < 3 || data[1] > 1) break;
            char   *line = display_text[data[1]];
            uint8_t n    = 0;
            for (uint8_t i = 2; i < length && n < DISPLAY_COLS && data[i] != 0; i++) {
                char c    = (char)data[i];
                line[n++] = (c >= 32 && c < 127) ? c : '?';   // the OLED font is ASCII
            }
            line[n]       = '\0';
            display_dirty = true;
            note_host();
            break;
        }

        case MSG_FEEDBACK:
            // reserved: per-tool RGB colour
            break;
    }
}

// --- Encoders --------------------------------------------------------------------------------------

bool encoder_update_user(uint8_t index, bool clockwise) {
    if (layer_state_is(_RGB)) {
        // Lighting only; nothing goes to the app, so a frequency can't change while dimming.
        switch (index) {
            case 0: clockwise ? rgb_matrix_increase_hue() : rgb_matrix_decrease_hue(); break;
            case 1: clockwise ? rgb_matrix_increase_sat() : rgb_matrix_decrease_sat(); break;
            case 2: clockwise ? rgb_matrix_increase_val() : rgb_matrix_decrease_val(); break;
        }
        return false;
    }
    send_input(index, clockwise ? EVT_CW : EVT_CCW, 1);
    return false;   // skip the default encoder action
}

// --- Keys ------------------------------------------------------------------------------------------

static bool     k13_down = false, k16_down = false;
static uint16_t boot_timer = 0;
static bool     boot_pending = false;

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    bool    pressed = record->event.pressed;
    uint8_t event   = pressed ? EVT_PRESS : EVT_RELEASE;

    switch (keycode) {
        case ENC_L:   send_input(0, event, 1); return false;
        case ENC_R:   send_input(1, event, 1); return false;
        case ENC_BIG: send_input(2, event, 1); return false;
    }

    uint8_t n = key_number(keycode);
    if (n) {
        // Bootloader chord: 13 + 16 held together (normal layer; in the RGB layer key 16 is brightness down).
        bool was_both = k13_down && k16_down;
        if (n == 13) k13_down = pressed;
        if (n == 16) k16_down = pressed;
        if (!was_both && k13_down && k16_down) boot_timer = timer_read();

        if (!layer_state_is(_RGB)) send_input(KEY_CONTROL_BASE - 1 + n, event, 1);
        return false;
    }
    return true;
}

void matrix_scan_user(void) {
    if (k13_down && k16_down && !boot_pending && timer_elapsed(boot_timer) > BOOT_HOLD_MS) {
        boot_pending = true;
#ifdef OLED_ENABLE
        oled_clear();
        oled_set_cursor(0, 1);
        oled_write_P(PSTR("     BOOTLOADER      "), false);
        oled_set_cursor(0, 2);
        oled_write_P(PSTR("   ready to flash    "), false);
        oled_render_dirty(true);
#endif
        wait_ms(300);
        reset_keyboard();   // same as QK_BOOT
    }
}

// --- Keymap ----------------------------------------------------------------------------------------

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {

/*
       ┌───┬───┬───┬───┐   ┌───┐ ┌───┐
       │RGB│ 2 │ + │ - │   │ L │ │ R │     L/R/Big: encoder presses
       ├───┼───┼───┼───┤   └───┘ └───┘
       │ 5 │ 6 │D->│MNU│
       ├───┼───┼───┼───┤
       │MSG│10 │CLR│ENT│      ┌───┐
       ├───┼───┼───┼───┤      │Big│
       │CDI│OBS│FPL│PRC│      └───┘
       └───┴───┴───┴───┘
       Legends are what the app's GPS tools do today; every key just reports its number.
       Hold 13 + 16 for 3 s: bootloader.
*/
    [_BASE] = LAYOUT(
                TT(_RGB), PK_2,  PK_3,  PK_4,  ENC_L,
                PK_5,     PK_6,  PK_7,  PK_8,  ENC_R,
                PK_9,     PK_10, PK_11, PK_12, ENC_BIG,
                PK_13,    PK_14, PK_15, PK_16
            ),

/*
       RGB layer: hold key 1, or double-tap it to latch (double-tap again to leave).
       The keys match DOIO's stock Fn2 layer; the encoders do the same jobs.
       ┌───┬───┬───┬───┐   ┌───┐ ┌───┐
       │   │Spi│Spd│Clr│   │Nxt│ │Prv│     encoder presses: next / previous effect, Big: on/off
       ├───┼───┼───┼───┤   └───┘ └───┘     Clr (key 4): erase saved settings (EE_CLR) and start fresh
       │Sai│Sad│   │BLd│
       ├───┼───┼───┼───┤                   Left encoder: hue   Right encoder: saturation
       │Tog│Mod│Hui│   │      ┌───┐        Big encoder: brightness
       ├───┼───┼───┼───┤      │Tog│
       │   │Vai│Hud│Vad│      └───┘        Hue and saturation only show in effects that use them
       └───┴───┴───┴───┘                   (a solid colour, say); rainbow effects pick their own colours.
*/
    [_RGB] = LAYOUT(
                _______, RM_SPDU, RM_SPDD, EE_CLR,  RM_NEXT,
                RM_SATU, RM_SATD, XXXXXXX, QK_BOOT, RM_PREV,
                RM_TOGG, RM_NEXT, RM_HUEU, XXXXXXX, RM_TOGG,
                XXXXXXX, RM_VALU, RM_HUED, RM_VALD
            ),
};

// --- OLED --------------------------------------------------------------------------------------------

#ifdef OLED_ENABLE
enum { SCREEN_NONE, SCREEN_STOCK, SCREEN_APP, SCREEN_RGB };
static uint8_t last_screen = SCREEN_NONE;

// One full row, centred and padded, so nothing from the previous text is left behind.
static void write_row(uint8_t row, const char *text) {
    char    out[DISPLAY_COLS + 1];
    uint8_t len = (uint8_t)strlen(text);
    if (len > DISPLAY_COLS) len = DISPLAY_COLS;
    uint8_t left = (DISPLAY_COLS - len) / 2;
    memset(out, ' ', DISPLAY_COLS);
    memcpy(out + left, text, len);
    out[DISPLAY_COLS] = '\0';
    oled_set_cursor(0, row);
    oled_write(out, false);
}

static void render_app(void) {
    if (!display_dirty) return;
    write_row(0, "");
    write_row(1, display_text[0]);   // the selected tool
    write_row(2, "");
    write_row(3, display_text[1]);   // its readout, if any
    display_dirty = false;
}

static void render_rgb(void) {
    char line[DISPLAY_COLS + 1];
    write_row(0, "RGB LIGHTS");
    strcpy(line, "H");
    strcat(line, get_u8_str(rgb_matrix_get_hue(), ' '));
    strcat(line, " S");
    strcat(line, get_u8_str(rgb_matrix_get_sat(), ' '));
    strcat(line, " V");
    strcat(line, get_u8_str(rgb_matrix_get_val(), ' '));
    write_row(1, line);
    strcpy(line, rgb_matrix_is_enabled() ? "on, effect " : "off, effect ");
    strcat(line, get_u8_str(rgb_matrix_get_mode(), ' '));
    write_row(2, line);
    write_row(3, "2x key 1: back");
}

bool oled_task_user(void) {
    uint8_t screen = layer_state_is(_RGB) ? SCREEN_RGB : host_alive() ? SCREEN_APP : SCREEN_STOCK;
    if (screen != last_screen) {
        oled_clear();
        display_dirty = true;
        last_screen   = screen;
    }

    switch (screen) {
        case SCREEN_RGB: render_rgb(); break;
        case SCREEN_APP: render_app(); break;
        default:         render_layer_status(); break;
    }
    return false;
}
#endif