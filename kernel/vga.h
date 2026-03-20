/*
 * vga.h - VGA graphics driver (now with high resolution!)
 *
 * UPGRADE: we moved from VGA mode 0x13 (320x200) to 640x480 using
 * the "Bochs VBE" (VESA BIOS Extensions) interface.
 *
 * SCALING: the VBE hardware runs at 1280x960 (big window), but all
 * our drawing code works at 640x480 (logical resolution). the flush
 * function scales each pixel to a 2x2 block. this gives us a nice
 * big window without changing any drawing code or layout math.
 *
 * how it works:
 *   old way: VGA mode 0x13 = framebuffer at fixed address 0xA0000, 320x200
 *   new way: Bochs VBE = we TELL the display what resolution we want,
 *            and it gives us a big framebuffer somewhere in memory.
 *
 * QEMU emulates a "Bochs Display Adapter" which supports this.
 * we talk to it through I/O ports 0x01CE (index) and 0x01CF (data).
 *
 * we use 32 bits per pixel (true color):
 *   each pixel = 4 bytes = 0x00RRGGBB
 *   no palette needed - we specify exact RGB colors!
 */

#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include "ports.h"

/*
 * LOGICAL resolution - what all drawing code uses.
 * the back buffer, all coordinates, all layout math uses these.
 */
#define SCREEN_WIDTH    640
#define SCREEN_HEIGHT   480

/*
 * PHYSICAL resolution - the actual VBE framebuffer size.
 * each logical pixel becomes a SCALE x SCALE block on screen.
 * this makes the window physically larger without changing the
 * pixel density or any layout code.
 */
#define SCALE           2
#define REAL_WIDTH      (SCREEN_WIDTH * SCALE)
#define REAL_HEIGHT     (SCREEN_HEIGHT * SCALE)

/* === Bochs VBE register interface === */
#define VBE_INDEX_PORT   0x01CE
#define VBE_DATA_PORT    0x01CF

#define VBE_REG_XRES     0x01
#define VBE_REG_YRES     0x02
#define VBE_REG_BPP      0x03
#define VBE_REG_ENABLE   0x04

#define VBE_ENABLED      0x01
#define VBE_LFB_ENABLED  0x40

static inline void vbe_write(uint16_t reg, uint16_t value) {
    outw(VBE_INDEX_PORT, reg);
    outw(VBE_DATA_PORT, value);
}

/* === PCI configuration space === */
#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static uint32_t pci_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) |
                    ((uint32_t)device << 11) | ((uint32_t)func << 8) |
                    (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

/* === 32-bit color definitions (0x00RRGGBB) === */
#define COLOR_BLACK         0x00000000
#define COLOR_WHITE         0x00FFFFFF
#define COLOR_DARK_GRAY     0x00333333
#define COLOR_GRAY          0x00808080
#define COLOR_LIGHT_GRAY    0x00B0B0B0

#define COLOR_LIGHT_GREEN   0x0055FF55
#define COLOR_LIGHT_RED     0x00FF5555
#define COLOR_LIGHT_BLUE    0x005599FF
#define COLOR_YELLOW        0x00FFFF55
#define COLOR_LIGHT_CYAN    0x0055FFFF
#define COLOR_LIGHT_MAGENTA 0x00FF55FF

#define COLOR_TITLE_BG      0x00182848
#define COLOR_PANEL_BG      0x00202020
#define COLOR_INPUT_BG      0x00303030
#define COLOR_GRID_DOT      0x00303050
#define COLOR_AXIS          0x00707070
#define COLOR_TICK           0x00A0A0A0

/* the framebuffer pointer (actual screen at REAL resolution) */
static volatile uint32_t *framebuffer;

/*
 * DOUBLE BUFFERING at LOGICAL resolution.
 *
 * all drawing happens at 640x480 in the back buffer.
 * when we flush, each pixel gets scaled to 2x2 on the real framebuffer.
 *
 * draw_target points to either:
 *   back_buffer -> for off-screen rendering (pan/zoom)
 *   screen_buffer -> for direct "visible" drawing (animation)
 *
 * screen_buffer is ALSO at logical resolution. when we write to it,
 * we immediately scale to the real framebuffer so it's visible.
 */
static uint32_t back_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint32_t screen_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];
static volatile uint32_t *draw_target;
static int drawing_to_screen = 0;  /* 1 = draw_target is screen_buffer */

/* fast_memcpy32 - bulk copy using rep movsl (declared early, used by flush) */
static inline void fast_memcpy32(uint32_t *dest, const uint32_t *src, int count) {
    if (count <= 0) return;
    __asm__ volatile (
        "rep movsl"
        : "+D"(dest), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

/* fast_memset32 - bulk fill using rep stosl */
static inline void fast_memset32(uint32_t *dest, uint32_t val, int count) {
    if (count <= 0) return;
    __asm__ volatile (
        "rep stosl"
        : "+D"(dest), "+c"(count)
        : "a"(val)
        : "memory"
    );
}

static void vga_use_backbuffer(void) {
    draw_target = (volatile uint32_t *)back_buffer;
    drawing_to_screen = 0;
}

static void vga_use_screen(void) {
    draw_target = (volatile uint32_t *)screen_buffer;
    drawing_to_screen = 1;
}

/*
 * write a 2x2 scaled pixel block to the real framebuffer.
 * called whenever we need something to appear on screen immediately.
 */
static inline void fb_write_scaled(int lx, int ly, uint32_t color) {
    if (lx < 0 || lx >= SCREEN_WIDTH || ly < 0 || ly >= SCREEN_HEIGHT) return;
    int rx = lx * SCALE;
    int ry = ly * SCALE;
    framebuffer[ry * REAL_WIDTH + rx]           = color;
    framebuffer[ry * REAL_WIDTH + rx + 1]       = color;
    framebuffer[(ry + 1) * REAL_WIDTH + rx]     = color;
    framebuffer[(ry + 1) * REAL_WIDTH + rx + 1] = color;
}

/*
 * flush the back buffer to the screen with 2x scaling.
 *
 * each logical pixel becomes a 2x2 block of physical pixels.
 * we process one logical row at a time, writing two physical rows.
 */
static void vga_flush(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        const uint32_t *src = &back_buffer[y * SCREEN_WIDTH];
        int ry = y * SCALE;
        volatile uint32_t *dst1 = &framebuffer[ry * REAL_WIDTH];
        volatile uint32_t *dst2 = &framebuffer[(ry + 1) * REAL_WIDTH];
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint32_t c = src[x];
            int rx = x * SCALE;
            dst1[rx]     = c;
            dst1[rx + 1] = c;
            dst2[rx]     = c;
            dst2[rx + 1] = c;
        }
    }
    /* also sync screen_buffer so direct draws stay consistent */
    fast_memcpy32(screen_buffer, back_buffer, SCREEN_WIDTH * SCREEN_HEIGHT);
}

/*
 * flush the screen_buffer to the real framebuffer.
 * used after direct drawing (animation) to sync visible state.
 */
static void vga_flush_screen(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        const uint32_t *src = &screen_buffer[y * SCREEN_WIDTH];
        int ry = y * SCALE;
        volatile uint32_t *dst1 = &framebuffer[ry * REAL_WIDTH];
        volatile uint32_t *dst2 = &framebuffer[(ry + 1) * REAL_WIDTH];
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint32_t c = src[x];
            int rx = x * SCALE;
            dst1[rx]     = c;
            dst1[rx + 1] = c;
            dst2[rx]     = c;
            dst2[rx + 1] = c;
        }
    }
}

/* initialize 1280x960x32 graphics mode (displayed as scaled 640x480) */
static void vga_init_graphics(void) {
    uint32_t bar0 = pci_read(0, 2, 0, 0x10);
    framebuffer = (volatile uint32_t *)(bar0 & 0xFFFFFFF0);

    draw_target = (volatile uint32_t *)screen_buffer;
    drawing_to_screen = 1;

    vbe_write(VBE_REG_ENABLE, 0x00);
    vbe_write(VBE_REG_XRES, REAL_WIDTH);      /* 1280 physical pixels */
    vbe_write(VBE_REG_YRES, REAL_HEIGHT);      /* 960 physical pixels */
    vbe_write(VBE_REG_BPP, 32);
    vbe_write(VBE_REG_ENABLE, VBE_ENABLED | VBE_LFB_ENABLED);
}

/*
 * draw a single pixel.
 * writes to the logical buffer (640x480).
 * if drawing to screen, also immediately scales to the real framebuffer.
 */
static inline void vga_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    draw_target[y * SCREEN_WIDTH + x] = color;
    if (drawing_to_screen) {
        fb_write_scaled(x, y, color);
    }
}

/* clear the entire logical screen */
static void vga_clear(uint32_t color) {
    int count = SCREEN_WIDTH * SCREEN_HEIGHT;
    volatile uint32_t *dest = draw_target;
    __asm__ volatile (
        "rep stosl"
        : "+D"(dest), "+c"(count)
        : "a"(color)
        : "memory"
    );
    if (drawing_to_screen) {
        /* also clear the real framebuffer */
        int real_count = REAL_WIDTH * REAL_HEIGHT;
        volatile uint32_t *fb = framebuffer;
        __asm__ volatile (
            "rep stosl"
            : "+D"(fb), "+c"(real_count)
            : "a"(color)
            : "memory"
        );
    }
}

/* draw a horizontal line */
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
    if (drawing_to_screen) {
        for (int px = x1; px < x2; px++) fb_write_scaled(px, y, color);
    }
}

/* draw a vertical line */
static void vga_draw_vline(int x, int y, int height, uint32_t color) {
    if (x < 0 || x >= SCREEN_WIDTH) return;
    int y1 = y < 0 ? 0 : y;
    int y2 = (y + height) > SCREEN_HEIGHT ? SCREEN_HEIGHT : (y + height);
    for (int row = y1; row < y2; row++) {
        draw_target[row * SCREEN_WIDTH + x] = color;
        if (drawing_to_screen) fb_write_scaled(x, row, color);
    }
}

/* draw a filled rectangle */
static void vga_fill_rect(int x, int y, int w, int h, uint32_t color) {
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
    if (drawing_to_screen) {
        /* scale the filled rect to real framebuffer */
        for (int row = y1; row < y2; row++) {
            int ry = row * SCALE;
            for (int col = x1; col < x2; col++) {
                int rx = col * SCALE;
                framebuffer[ry * REAL_WIDTH + rx]           = color;
                framebuffer[ry * REAL_WIDTH + rx + 1]       = color;
                framebuffer[(ry + 1) * REAL_WIDTH + rx]     = color;
                framebuffer[(ry + 1) * REAL_WIDTH + rx + 1] = color;
            }
        }
    }
}

#endif
