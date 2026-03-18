/*
 * keyboard.h - read keys from the keyboard
 *
 * how it works at the hardware level:
 * 1. you press a key
 * 2. the keyboard controller puts a "scancode" at port 0x60
 * 3. port 0x64 bit 0 goes high (= "there's data to read")
 * 4. we read the scancode from port 0x60
 * 5. we look up which character that scancode maps to
 *
 * scancodes are NOT ASCII! they're arbitrary numbers the keyboard uses.
 * for example: pressing 'A' sends scancode 0x1E, not 0x41 (ASCII 'A')
 * so we need a lookup table to translate them.
 *
 * "key press" sends one scancode, "key release" sends scancode + 0x80
 * we only care about key presses (scancodes < 0x80)
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include "ports.h"

#define KEYBOARD_DATA_PORT   0x60
#define KEYBOARD_STATUS_PORT 0x64

/*
 * scancode-to-ASCII lookup table (US keyboard layout, scan code set 1)
 *
 * index = scancode from keyboard
 * value = ASCII character (0 means "no printable character")
 *
 * this only covers the basic keys. shift, ctrl, etc need
 * special handling we'll add later.
 */
static const char scancode_to_ascii[128] = {
    0,   27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0,  ' '
};

/* check if the keyboard has data ready to read
   bit 0 of port 0x64 = 1 means "yes, there's a scancode waiting" */
static inline int keyboard_has_data(void) {
    return inb(KEYBOARD_STATUS_PORT) & 0x01;
}

/* read one character from the keyboard
   returns the ASCII character, or 0 if no key / non-printable key */
static inline char keyboard_read_char(void) {
    /* wait until there's data */
    if (!keyboard_has_data()) {
        return 0;
    }

    /* read the scancode from the keyboard */
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    /* ignore key releases (scancode >= 0x80)
       we only want key presses */
    if (scancode & 0x80) {
        return 0;
    }

    /* translate scancode to ASCII using our lookup table */
    if (scancode < sizeof(scancode_to_ascii)) {
        return scancode_to_ascii[scancode];
    }

    return 0;
}

#endif
