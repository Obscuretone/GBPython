/* gbpython banked UI: splash screen, input-buffer editor rendering, and
   the input() line editor. Called rarely, so the BANKED overhead doesn't
   matter; keeps ROM bank 0 free for the C library (incl. software float). */

#pragma bank 3

#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include "gbpython.h"

void draw_input_buffer(void) BANKED {
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

void splash_screen(void) BANKED {
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
    printf("      v4.1.0        ");

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
   Start or the RUN key submits. */
void ui_input_line(char* dst, uint8_t maxlen) BANKED {
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
