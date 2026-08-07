#include <gb/gb.h>
#include <gbdk/console.h>
#include <gbdk/font.h>
#include <stdio.h>
#include <string.h>
#include "gbpython.h"

/* UI and OSK Engine Definitions.
   4x15 layout: alphanumerics on the left, specials on the right, freeing
   two screen rows for the output window. */
#define OSK_ROWS 4
#define OSK_COLS 15
#define OSK_X 2
#define OSK_Y 13

char osk_grid[OSK_ROWS][OSK_COLS] = {
    {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', '-', '*', '/', '='},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '(', ')', '<', '>', '%'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', ':', '\'', '"', ',', '.'},
    {'z', 'x', 'c', 'v', 'b', 'n', 'm', '!', '?', '_', '[', ']', '{', '}', '\n'}
};

uint8_t cursor_row = 0;
uint8_t cursor_col = 0;
char input_buffer[INPUT_MAX + 1] = "";
uint8_t input_len = 0;

/* Return-arrow glyph for the newline key, loaded into font tile slot 96
   (the spectrum font uses 0-95; the boot-time inversion loop copies 0-127
   to 128-255, so the highlighted version at slot 224 comes for free). */
#define TILE_RETURN_ARROW 96
static const unsigned char return_arrow_tile[16] = {
    0x00, 0x00,
    0x06, 0x06,
    0x06, 0x06,
    0x26, 0x26,
    0x7E, 0x7E,
    0x7E, 0x7E,
    0x20, 0x20,
    0x00, 0x00
};

void draw_osk(void) {
    uint8_t r, c;
    for (r = 0; r < OSK_ROWS; r++) {
        gotoxy(OSK_X, OSK_Y + r);
        for (c = 0; c < OSK_COLS; c++) {
            char display_char = osk_grid[r][c];
            if (display_char == '\n') {
                /* newline key shows a custom return-arrow tile */
                uint8_t t = TILE_RETURN_ARROW;
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

void draw_input_buffer(void) {
    uint8_t y, line, col;
    uint8_t i;
    uint8_t total_lines = 0;
    uint8_t skip = 0;

    for (y = 2; y <= 5; y++) {
        gotoxy(0, y);
        printf("                    ");
    }

    /* Long programs don't fit the 4 visible rows; show the tail so the
       user always sees what they're typing. First pass: count lines. */
    col = 0;
    for (i = 0; i < input_len; i++) {
        if (input_buffer[i] == '\n' || col == 20) {
            total_lines++;
            col = 0;
            if (input_buffer[i] == '\n') continue;
        }
        col++;
    }
    if (total_lines > 3) {
        skip = total_lines - 3;
    }

    line = 0;
    col = 0;
    gotoxy(0, 2);
    for (i = 0; i < input_len; i++) {
        if (input_buffer[i] == '\n' || col == 20) {
            line++;
            col = 0;
            if (line >= skip) {
                gotoxy(0, line - skip + 2);
            }
            if (input_buffer[i] == '\n') continue;
        }
        if (line >= skip) {
            putchar(input_buffer[i]);
        }
        col++;
    }

    if (col < 20 && line >= skip) {
        gotoxy(col, line - skip + 2);
        putchar('_');
    }
}


/* 4x4 Pixel-art Python Logo (16 tiles * 16 bytes per tile) */
static const unsigned char python_logo_tiles[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x1F, 0x1F, 0x60, 0x7F, 0x80, 0xFF, 0x80, 0xFD, 0x80, 0xFF, 0x80, 0xFF, 0x81, 0xFF,
    0x00, 0x00, 0xE0, 0xE0, 0x18, 0xF8, 0x04, 0xFC, 0x04, 0xFC, 0x04, 0xFC, 0xFC, 0xFC, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x3F, 0x3F, 0x40, 0x7F, 0x40, 0x7F, 0x40, 0x7F, 0x40, 0x7F, 0x40, 0x7F, 0x7F, 0x7F,
    0x80, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xE0, 0xFF,
    0xFC, 0xFC, 0x04, 0xFC, 0x08, 0xF8, 0x08, 0xF8, 0x08, 0xF8, 0x08, 0xF8, 0x08, 0xF8, 0x08, 0xF8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1F, 0x10, 0x1F, 0x10, 0x1F, 0x10, 0x1F, 0x10, 0x1F, 0x10, 0x1F, 0x10, 0x1F, 0x10, 0x3F, 0x20,
    0xFF, 0x07, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00,
    0xFE, 0xFE, 0xFE, 0x02, 0xFE, 0x02, 0xFE, 0x02, 0xFE, 0x02, 0xFE, 0x02, 0xFE, 0x02, 0xFC, 0xFC,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3F, 0x3F, 0x00, 0x00, 0x3F, 0x3F, 0x3F, 0x20, 0x3F, 0x20, 0x1F, 0x18, 0x07, 0x07, 0x00, 0x00,
    0xFF, 0x01, 0xFF, 0x81, 0xFF, 0x01, 0x7F, 0x01, 0xFF, 0x01, 0xFE, 0x06, 0xF8, 0xF8, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

void splash_screen(void) {
    uint8_t i;
    uint8_t keys;
    uint8_t frame_counter = 0;
    uint8_t show_press_start = 1;

    /* 4x4 Tile index map referencing tiles 128 to 143 */
    const uint8_t python_logo_map[16] = {
        128, 129, 130, 131,
        132, 133, 134, 135,
        136, 137, 138, 139,
        140, 141, 142, 143
    };

    /* Clear screen to blank tiles */
    for (i = 0; i < 18; i++) {
        gotoxy(0, i);
        printf("                    ");
    }

    /* Draw text titles around the bitmap area */
    gotoxy(0, 1);
    printf("    GBPython VM     ");
    gotoxy(0, 2);
    printf("      v3.1.0        ");

    /* Load Python logo bitmap tiles into VRAM background slot 128 */
    set_bkg_data(128, 16, python_logo_tiles);

    /* Blit 4x4 tile map to center background (x=8, y=4, width=4, height=4) */
    set_bkg_tiles(8, 4, 4, 4, python_logo_map);

    gotoxy(0, 9);
    printf("  Retro Interpreter ");
    gotoxy(0, 10);
    printf("  on Game Boy DMG   ");

    /* Blinking loop waiting for START button */
    while (1) {
        keys = joypad();
        if (keys & J_START) {
            break;
        }

        frame_counter++;
        if (frame_counter == 25) {
            frame_counter = 0;
            show_press_start = !show_press_start;
            gotoxy(0, 13);
            if (show_press_start) {
                printf("    PRESS START     ");
            } else {
                printf("                    ");
            }
        }
        wait_vbl_done();
    }

    /* Smooth 30 FPS vertical transition wipe.
       Blank via set_bkg_tiles instead of printf: printing a full-width line
       on the bottom row makes the gbdk console scroll and wrecks the layout. */
    for (i = 0; i < 18; i++) {
        static const uint8_t blank_row[20] = {0}; /* tile 0 == space */
        set_bkg_tiles(0, i, 20, 1, blank_row);
        wait_vbl_done();
        wait_vbl_done();
    }
    gotoxy(0, 0);
}

/* Interactive line entry for the interpreter's input() builtin. Runs its
   own little event loop on the OSK; the entry line is drawn over the last
   output-window row (the "? " line input() just printed). Same controls as
   the main editor: A types, B deletes, Select+A shifts, tap-Select spaces,
   Start or the return-arrow key submits. */
void ui_input_line(char* dst, uint8_t maxlen) {
    uint8_t keys;
    uint8_t last_keys;
    uint8_t pressed;
    uint8_t len = 0;
    uint8_t used_shift = 0;
    uint8_t dirty = 1;
    uint8_t row = 7 + (out_count ? out_count - 1 : 0);
    char ch;

    dst[0] = '\0';
    last_keys = joypad(); /* START may still be held from launching the run */

    while (1) {
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
            ch = osk_grid[cursor_row][cursor_col];
            if (ch == '\n') break; /* return arrow submits */
            if (keys & J_SELECT) {
                if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
                used_shift = 1;
            }
            if (len < maxlen) {
                dst[len++] = ch;
                dst[len] = '\0';
                dirty = 1;
            }
        }
        if (pressed & J_B) {
            if (len > 0) {
                dst[--len] = '\0';
                dirty = 1;
            }
        }
        if (pressed & J_SELECT) {
            used_shift = 0;
        }
        if ((last_keys & J_SELECT) && !(keys & J_SELECT)) {
            if (!used_shift && len < maxlen) {
                dst[len++] = ' ';
                dst[len] = '\0';
                dirty = 1;
            }
        }
        if (pressed & J_START) break;

        if (dirty) {
            uint8_t i;
            gotoxy(0, row);
            printf("? %s_", dst);
            for (i = len + 3; i < 20; i++) putchar(' ');
            dirty = 0;
        }

        last_keys = keys;
        wait_vbl_done();
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

    /* Initialize Custom Font (Retro ZX Spectrum style) */
    font_init();
    font_set(font_load(font_spect));

    /* Custom return-arrow glyph in the unused slot after the font, so the
       inversion loop below covers it too */
    set_bkg_data(TILE_RETURN_ARROW, 1, return_arrow_tile);

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
            /* SELECT held while typing acts as shift: uppercase letters */
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

        if (pressed & J_B) {
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
                input_buffer[input_len++] = ' ';
                input_buffer[input_len] = '\0';
                draw_input_buffer();
            }
        }

        if (pressed & J_START) {
            run_interpreter();
        }

        last_keys = keys;
        wait_vbl_done();
    }
}
