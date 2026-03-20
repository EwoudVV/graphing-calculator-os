/*
 * keyboard.h - read keys from the keyboard
 *
 * UPDATED: now returns int instead of char, so we can represent
 * special keys (arrows, etc) alongside regular ASCII.
 *
 * arrow keys are "extended" scancodes - the keyboard sends TWO bytes:
 *   first 0xE0 (the "this is a special key" prefix)
 *   then the actual scancode (0x48=up, 0x50=down, 0x4B=left, 0x4D=right)
 *
 * we track the 0xE0 prefix with a flag, and on the next read
 * we know to check the extended scancode table instead.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ports.h"

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/* special key codes (above ASCII range so they don't conflict) */
#define KEY_NONE    0
#define KEY_UP      256
#define KEY_DOWN    257
#define KEY_LEFT    258
#define KEY_RIGHT   259

/* shift scancodes */
#define SCANCODE_LEFT_SHIFT_PRESS    0x2A
#define SCANCODE_LEFT_SHIFT_RELEASE  0xAA
#define SCANCODE_RIGHT_SHIFT_PRESS   0x36
#define SCANCODE_RIGHT_SHIFT_RELEASE 0xB6
#define SCANCODE_EXTENDED_PREFIX     0xE0

/* extended scancodes (sent after 0xE0) */
#define SCANCODE_EXT_UP    0x48
#define SCANCODE_EXT_DOWN  0x50
#define SCANCODE_EXT_LEFT  0x4B
#define SCANCODE_EXT_RIGHT 0x4D

/* UNSHIFTED scancode-to-ASCII table */
static const char scancode_to_ascii[128] = {
    0,   27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,  ' '
};

/* SHIFTED scancode-to-ASCII table */
static const char scancode_to_ascii_shifted[128] = {
    0,   27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0,  ' '
};

/* keyboard state */
static int shift_held = 0;
static int extended_key = 0;  /* 1 = next scancode is an extended key */

/*
 * check if there's KEYBOARD data ready to read.
 *
 * IMPORTANT: both keyboard and mouse share port 0x60!
 * the status register at port 0x64 tells us which device sent the data:
 *   bit 0 = 1 means "there's data on port 0x60"
 *   bit 5 = 1 means "the data is from the MOUSE" (auxiliary device)
 *   bit 5 = 0 means "the data is from the KEYBOARD"
 *
 * if we don't check bit 5, we'll accidentally read mouse movement
 * bytes as keyboard scancodes = garbage characters in the input box!
 */
static inline int keyboard_has_data(void) {
    uint8_t status = inb(KEYBOARD_STATUS_PORT);
    /* data must be available (bit 0) AND it must NOT be from the mouse (bit 5) */
    return (status & 0x01) && !(status & 0x20);
}

/*
 * read one key from the keyboard.
 * returns: ASCII char for regular keys, KEY_UP/DOWN/LEFT/RIGHT for arrows, 0 for nothing
 */
static inline int keyboard_read_key(void) {
    if (!keyboard_has_data()) return KEY_NONE;

    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    /* 0xE0 prefix means the NEXT scancode is an extended key (arrows etc) */
    if (scancode == SCANCODE_EXTENDED_PREFIX) {
        extended_key = 1;
        return KEY_NONE;
    }

    /* handle extended scancodes (arrow keys) */
    if (extended_key) {
        extended_key = 0;
        if (scancode & 0x80) return KEY_NONE; /* ignore extended key releases */
        switch (scancode) {
            case SCANCODE_EXT_UP:    return KEY_UP;
            case SCANCODE_EXT_DOWN:  return KEY_DOWN;
            case SCANCODE_EXT_LEFT:  return KEY_LEFT;
            case SCANCODE_EXT_RIGHT: return KEY_RIGHT;
            default: return KEY_NONE;
        }
    }

    /* handle shift state */
    if (scancode == SCANCODE_LEFT_SHIFT_PRESS ||
        scancode == SCANCODE_RIGHT_SHIFT_PRESS) {
        shift_held = 1;
        return KEY_NONE;
    }
    if (scancode == SCANCODE_LEFT_SHIFT_RELEASE ||
        scancode == SCANCODE_RIGHT_SHIFT_RELEASE) {
        shift_held = 0;
        return KEY_NONE;
    }

    /* ignore other key releases */
    if (scancode & 0x80) return KEY_NONE;

    /* translate to ASCII */
    if (scancode < sizeof(scancode_to_ascii)) {
        if (shift_held)
            return (int)(unsigned char)scancode_to_ascii_shifted[scancode];
        else
            return (int)(unsigned char)scancode_to_ascii[scancode];
    }

    return KEY_NONE;
}

#endif
