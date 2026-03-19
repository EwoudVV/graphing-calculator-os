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

/* the framebuffer pointer - set during init */
static volatile uint32_t *framebuffer;

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
    framebuffer[y * SCREEN_WIDTH + x] = color;
}

/* clear the entire screen to one color */
static void vga_clear(uint32_t color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        framebuffer[i] = color;
    }
}

/* draw a horizontal line */
static void vga_draw_hline(int x, int y, int width, uint32_t color) {
    for (int i = 0; i < width; i++) {
        vga_put_pixel(x + i, y, color);
    }
}

/* draw a vertical line */
static void vga_draw_vline(int x, int y, int height, uint32_t color) {
    for (int i = 0; i < height; i++) {
        vga_put_pixel(x, y + i, color);
    }
}

/* draw a filled rectangle */
static void vga_fill_rect(int x, int y, int w, int h, uint32_t color) {
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            vga_put_pixel(x + col, y + row, color);
        }
    }
}

#endif
