/*
 * ports.h - talk to hardware through I/O ports
 *
 * the CPU has a separate address space just for hardware devices.
 * each device listens on specific port numbers:
 *   - port 0x60 = keyboard data
 *   - port 0x64 = keyboard status
 *   - port 0x20 = interrupt controller (PIC)
 *
 * "inline assembly" lets us use x86 instructions (in/out)
 * directly from C code, since C has no built-in way to do port I/O
 */

#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>

/* read one byte from a hardware port
   the "in" instruction reads from the port into AL register */
static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

/* write one byte to a hardware port
   the "out" instruction sends a byte from AL to the port */
static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

#endif
