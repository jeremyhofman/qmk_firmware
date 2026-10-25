/* CLIKSTP keymap settings for the DOIO KB16 rev2. */
# pragma once

// Key 1 is TT(_RGB): hold for momentary, tap this many times to toggle to the RGB layer.
// Layer settings for RGB then shows on the LCD panel of the DOIO device.
# define TAPPING_TOGGLE 2

// The OLED turns off after this long without a key or encoder interaction.
#define OLED_TIMEOUT 90000

#define WS2818_BITBANG_NOP_FUDGE 0.21     // middle of the working range
#define WS2812_TIMING 1650                // longer low phases: 1 bit low 2 loops, 0 bit low 4