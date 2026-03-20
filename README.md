# Graphing Calculator OS

An operating system built from scratch - no Linux, no Windows, only hardware.

Type in a math equation and see it graphed on screen. The OS handles everything: keyboard input, mouse input, equation parsing, and pixel-by-pixel plotting. It even supports implicit equations like circles.

## what it does

- boots straight into a graphing calculator
- type equations and see them graphed with an animated sweep
- supports up to 6 equations at once, each in a different color
- implicit equations work too (like `x^2+y^2=9` for circles)
- mouse drag to pan, `[` `]` to zoom
- trace mode: move the mouse along any curve to see exact coordinates
- tangent lines with slope display (works on circles too!)
- plot the derivative of any equation as a new curve
- zero markers (where curves cross the x-axis) and intersection markers
- click an equation in the panel to delete it
- math functions: `sin`, `cos`, `tan`, `sqrt`, `abs`, `ln`, `log`, `exp`
- constants: `pi`, `e`
- 1280x960 display with 2x pixel scaling
- ships as a bootable ISO

## how to run

### option 1: download the ISO
 - download the `.iso` file
 - create a virtual machine (virtualbox, utm, etc)
 - OR flash the iso to a usb stick and boot your computer directly off it
 - make sure UEFI boot is OFF, architecture is x86_64, and display is VGA

### option 2: run with qemu directly
```
qemu-system-i386 -cdrom graphcalcos.iso -vga std
```

### option 3: build from source
```
make run
```

## dependencies

you need a cross-compiler and an emulator to build and run this:

- **nasm** - assembler for the bootloader
- **i686-elf-gcc** - cross-compiler that targets bare metal x86
- **i686-elf-grub** - for building the bootable ISO
- **qemu** - emulates a PC so you don't have to flash a USB every time

### install on mac (homebrew)

```
brew install nasm qemu
brew install i686-elf-gcc i686-elf-binutils i686-elf-grub
```

## build commands

```
make run       - build and run in QEMU
make iso       - build the bootable ISO (graphcalcos.iso)
make run-iso   - build ISO and boot it in QEMU
make clean     - delete build files for a fresh build
```

## project structure

```
graphing-calculator-os/
├── boot/
│   └── boot.asm            - bootloader (x86 asm). sets up stack,
│                              inits the FPU, jumps to C kernel
├── kernel/
│   ├── kernel.c            - the entire OS. UI, plotting, trace,
│   │                         tangent lines, mouse, keyboard, main loop
│   ├── math_parser.h       - equation parser + bytecode compiler + VM.
│   │                         compiles "x^2+1" to bytecode, runs it
│   │                         thousands of times per frame
│   ├── vga.h               - graphics driver. 1280x960x32bpp via Bochs
│   │                         VBE, double buffering, 2x pixel scaling
│   ├── keyboard.h          - PS/2 keyboard driver. scancodes from port
│   │                         0x60, shift support
│   ├── mouse.h             - PS/2 mouse driver. non-blocking state
│   │                         machine, 3-byte packet collection
│   ├── font.h              - 8x8 bitmap font for text rendering
│   └── ports.h             - x86 I/O port functions (inb/outb/inw/outw)
├── iso/
│   └── boot/
│       └── grub/
│           └── grub.cfg    - GRUB bootloader config for the ISO
├── linker.ld               - memory layout for the kernel binary
├── Makefile                - builds everything + creates bootable ISO
└── graphcalcos.iso         - the bootable ISO (after make iso)
```

## controls

| key | action |
|-----|--------|
| type + Enter | plot an equation |
| arrows | pan the graph |
| `[` / `]` | zoom out / in |
| Tab | clear all equations |
| click equation | delete that equation |
| mouse drag | pan the graph |
| Shift+T | toggle trace mode |
| mouse (in trace) | move along curve |
| Up/Down (in trace) | switch equation |
| Shift+D (in trace) | toggle tangent line |
| Shift+F (in trace) | plot derivative curve |

made for [Hack Club Boot](https://boot.hackclub.com)!
