/*
 * mouse.h - PS/2 mouse driver
 *
 * how does a mouse talk to the CPU?
 * ===================================
 * the PS/2 controller is a chip that sits between the keyboard, the mouse,
 * and the CPU. it uses two I/O ports:
 *   - port 0x60 = DATA port  (read/write actual bytes to/from devices)
 *   - port 0x64 = COMMAND/STATUS port
 *       - reading 0x64 gives us a status byte (is there data? is it busy?)
 *       - writing 0x64 sends a command to the PS/2 controller itself
 *
 * the mouse is called the "auxiliary device" (the keyboard is the primary).
 * to send a command to the mouse, we first tell the PS/2 controller
 * "hey, the next byte I write to port 0x60 is for the mouse, not the keyboard"
 * by writing 0xD4 to port 0x64.
 *
 * the mouse sends data in 3-byte packets:
 *   byte 1: flags  (buttons + sign bits + overflow)
 *   byte 2: X movement (delta, how far it moved)
 *   byte 3: Y movement (delta)
 *
 * the flags byte layout:
 *   bit 0 = left button pressed
 *   bit 1 = right button pressed
 *   bit 2 = middle button pressed
 *   bit 3 = always 1 (used to sync packets -- if this is 0, we're out of sync!)
 *   bit 4 = X sign bit (1 = negative / moving left)
 *   bit 5 = Y sign bit (1 = negative / moving down... wait, actually UP in PS/2!)
 *   bit 6 = X overflow
 *   bit 7 = Y overflow
 *
 * PS/2 mouse Y-axis is INVERTED compared to screen coordinates:
 *   - mouse sends positive dy = moving UP
 *   - but on screen, y increases going DOWN
 *   so we SUBTRACT dy from our screen y position.
 */

#ifndef MOUSE_H
#define MOUSE_H

#include "ports.h"

/*
 * screen size constants.
 * the VBE runs at 1280x960 (physical) but we think in 640x480 (logical).
 * mouse deltas come in physical pixels, but we track and report
 * position in LOGICAL pixels so everything else stays simple.
 * we accumulate sub-pixel movement and only step when we cross
 * a logical pixel boundary.
 */
#define MOUSE_SCREEN_WIDTH  640
#define MOUSE_SCREEN_HEIGHT 480
#define MOUSE_SCALE         2

/* --- PS/2 controller ports --- */
#define MOUSE_DATA_PORT    0x60   /* read/write data to devices */
#define MOUSE_STATUS_PORT  0x64   /* read status / write commands */

/* --- PS/2 controller commands (written to port 0x64) --- */
#define MOUSE_CMD_ENABLE_AUX   0xA8  /* turn on the mouse port */
#define MOUSE_CMD_WRITE_MOUSE  0xD4  /* "next byte on 0x60 goes to mouse" */
#define MOUSE_CMD_READ_CONFIG  0x20  /* read the controller config byte */
#define MOUSE_CMD_WRITE_CONFIG 0x60  /* write the controller config byte */

/* --- mouse device commands (sent through the controller to the mouse) --- */
#define MOUSE_DEV_SET_DEFAULTS 0xF6  /* reset mouse to default settings */
#define MOUSE_DEV_ENABLE_DATA  0xF4  /* start sending movement packets */

/* --- status register bits (read from port 0x64) --- */
#define MOUSE_STATUS_OUTPUT_FULL  0x01  /* bit 0: data is ready to read */
#define MOUSE_STATUS_INPUT_FULL   0x02  /* bit 1: controller is busy, don't write yet */

/*
 * mouse_state - everything we know about the mouse right now
 *
 * x, y     = where the cursor is on screen (absolute pixel position)
 * dx, dy   = how much it moved last time (relative, in pixels)
 * left/right/middle = are the buttons pressed? (1 = yes, 0 = no)
 */
typedef struct {
    int x, y;                      /* absolute cursor position (pixels) */
    int dx, dy;                    /* last relative movement */
    int left, right, middle;       /* button states (1 = pressed) */
} mouse_state;

/* global mouse state -- any code that includes this header can read it */
static mouse_state mouse;

/*
 * physical-space accumulators for smooth mouse tracking.
 * QEMU sends deltas in physical pixels (1280x960 space).
 * we accumulate here and divide by MOUSE_SCALE to get logical position.
 * this prevents the mouse from feeling sluggish (losing small movements
 * to integer division truncation).
 */
static int mouse_phys_x = MOUSE_SCREEN_WIDTH * MOUSE_SCALE / 2;
static int mouse_phys_y = MOUSE_SCREEN_HEIGHT * MOUSE_SCALE / 2;

/* ====================================================================
 *  INTERNAL HELPER FUNCTIONS
 *  (these handle the annoying low-level timing/waiting stuff)
 * ==================================================================== */

/*
 * mouse_wait_write - wait until we're allowed to WRITE to the PS/2 controller
 *
 * before we send any command or data, we have to make sure the controller's
 * input buffer is empty. if we write while it's still processing the last
 * byte, our data gets lost!
 *
 * we check bit 1 of the status register:
 *   bit 1 = 1 means "I'm still busy, hold on"
 *   bit 1 = 0 means "ok, you can write now"
 *
 * the timeout prevents an infinite loop if the hardware is broken/missing.
 */
static void mouse_wait_write(void) {
    int timeout = 100000;  /* give up after this many tries */
    while (timeout--) {
        if ((inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_INPUT_FULL) == 0) {
            return;  /* input buffer is empty, safe to write! */
        }
    }
    /* if we get here, the controller never became ready -- hardware issue */
}

/*
 * mouse_wait_read - wait until there's data available to READ
 *
 * after we ask the mouse for something, the response doesn't appear
 * instantly. we need to wait for bit 0 of the status register:
 *   bit 0 = 1 means "there's a byte waiting for you on port 0x60"
 *   bit 0 = 0 means "nothing yet, keep waiting"
 */
static void mouse_wait_read(void) {
    int timeout = 100000;
    while (timeout--) {
        if (inb(MOUSE_STATUS_PORT) & MOUSE_STATUS_OUTPUT_FULL) {
            return;  /* data is ready to read! */
        }
    }
}

/*
 * mouse_send_cmd - send a command byte to the MOUSE (not the controller)
 *
 * this is a two-step process:
 * 1. tell the PS/2 controller "the next byte is for the mouse" (0xD4)
 * 2. write the actual command byte to the data port (0x60)
 *
 * the mouse will respond with 0xFA ("ACK" = acknowledged) which we
 * read and discard. if the mouse doesn't ACK, something went wrong.
 */
static void mouse_send_cmd(uint8_t cmd) {
    /* step 1: tell the controller to forward our next byte to the mouse */
    mouse_wait_write();
    outb(MOUSE_STATUS_PORT, MOUSE_CMD_WRITE_MOUSE);

    /* step 2: send the actual command */
    mouse_wait_write();
    outb(MOUSE_DATA_PORT, cmd);

    /* step 3: read (and discard) the ACK byte (0xFA) from the mouse */
    mouse_wait_read();
    inb(MOUSE_DATA_PORT);  /* should be 0xFA, we just throw it away */
}

/*
 * PACKET STATE MACHINE
 *
 * the mouse sends 3 bytes per packet, but they don't all arrive
 * at the same instant. we can't just block and wait because that
 * would freeze the whole OS (no keyboard, no drawing, nothing).
 *
 * instead, we collect bytes one at a time using a state machine:
 *   state 0: waiting for byte 1 (flags)
 *   state 1: got flags, waiting for byte 2 (dx)
 *   state 2: got dx, waiting for byte 3 (dy) -> process packet!
 *
 * each call to mouse_read() checks if there's a mouse byte ready.
 * if yes, it stores it and advances the state. when all 3 bytes
 * are collected, it processes the packet and returns 1.
 *
 * this is called a "non-blocking" design - the function returns
 * immediately instead of waiting, so the kernel loop stays responsive.
 */
static int mouse_packet_state = 0;   /* which byte are we waiting for? */
static uint8_t mouse_bytes[3];       /* collected packet bytes */

/* ====================================================================
 *  PUBLIC API
 * ==================================================================== */

/*
 * mouse_init - set up the PS/2 mouse so it starts sending us data
 *
 * this is the boot-up sequence. we need to:
 * 1. tell the PS/2 controller to enable the mouse port
 * 2. modify the controller's config to allow mouse interrupts
 * 3. tell the mouse itself to reset to defaults
 * 4. tell the mouse to start sending movement packets
 *
 * after this, the mouse will send 3-byte packets whenever it moves
 * or a button is clicked. we can read them with mouse_read().
 */
static void mouse_init(void) {
    /*
     * STEP 1: enable the auxiliary (mouse) port
     *
     * the PS/2 controller has two channels: one for keyboard, one for mouse.
     * the mouse channel might be disabled by default (BIOS does this sometimes).
     * command 0xA8 tells the controller to activate the mouse channel.
     */
    mouse_wait_write();
    outb(MOUSE_STATUS_PORT, MOUSE_CMD_ENABLE_AUX);

    /*
     * STEP 2: enable mouse interrupts in the controller config
     *
     * the PS/2 controller has a "config byte" that controls various settings.
     * we need to set bit 1 (enable auxiliary/mouse interrupt) so that the
     * mouse can actually send data through to us.
     *
     * process: read the config -> set bit 1 -> write it back
     */

    /* 2a: ask the controller to give us its config byte */
    mouse_wait_write();
    outb(MOUSE_STATUS_PORT, MOUSE_CMD_READ_CONFIG);

    /* 2b: read the config byte from the data port */
    mouse_wait_read();
    uint8_t config = inb(MOUSE_DATA_PORT);

    /*
     * 2c: set bit 1 (enable IRQ12 for mouse) and make sure
     *     bit 5 is CLEAR (bit 5 = 1 would DISABLE the mouse clock,
     *     which would prevent the mouse from sending anything!)
     */
    config |= 0x02;   /* set bit 1:   enable mouse interrupt (IRQ12) */
    config &= ~0x20;  /* clear bit 5: enable mouse clock (don't disable it!) */

    /* 2d: write the modified config back */
    mouse_wait_write();
    outb(MOUSE_STATUS_PORT, MOUSE_CMD_WRITE_CONFIG);
    mouse_wait_write();
    outb(MOUSE_DATA_PORT, config);

    /*
     * STEP 3: tell the mouse to reset to default settings
     *
     * command 0xF6 = "set defaults". this resets the mouse's sample rate,
     * resolution, and scaling to standard values. good idea after boot
     * because who knows what state the BIOS left it in.
     */
    mouse_send_cmd(MOUSE_DEV_SET_DEFAULTS);

    /*
     * STEP 4: enable data reporting
     *
     * command 0xF4 = "enable data reporting". this is the ON switch!
     * until we send this, the mouse is silent even if it's moving.
     * after this command, the mouse will start sending 3-byte packets.
     */
    mouse_send_cmd(MOUSE_DEV_ENABLE_DATA);

    /*
     * STEP 5: initialize our mouse state struct
     *
     * start the cursor in the center of the screen. all buttons start
     * as "not pressed" (0). no movement yet (dx=dy=0).
     */
    mouse_phys_x = MOUSE_SCREEN_WIDTH * MOUSE_SCALE / 2;
    mouse_phys_y = MOUSE_SCREEN_HEIGHT * MOUSE_SCALE / 2;
    mouse.x = MOUSE_SCREEN_WIDTH / 2;
    mouse.y = MOUSE_SCREEN_HEIGHT / 2;
    mouse.dx = 0;
    mouse.dy = 0;
    mouse.left = 0;
    mouse.right = 0;
    mouse.middle = 0;
}

/*
 * mouse_read - collect mouse bytes one at a time (non-blocking)
 *
 * returns: 1 if a complete 3-byte packet was received (mouse state updated)
 *          0 if still collecting bytes or no data available
 *
 * WHY A STATE MACHINE?
 * the old approach tried to read all 3 bytes at once with blocking waits.
 * problem: while waiting for byte 2 or 3, the keyboard can't be read,
 * and if a keyboard byte arrives on port 0x60 first, we'd read it as
 * mouse data = corruption!
 *
 * the state machine approach: each call reads AT MOST one byte.
 * if it's a mouse byte, we store it and advance the state.
 * once we have all 3 bytes, we process the packet.
 * between calls, the kernel loop can handle keyboard input normally.
 */
static int mouse_read(void) {
    /* check if there's data AND it's from the mouse (bit 5) */
    uint8_t status = inb(MOUSE_STATUS_PORT);
    if ((status & 0x21) != 0x21) {
        return 0;  /* no mouse data right now */
    }

    /* read one byte from the mouse */
    uint8_t byte = inb(MOUSE_DATA_PORT);

    switch (mouse_packet_state) {
        case 0:
            /*
             * BYTE 1: flags
             * sync check: bit 3 should ALWAYS be 1 in the flags byte.
             * if it's 0, we're out of sync (reading mid-packet).
             * discard and keep waiting for a valid first byte.
             */
            if (!(byte & 0x08)) {
                return 0;  /* not a valid flags byte, skip it */
            }
            /* also discard if overflow bits are set (garbage data) */
            if (byte & 0xC0) {
                return 0;
            }
            mouse_bytes[0] = byte;
            mouse_packet_state = 1;
            return 0;  /* need more bytes */

        case 1:
            /* BYTE 2: X delta */
            mouse_bytes[1] = byte;
            mouse_packet_state = 2;
            return 0;  /* need one more byte */

        case 2:
            /* BYTE 3: Y delta - packet complete! */
            mouse_bytes[2] = byte;
            mouse_packet_state = 0;  /* reset for next packet */
            break;

        default:
            mouse_packet_state = 0;
            return 0;
    }

    /* === PROCESS THE COMPLETE PACKET === */

    uint8_t flags  = mouse_bytes[0];
    uint8_t raw_dx = mouse_bytes[1];
    uint8_t raw_dy = mouse_bytes[2];

    /* decode button states from flags byte */
    mouse.left   = !!(flags & 0x01);
    mouse.right  = !!(flags & 0x02);
    mouse.middle = !!(flags & 0x04);

    /*
     * decode movement with sign extension
     *
     * the raw dx/dy are unsigned bytes (0-255), but movement can be
     * negative. the sign is in the flags byte:
     *   bit 4 = X sign (1 = negative / moving left)
     *   bit 5 = Y sign (1 = negative in PS/2 terms)
     *
     * sign extension: if sign bit set, OR with 0xFFFFFF00 to make
     * it a proper negative int in two's complement.
     */
    int dx = (int)raw_dx;
    int dy = (int)raw_dy;

    if (flags & 0x10) dx |= 0xFFFFFF00;  /* X negative */
    if (flags & 0x20) dy |= 0xFFFFFF00;  /* Y negative (PS/2 up) */

    mouse.dx = dx;
    mouse.dy = dy;

    /*
     * update cursor position using physical accumulators.
     *
     * mouse deltas are in physical pixel space (1280x960).
     * we accumulate in physical space, then divide by MOUSE_SCALE
     * to get the logical position (640x480).
     *
     * PS/2 Y-axis is INVERTED from screen coordinates:
     *   PS/2: positive dy = mouse moving UP
     *   screen: positive y = further DOWN
     * so we SUBTRACT dy.
     */
    mouse_phys_x += dx;
    mouse_phys_y -= dy;

    /* clamp physical to bounds */
    if (mouse_phys_x < 0) mouse_phys_x = 0;
    if (mouse_phys_y < 0) mouse_phys_y = 0;
    if (mouse_phys_x >= MOUSE_SCREEN_WIDTH * MOUSE_SCALE)
        mouse_phys_x = MOUSE_SCREEN_WIDTH * MOUSE_SCALE - 1;
    if (mouse_phys_y >= MOUSE_SCREEN_HEIGHT * MOUSE_SCALE)
        mouse_phys_y = MOUSE_SCREEN_HEIGHT * MOUSE_SCALE - 1;

    /* convert to logical coordinates */
    mouse.x = mouse_phys_x / MOUSE_SCALE;
    mouse.y = mouse_phys_y / MOUSE_SCALE;

    return 1;  /* complete packet processed! */
}

#endif /* MOUSE_H */
