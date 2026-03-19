/*
 * vga.h - VGA graphics mode driver
 *
 * VGA has two main modes:
 *   - text mode (what we've been using): 80x25 characters
 *   - graphics mode 0x13: 320x200 pixels, 256 colors
 *
 * in mode 0x13, the framebuffer is at address 0xA0000.
 * each byte = one pixel, and the byte value = color index (0-255).
 * the screen is 320 pixels wide and 200 pixels tall = 64000 bytes.
 *
 * to switch modes, we have to program a bunch of VGA registers.
 * the VGA controller has 5 groups of registers, each controlling
 * different parts of the display hardware:
 *   - Miscellaneous: clock speed, sync polarity
 *   - Sequencer: memory access, clocking
 *   - CRTC: screen timing (when to draw, when to blank)
 *   - Graphics Controller: how CPU accesses video memory
 *   - Attribute Controller: color palette mapping
 *
 * we just load pre-set values for mode 0x13 into all of them.
 * these values are standardized - every VGA card uses the same ones.
 */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include "ports.h"

#define VGA_FRAMEBUFFER 0xA0000
#define SCREEN_WIDTH    320
#define SCREEN_HEIGHT   200

/* standard VGA palette colors (first 16) */
#define COLOR_BLACK        0
#define COLOR_BLUE         1
#define COLOR_GREEN        2
#define COLOR_CYAN         3
#define COLOR_RED          4
#define COLOR_MAGENTA      5
#define COLOR_BROWN        6
#define COLOR_LIGHT_GRAY   7
#define COLOR_DARK_GRAY    8
#define COLOR_LIGHT_BLUE   9
#define COLOR_LIGHT_GREEN  10
#define COLOR_LIGHT_CYAN   11
#define COLOR_LIGHT_RED    12
#define COLOR_LIGHT_MAGENTA 13
#define COLOR_YELLOW       14
#define COLOR_WHITE        15

/* ============================================================
 * VGA register values for mode 0x13 (320x200x256)
 * ============================================================
 * these are basically a "recipe" - load these exact numbers
 * into the VGA registers and you get mode 0x13.
 * every VGA-compatible card uses the same values.
 */

/* miscellaneous output register - controls basic VGA operation
   0x63 = use 0x3D4/0x3D5 ports, enable RAM, 28MHz clock */
static const uint8_t mode_13h_misc = 0x63;

/* sequencer registers - control memory access and timing */
static const uint8_t mode_13h_seq[] = {
    0x03, 0x01, 0x0F, 0x00, 0x0E
};

/* CRT controller registers - control screen timing
   (when to start/stop drawing each scanline, refresh rate, etc) */
static const uint8_t mode_13h_crtc[] = {
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF
};

/* graphics controller registers - control how CPU reads/writes video RAM */
static const uint8_t mode_13h_gc[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF
};

/* attribute controller registers - map color indices to actual colors */
static const uint8_t mode_13h_ac[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

/*
 * write a list of values to sequential VGA registers.
 * many VGA register groups work as index/data pairs:
 *   - write the register number to the index port
 *   - write the value to the data port
 */
static void vga_write_regs(uint16_t index_port, uint16_t data_port,
                           const uint8_t *values, int count) {
    for (int i = 0; i < count; i++) {
        outb(index_port, i);
        outb(data_port, values[i]);
    }
}

/*
 * switch the VGA card to mode 0x13 (320x200, 256 colors)
 *
 * this programs all 5 register groups in order.
 * after this function, the screen shows pixels instead of text,
 * and we can draw by writing bytes to 0xA0000.
 */
static void vga_init_graphics(void) {
    /* 1. miscellaneous register - set basic VGA parameters */
    outb(0x3C2, mode_13h_misc);

    /* 2. sequencer registers - control memory and clocking
       first, reset the sequencer (register 0 = 0x03 enables it) */
    vga_write_regs(0x3C4, 0x3C5, mode_13h_seq, 5);

    /* 3. CRTC registers - control display timing
       must unlock CRTC first by clearing bit 7 of register 0x11 */
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & 0x7F);  /* clear protect bit */
    vga_write_regs(0x3D4, 0x3D5, mode_13h_crtc, 25);

    /* 4. graphics controller registers - control memory access mode */
    vga_write_regs(0x3CE, 0x3CF, mode_13h_gc, 9);

    /* 5. attribute controller registers - control palette
       the AC is weird: you write index AND data to the same port (0x3C0)
       but first read from 0x3DA to reset its flip-flop state */
    inb(0x3DA);  /* reset attribute controller flip-flop */
    for (int i = 0; i < 21; i++) {
        outb(0x3C0, i);              /* write index */
        outb(0x3C0, mode_13h_ac[i]); /* write value */
    }
    /* re-enable video output (bit 5 of AC index register) */
    outb(0x3C0, 0x20);
}

/*
 * draw a single pixel at (x, y) with the given color.
 * the framebuffer is a flat array: position = y * 320 + x
 * each byte is one pixel.
 */
static inline void vga_put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    volatile uint8_t *fb = (volatile uint8_t *)VGA_FRAMEBUFFER;
    fb[y * SCREEN_WIDTH + x] = color;
}

/* clear the entire screen to one color */
static void vga_clear(uint8_t color) {
    volatile uint8_t *fb = (volatile uint8_t *)VGA_FRAMEBUFFER;
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        fb[i] = color;
    }
}

/* draw a horizontal line */
static void vga_draw_hline(int x, int y, int width, uint8_t color) {
    for (int i = 0; i < width; i++) {
        vga_put_pixel(x + i, y, color);
    }
}

/* draw a vertical line */
static void vga_draw_vline(int x, int y, int height, uint8_t color) {
    for (int i = 0; i < height; i++) {
        vga_put_pixel(x, y + i, color);
    }
}

/* draw a filled rectangle */
static void vga_fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int row = 0; row < h; row++) {
        vga_draw_hline(x, y + row, w, color);
    }
}

#endif
