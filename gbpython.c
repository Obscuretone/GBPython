#include <gb/gb.h>
#include <gbdk/console.h>
#include <gbdk/font.h>
#include <stdio.h>
#include <string.h>
#include "gbpython.h"

/* UI and OSK Engine Definitions.
   4x15 layout: alphanumerics on the left, specials on the right, freeing
   two screen rows for the output window. */
char osk_grid[OSK_ROWS][OSK_COLS] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', '-', '*', '/', '='},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '(', ')', '<', '>', '%'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', ':', '\'', '"', ',', '.'},
    {'z', 'x', 'c', 'v', 'b', 'n', 'm', '!', '?', '_', '[', ']', '{', '}', '\n'}
};

uint8_t cursor_row = 0;
uint8_t cursor_col = 0;
/* The ROM boots with fizzbuzz pre-typed: cursor onto the run key and go */
char input_buffer[INPUT_MAX + 1] =
    "for i in range(1,16):\n"
    "    if i%15==0: print('fizzbuzz')\n"
    "    elif i%3==0: print('fizz')\n"
    "    elif i%5==0: print('buzz')\n"
    "    else: print(i)";
uint8_t input_len = 0; /* set from the buffer at boot */
uint8_t runs_done = 0;   /* increments when a run finishes (tests poll it) */
uint8_t program_ran = 0; /* last RUN wasn't followed by an edit */

/* Play-triangle glyph for the RUN key, loaded into font tile slot 96
   (the spectrum font uses 0-95; the boot-time inversion loop copies 0-127
   to 128-255, so the highlighted version at slot 224 comes for free). */
#define TILE_RUN 96
static const unsigned char run_tile[16] = {
    0x00, 0x00,
    0x40, 0x40,
    0x70, 0x70,
    0x7C, 0x7C,
    0x7C, 0x7C,
    0x70, 0x70,
    0x40, 0x40,
    0x00, 0x00
};

void draw_osk(void) {
    uint8_t r, c;
    for (r = 0; r < OSK_ROWS; r++) {
        gotoxy(OSK_X, OSK_Y + r);
        for (c = 0; c < OSK_COLS; c++) {
            char display_char = osk_grid[r][c];
            if (display_char == '\n') {
                /* the RUN key shows a play-triangle tile */
                uint8_t t = TILE_RUN;
                if (r == cursor_row && c == cursor_col) {
                    t += 128; /* inverted copy */
                }
                set_bkg_tiles(OSK_X + c, OSK_Y + r, 1, 1, &t);
                gotoxy(OSK_X + c + 1, OSK_Y + r);
                continue;
            }

            if (r == cursor_row && c == cursor_col) {
                uint8_t char_tile;

                /* 1. Print standard cell */
                printf("%c", display_char);

                /* 2. Read back the exact tile index from VRAM screen map */
                get_bkg_tiles(OSK_X + c, OSK_Y + r, 1, 1, &char_tile);

                /* 3. Apply inversion offset (standard font 0-127 mapped to inverted 128-255) */
                if (char_tile < 128) {
                    char_tile += 128;
                }

                /* 4. Write inverted tile back to the screen */
                set_bkg_tiles(OSK_X + c, OSK_Y + r, 1, 1, &char_tile);

                /* 5. Advance stdout cursor to maintain layout synchronization */
                gotoxy(OSK_X + c + 1, OSK_Y + r);
            } else {
                printf("%c", display_char);
            }
        }
    }
}


void main(void) {
    uint8_t keys;
    uint8_t last_keys = 0;
    uint8_t pressed;
    uint8_t select_used_as_shift = 0;
    char selected_char;

    unsigned char tile_buf[16];
    uint16_t t;
    uint8_t b;

    input_len = (uint8_t)strlen(input_buffer);

    /* Initialize Custom Font (Retro ZX Spectrum style) */
    font_init();
    font_set(font_load(font_spect));

    /* Custom RUN glyph in the unused slot after the font, so the
       inversion loop below covers it too */
    set_bkg_data(TILE_RUN, 1, run_tile);

    /* Launch Animated Splash Screen bootstage */
    splash_screen();

    /* Dynamically generate inverted font in VRAM background slots 128-255.
       Must happen AFTER the splash: the splash parks its logo bitmap in
       slots 128-143, which would otherwise clobber the inverted glyphs for
       ASCII 0x20-0x2F (space and most punctuation). */
    for (t = 0; t < 128; t++) {
        get_bkg_data(t, 1, tile_buf);
        for (b = 0; b < 16; b++) {
            tile_buf[b] = ~tile_buf[b];
        }
        set_bkg_data(t + 128, 1, tile_buf);
    }

    /* Initialize Screen elements (every string exactly 20 cols, positioned
       with gotoxy, so the console never wraps or scrolls) */
    gotoxy(0, 0);
    printf("-- GBPython REPL --");
    gotoxy(0, 6);
    printf("--- Out ------------");
    gotoxy(0, 12);
    printf("--------------------");

    draw_input_buffer();
    draw_osk();

    while(1) {
        keys = joypad();
        pressed = keys & ~last_keys;

        if (pressed & J_UP) {
            if (cursor_row > 0) cursor_row--;
            draw_osk();
        }
        if (pressed & J_DOWN) {
            if (cursor_row < OSK_ROWS - 1) cursor_row++;
            draw_osk();
        }
        if (pressed & J_LEFT) {
            cursor_col = cursor_col > 0 ? cursor_col - 1 : OSK_COLS - 1;
            draw_osk();
        }
        if (pressed & J_RIGHT) {
            cursor_col = cursor_col < OSK_COLS - 1 ? cursor_col + 1 : 0;
            draw_osk();
        }

        if (pressed & J_A) {
            selected_char = osk_grid[cursor_row][cursor_col];
            if (selected_char == '\n') {
                if (program_ran) {
                    /* second RUN press: clear out the old program */
                    input_len = 0;
                    input_buffer[0] = '\0';
                    draw_input_buffer();
                    program_ran = 0;
                } else {
                    run_interpreter();
                    program_ran = 1;
                }
            } else {
                program_ran = 0; /* editing keeps the program for re-run */
                /* SELECT held while typing acts as shift: uppercase */
                if (keys & J_SELECT) {
                    if (selected_char >= 'a' && selected_char <= 'z') {
                        selected_char = selected_char - 'a' + 'A';
                    }
                    select_used_as_shift = 1;
                }
                if (input_len < INPUT_MAX) {
                    input_buffer[input_len++] = selected_char;
                    input_buffer[input_len] = '\0';
                    draw_input_buffer();
                }
            }
        }

        if (pressed & J_B) {
            program_ran = 0;
            if (input_len > 0) {
                input_len--;
                input_buffer[input_len] = '\0';
                draw_input_buffer();
            }
        }

        /* SELECT: tap = space, hold + A = shift. Space is typed on release
           so starting a shift chord doesn't also insert a space. */
        if (pressed & J_SELECT) {
            select_used_as_shift = 0;
        }
        if ((last_keys & J_SELECT) && !(keys & J_SELECT)) {
            if (!select_used_as_shift && input_len < INPUT_MAX) {
                program_ran = 0;
                input_buffer[input_len++] = ' ';
                input_buffer[input_len] = '\0';
                draw_input_buffer();
            }
        }

        if (pressed & J_START) {
            /* START types a newline: multi-line programs without leaving
               the keyboard */
            if (input_len < INPUT_MAX) {
                program_ran = 0;
                input_buffer[input_len++] = '\n';
                input_buffer[input_len] = '\0';
                draw_input_buffer();
            }
        }

        last_keys = keys;
        wait_vbl_done();
    }
}
