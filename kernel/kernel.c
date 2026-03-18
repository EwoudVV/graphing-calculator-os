/*
 * the kernel main function
 *
 * the first C code that runs in the os
 * running directly on the hardware, bare metal
 *
 * to print text, write directly to the VGA text buffer in video memory
 */

#include <stdint.h>
#include "keyboard.h"

/* the VGA text buffer is at a physical memory address
   the gpu constantly reads from this and displays it on screen
   each character takes 2 bytes: [ASCII char] [color attribute]
   the screen is 80 columns x 25 rows = 2000 characters = 4000 bytes */
#define VGA_BUFFER 0xB8000
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* color attribute byte: high nibble = background, low nibble = foreground
   0x0F = black background (0) + white foreground (F) */
#define COLOR_WHITE_ON_BLACK 0x0F
#define COLOR_GREEN_ON_BLACK 0x0A
#define COLOR_CYAN_ON_BLACK  0x0B

// writes a string to the VGA text buffer at a given row
void print_at(const char *str, int row, int col, uint8_t color) {
    /* cast the VGA buffer address to a pointer it can write to, use volatile because this memory is read by the video hardware. */
    volatile uint16_t *vga = (volatile uint16_t *)VGA_BUFFER;

    /* calculate the position in the buffer
       each row is 80 characters wide */
    int pos = row * VGA_WIDTH + col;

    // write each character
    for (int i = 0; str[i] != '\0'; i++) {
        /* put the ASCII char and color into one 16 bit value
           low byte = character, high byte = color attribute */
        vga[pos + i] = (uint16_t)str[i] | ((uint16_t)color << 8);
    }
}

// clear the entire screen by filling it with spaces
void clear_screen(void) {
    volatile uint16_t *vga = (volatile uint16_t *)VGA_BUFFER;
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        vga[i] = (uint16_t)' ' | ((uint16_t)COLOR_WHITE_ON_BLACK << 8);
    }
}

/* put a single character on screen at a position */
void put_char_at(char c, int row, int col, uint8_t color) {
    volatile uint16_t *vga = (volatile uint16_t *)VGA_BUFFER;
    int pos = row * VGA_WIDTH + col;
    vga[pos] = (uint16_t)c | ((uint16_t)color << 8);
}

// kernel entry point, called from boot.asm
void kmain(void) {
    clear_screen();

    print_at("Hello World!", 1, 2, COLOR_GREEN_ON_BLACK);
    print_at("This is a graphing calculator operating system", 3, 2, COLOR_WHITE_ON_BLACK);
    print_at("Built from scratch! Coded by EVV (me!!)", 5, 2, COLOR_CYAN_ON_BLACK);

    /* input line: where the user types */
    print_at("> ", 8, 2, COLOR_WHITE_ON_BLACK);
    int cursor_col = 4;  /* start after "> " */

    /*
     * the main loop! this is the "kernel loop" that runs forever.
     * every OS has one of these - it just keeps checking for input
     * and responding to it. without this loop, the OS would
     * print its messages and then... stop. dead.
     *
     * right now we "poll" the keyboard (keep asking "any keys?")
     * later we'll use interrupts so the CPU doesn't waste cycles
     */
    while (1) {
        char c = keyboard_read_char();

        if (c == 0) {
            /* no key pressed, keep looping */
            continue;
        }

        if (c == '\b') {
            /* backspace: move cursor back and erase the character */
            if (cursor_col > 4) {
                cursor_col--;
                put_char_at(' ', 8, cursor_col, COLOR_WHITE_ON_BLACK);
            }
        } else if (c == '\n') {
            /* enter key: for now, just clear the input line */
            for (int i = 4; i < VGA_WIDTH; i++) {
                put_char_at(' ', 8, i, COLOR_WHITE_ON_BLACK);
            }
            cursor_col = 4;
        } else if (cursor_col < VGA_WIDTH - 1) {
            /* regular character: put it on screen and advance cursor */
            put_char_at(c, 8, cursor_col, COLOR_GREEN_ON_BLACK);
            cursor_col++;
        }
    }
}