/*
 * vga.h - VGA graphics driver (now with high resolution!)
 *
 * UPGRADE: we moved from VGA mode 0x13 (320x200) to 640x480 using
 * the "Bochs VBE" (VESA BIOS Extensions) interface.
 *
 * how it works:
 *   old way: VGA mode 0x13 = framebuffer at fixed address 0xA0000, 320x200
 *   new way: Bochs VBE = we TELL the display what resolution we want,
 *            and it gives us a big framebuffer somewhere in memory.
 *
 * QEMU emulates a "Bochs Display Adapter" which supports this.
 * we talk to it through I/O ports 0x01CE (index) and 0x01CF (data).
 *
 * but where's the framebuffer? it's a PCI device, so its memory address
 * is stored in a "PCI BAR" (Base Address Register). we read it from
 * the PCI configuration space using ports 0x0CF8/0x0CFC.
 *
 * we use 32 bits per pixel (true color):
 *   each pixel = 4 bytes = 0x00RRGGBB
 *   no palette needed - we specify exact RGB colors!
 */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include "ports.h"

#define SCREEN_WIDTH    640
#define SCREEN_HEIGHT   480

/* === Bochs VBE register interface ===
 * the display adapter has numbered registers.
 * to access one: write the register number to port 0x01CE,
 * then read/write the value at port 0x01CF.
 */
#define VBE_INDEX_PORT   0x01CE
#define VBE_DATA_PORT    0x01CF

#define VBE_REG_XRES     0x01   /* horizontal resolution */
#define VBE_REG_YRES     0x02   /* vertical resolution */
#define VBE_REG_BPP      0x03   /* bits per pixel */
#define VBE_REG_ENABLE   0x04   /* enable/disable the display */

#define VBE_ENABLED      0x01   /* turn on VBE mode */
#define VBE_LFB_ENABLED  0x40   /* use Linear FrameBuffer (not banked) */

/* write a value to a Bochs VBE register */
static inline void vbe_write(uint16_t reg, uint16_t value) {
    outw(VBE_INDEX_PORT, reg);
    outw(VBE_DATA_PORT, value);
}

/* === PCI configuration space ===
 * PCI is the bus that connects hardware (VGA, network, etc) to the CPU.
 * each device has a "configuration space" with info about it.
 * BAR 0 (Base Address Register 0) tells us where the framebuffer is in memory.
 *
 * to read PCI config: write the address to port 0x0CF8, read data from 0x0CFC.
 * the address encodes: bus number, device number, function, and register offset.
 *
 * in QEMU's default machine (i440fx), the VGA card is at bus 0, device 2, function 0.
 */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static uint32_t pci_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    /*
     * PCI config address format (32 bits):
     *   bit 31    = 1 (enable config access)
     *   bits 16-23 = bus number
     *   bits 11-15 = device number
     *   bits 8-10  = function number
     *   bits 0-7   = register offset (must be 4-byte aligned)
     */
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)device << 11) | ((uint32_t)func << 8) |
                    (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/* === 32-bit color definitions (0x00RRGGBB) ===
 * with 32bpp we can use ANY color! no more 16-color palette.
 */
#define COLOR_BLACK         0x00000000
#define COLOR_WHITE         0x00FFFFFF
#define COLOR_DARK_GRAY     0x00333333
#define COLOR_GRAY          0x00808080
#define COLOR_LIGHT_GRAY    0x00B0B0B0

/* graph colors for multiple equations */
#define COLOR_LIGHT_GREEN   0x0055FF55
#define COLOR_LIGHT_RED     0x00FF5555
#define COLOR_LIGHT_BLUE    0x005599FF
#define COLOR_YELLOW        0x00FFFF55
#define COLOR_LIGHT_CYAN    0x0055FFFF
#define COLOR_LIGHT_MAGENTA 0x00FF55FF

/* UI colors */
#define COLOR_TITLE_BG      0x00182848
#define COLOR_PANEL_BG      0x00202020
#define COLOR_INPUT_BG      0x00303030
#define COLOR_GRID_DOT      0x00303050
#define COLOR_AXIS          0x00707070
#define COLOR_TICK           0x00A0A0A0

/* the framebuffer pointer (actual screen) - set during init */
static volatile uint32_t *framebuffer;

/*
 * DOUBLE BUFFERING
 *
 * the problem: when we draw directly to the framebuffer (screen),
 * the user sees every pixel appear one by one. for panning/zooming
 * this looks terrible - you see the old image get erased and the
 * new one drawn progressively.
 *
 * the solution: draw to a hidden "back buffer" in regular RAM,
 * then copy the entire buffer to the screen in one fast operation.
 * the user sees an instant update instead of progressive drawing.
 *
 * this is the same technique used by every game and OS:
 *   1. draw the next frame to the back buffer (invisible)
 *   2. "flip" - copy back buffer to screen (instant)
 *
 * we use a pointer called draw_target that all drawing functions
 * write to. by switching this pointer, we control WHERE pixels go:
 *   draw_target = framebuffer   -> draws directly to screen (visible)
 *   draw_target = back_buffer   -> draws to hidden buffer (invisible)
 */
static uint32_t back_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static volatile uint32_t *draw_target;

/* switch drawing to the hidden back buffer */
static void vga_use_backbuffer(void) {
    draw_target = (volatile uint32_t *)back_buffer;
}

/* switch drawing directly to the screen */
static void vga_use_screen(void) {
    draw_target = framebuffer;
}

/*
 * copy the entire back buffer to the screen in one go ("flip").
 *
 * uses "rep movsl" - an x86 instruction that copies 32-bit words
 * in a tight hardware-optimized loop. the CPU knows this pattern
 * and can burst data across the memory bus much faster than a
 * C for-loop that does individual writes.
 *
 *   rep    = "repeat ECX times"
 *   movsl  = "move string long" (copy 4 bytes from ESI to EDI, advance both)
 */
static void vga_flush(void) {
    int count = SCREEN_WIDTH * SCREEN_HEIGHT;
    const uint32_t *src = (const uint32_t *)back_buffer;
    volatile uint32_t *dest = framebuffer;
    __asm__ volatile (
        "rep movsl"
        : "+D"(dest), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

/*
 * initialize 640x480x32 graphics mode using Bochs VBE.
 *
 * step 1: find framebuffer address from PCI
 * step 2: program the display resolution
 */
static void vga_init_graphics(void) {
    /*
     * step 1: find the framebuffer address.
     *
     * the VGA card is a PCI device. its framebuffer is mapped into
     * the CPU's memory space at an address stored in BAR 0.
     * in QEMU, the VGA is at bus 0, device 2, function 0.
     * BAR 0 is at PCI config offset 0x10.
     */
    uint32_t bar0 = pci_read(0, 2, 0, 0x10);
    /* mask off the lower 4 bits (they're flags, not part of the address) */
    framebuffer = (volatile uint32_t *)(bar0 & 0xFFFFFFF0);

    /* default: draw directly to screen */
    draw_target = framebuffer;

    /*
     * step 2: set the display mode.
     * disable VBE first, change settings, then re-enable.
     * this is like turning off a monitor before changing its resolution.
     */
    vbe_write(VBE_REG_ENABLE, 0x00);             /* disable */
    vbe_write(VBE_REG_XRES, SCREEN_WIDTH);        /* 640 pixels wide */
    vbe_write(VBE_REG_YRES, SCREEN_HEIGHT);       /* 480 pixels tall */
    vbe_write(VBE_REG_BPP, 32);                   /* 32 bits per pixel */
    vbe_write(VBE_REG_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED); /* enable! */
}

/*
 * draw a single pixel at (x, y) with a 32-bit color.
 * the framebuffer is a flat array: position = y * width + x
 * each element is a uint32_t (4 bytes) = one pixel.
 */
static inline void vga_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    draw_target[y * SCREEN_WIDTH + x] = color;
}

/*
 * clear the entire screen to one color.
 *
 * uses "rep stosl" - fills memory with a 32-bit value in a
 * hardware-optimized loop. way faster than writing one pixel at a time.
 *
 *   rep    = "repeat ECX times"
 *   stosl  = "store string long" (write EAX to EDI, advance EDI)
 */
static void vga_clear(uint32_t color) {
    int count = SCREEN_WIDTH * SCREEN_HEIGHT;
    volatile uint32_t *dest = draw_target;
    __asm__ volatile (
        "rep stosl"
        : "+D"(dest), "+c"(count)
        : "a"(color)
        : "memory"
    );
}

/* draw a horizontal line (uses rep stosl for speed) */
static void vga_draw_hline(int x, int y, int width, uint32_t color) {
    if (y < 0 || y >= SCREEN_HEIGHT) return;
    int x1 = x < 0 ? 0 : x;
    int x2 = (x + width) > SCREEN_WIDTH ? SCREEN_WIDTH : (x + width);
    int count = x2 - x1;
    if (count <= 0) return;
    volatile uint32_t *dest = &draw_target[y * SCREEN_WIDTH + x1];
    __asm__ volatile (
        "rep stosl"
        : "+D"(dest), "+c"(count)
        : "a"(color)
        : "memory"
    );
}

/* draw a vertical line */
static void vga_draw_vline(int x, int y, int height, uint32_t color) {
    if (x < 0 || x >= SCREEN_WIDTH) return;
    int y1 = y < 0 ? 0 : y;
    int y2 = (y + height) > SCREEN_HEIGHT ? SCREEN_HEIGHT : (y + height);
    for (int row = y1; row < y2; row++) {
        draw_target[row * SCREEN_WIDTH + x] = color;
    }
}

/*
 * draw a filled rectangle.
 *
 * OLD: called vga_put_pixel per pixel = 4 bounds checks per pixel.
 *      for a 500x432 rect that's 864,000 comparisons!
 *
 * NEW: clamp bounds ONCE, then fill each row with rep stosl.
 *      zero per-pixel overhead.
 */
static void vga_fill_rect(int x, int y, int w, int h, uint32_t color) {
    /* clamp to screen bounds once */
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = (x + w) > SCREEN_WIDTH ? SCREEN_WIDTH : (x + w);
    int y2 = (y + h) > SCREEN_HEIGHT ? SCREEN_HEIGHT : (y + h);
    int row_width = x2 - x1;
    if (row_width <= 0) return;

    for (int row = y1; row < y2; row++) {
        volatile uint32_t *dest = &draw_target[row * SCREEN_WIDTH + x1];
        int count = row_width;
        __asm__ volatile (
            "rep stosl"
            : "+D"(dest), "+c"(count)
            : "a"(color)
            : "memory"
        );
    }
}

#endif
