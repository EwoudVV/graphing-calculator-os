/*
 * ports.h - talk to hardware through I/O ports
 *
 * the CPU has a separate address space just for hardware devices.
 * each device listens on specific port numbers:
 *   - port 0x60 = keyboard data
 *   - port 0x64 = keyboard status
 *   - port 0x20 = interrupt controller (PIC)
 *   - ports 0x01CE/0x01CF = Bochs VBE display (for higher resolutions)
 *   - ports 0x0CF8/0x0CFC = PCI configuration (to find hardware addresses)
 *
 * "inline assembly" lets us use x86 instructions (in/out)
 * directly from C code, since C has no built-in way to do port I/O.
 *
 * we have 8-bit, 16-bit, and 32-bit versions because different
 * hardware expects different data sizes:
 *   - keyboard uses 8-bit (inb/outb)
 *   - VBE display uses 16-bit (inw/outw)
 *   - PCI config uses 32-bit (inl/outl)
 */

#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>

/* === 8-bit port I/O (most hardware) === */

static inline uint8_t inb(uint16_t port) {
    uint8_t result;
    __asm__ volatile ("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile ("outb %0, %1" : : "a"(data), "Nd"(port));
}

/* === 16-bit port I/O (used by Bochs VBE) === */

static inline uint16_t inw(uint16_t port) {
    uint16_t result;
    __asm__ volatile ("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outw(uint16_t port, uint16_t data) {
    __asm__ volatile ("outw %0, %1" : : "a"(data), "Nd"(port));
}

/* === 32-bit port I/O (used by PCI) === */

static inline uint32_t inl(uint16_t port) {
    uint32_t result;
    __asm__ volatile ("inl %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static inline void outl(uint16_t port, uint32_t data) {
    __asm__ volatile ("outl %0, %1" : : "a"(data), "Nd"(port));
}

#endif
