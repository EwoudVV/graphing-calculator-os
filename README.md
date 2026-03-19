# Graphing Calculator OS

An operating system built from scratch - no Linux, no Windows, only hardware.

Type in a math equation and see it graphed on screen. The OS handles keyboard input, parses the equation, and plots it pixel by pixel.

## what it does (and will do)

- boots into a graphing calculator
- keyboard input so you can type equations
- draws axes, a grid, and plots the graph on screen
- supports basic math operations (more coming later)

## dependencies

you need a cross-compiler and an emulator to build and run this:

- **nasm** - assembler for the bootloader
- **i686-elf-gcc** - cross-compiler that targets bare metal x86
- **qemu** - emulates a PC so you don't have to flash a USB every time

### install on mac (homebrew)

```
brew install nasm qemu
brew install i686-elf-gcc i686-elf-binutils
```

## how to run

```
make run
```

it builds everything and opens a QEMU window. close the window to stop it.

`make clean` to delete the build files if you want a fresh build.

## project structure

```
boot/boot.asm      - bootloader, first code that runs (x86 assembly)
kernel/kernel.c    - the kernel, all the main OS logic (C)
kernel/keyboard.h  - keyboard driver, reads keys from hardware ports
kernel/ports.h     - low-level I/O port functions (inb/outb)
linker.ld          - tells the linker how to lay out the kernel in memory
Makefile           - builds and runs everything
```

made for [Hack Club Boot](https://boot.hackclub.com)!
