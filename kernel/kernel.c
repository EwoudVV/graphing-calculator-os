/*
 * kernel.c - GraphCalcOS kernel
 *
 * NOW IN GRAPHICS MODE!
 * instead of writing characters to 0xB8000 (text buffer),
 * we write individual pixels to 0xA0000 (graphics framebuffer).
 * every single thing on screen - text, lines, graphs - is drawn
 * pixel by pixel by us.
 */

#include <stdint.h>
#include "keyboard.h"
#include "font.h"   /* includes vga.h and ports.h */

/* layout constants - where things go on screen
 *
 * screen is 320x200 pixels. we split it into:
 *   - top: title area
 *   - middle: graph area (where graphs will be drawn)
 *   - bottom: input bar (where you type equations)
 */
#define INPUT_BAR_Y     184   /* y position of the input bar */
#define INPUT_TEXT_Y     188  /* y position of text inside the bar */
#define INPUT_TEXT_X     24   /* x position of text (after "> ") */
#define MAX_INPUT_LEN    34   /* max characters that fit in the input bar */

/* draw the input bar at the bottom of the screen */
void draw_input_bar(void) {
    /* dark gray background bar */
    vga_fill_rect(0, INPUT_BAR_Y, SCREEN_WIDTH, 16, COLOR_DARK_GRAY);

    /* draw the prompt "> " */
    draw_string(8, INPUT_TEXT_Y, ">", COLOR_WHITE);
}

/* draw a simple blinking cursor (just a vertical line) */
void draw_cursor(int char_pos, uint8_t color) {
    int x = INPUT_TEXT_X + char_pos * FONT_WIDTH;
    vga_draw_vline(x, INPUT_TEXT_Y, FONT_HEIGHT, color);
}

/* clear the text area of the input bar (not the prompt) */
void clear_input_text(void) {
    vga_fill_rect(INPUT_TEXT_X, INPUT_TEXT_Y, SCREEN_WIDTH - INPUT_TEXT_X - 4, FONT_HEIGHT, COLOR_DARK_GRAY);
}

// kernel entry point, called from boot.asm
void kmain(void) {
    /* === SWITCH TO GRAPHICS MODE === */
    vga_init_graphics();
    vga_clear(COLOR_BLACK);

    /* draw the title */
    draw_string(8, 4, "GraphCalcOS", COLOR_LIGHT_GREEN);
    draw_string(8, 16, "type something and press enter!", COLOR_LIGHT_GRAY);

    /* draw a horizontal line to separate title from graph area */
    vga_draw_hline(0, 28, SCREEN_WIDTH, COLOR_DARK_GRAY);

    /* draw the input bar */
    draw_input_bar();

    /* input state */
    char input_buf[MAX_INPUT_LEN + 1];  /* +1 for null terminator */
    int input_len = 0;
    input_buf[0] = '\0';

    draw_cursor(0, COLOR_WHITE);

    /* main kernel loop - poll keyboard and handle input */
    while (1) {
        char c = keyboard_read_char();

        if (c == 0) continue;

        if (c == '\b') {
            /* backspace */
            if (input_len > 0) {
                /* erase cursor */
                draw_cursor(input_len, COLOR_DARK_GRAY);
                /* remove last character */
                input_len--;
                input_buf[input_len] = '\0';
                /* redraw: clear text area and rewrite the string */
                clear_input_text();
                draw_string(INPUT_TEXT_X, INPUT_TEXT_Y, input_buf, COLOR_WHITE);
                /* draw cursor at new position */
                draw_cursor(input_len, COLOR_WHITE);
            }
        } else if (c == '\n') {
            /* enter: for now, just show what was typed above the input bar */
            /* clear the display area */
            vga_fill_rect(0, 32, SCREEN_WIDTH, 148, COLOR_BLACK);
            draw_string(8, 40, "you typed:", COLOR_LIGHT_GRAY);
            draw_string(8, 56, input_buf, COLOR_LIGHT_GREEN);

            /* clear input */
            draw_cursor(input_len, COLOR_DARK_GRAY);
            input_len = 0;
            input_buf[0] = '\0';
            clear_input_text();
            draw_cursor(0, COLOR_WHITE);
        } else if (input_len < MAX_INPUT_LEN) {
            /* regular character - add to buffer and display */
            draw_cursor(input_len, COLOR_DARK_GRAY);  /* erase old cursor */
            input_buf[input_len] = c;
            input_len++;
            input_buf[input_len] = '\0';
            /* draw the new character */
            draw_char(INPUT_TEXT_X + (input_len - 1) * FONT_WIDTH, INPUT_TEXT_Y, c, COLOR_WHITE);
            /* draw cursor at new position */
            draw_cursor(input_len, COLOR_WHITE);
        }
    }
}
