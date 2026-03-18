# Makefile for GraphCalcOS
#
# Build pipeline:
#   1. NASM assembles boot.asm -> boot.o  (assembly -> object file)
#   2. i686-elf-gcc compiles kernel.c -> kernel.o  (C -> object file)
#   3. i686-elf-gcc links boot.o + kernel.o -> kernel.bin  (final binary)
#   4. QEMU boots kernel.bin as a Multiboot kernel

# Cross-compiler tools (these target i686, not your Mac's ARM CPU)
ASM = nasm
CC = i686-elf-gcc
LD = i686-elf-gcc

# Compiler flags explained:
#   -ffreestanding  = no standard library, no assumptions about the environment
#   -O2             = optimization level 2 (makes code faster)
#   -Wall -Wextra   = enable all warnings (catch bugs early!)
#   -nostdlib       = don't link the standard C library
#   -mno-sse        = don't use SSE instructions (needs extra setup in kernel)
CFLAGS = -ffreestanding -O2 -Wall -Wextra -nostdlib -mno-sse -mno-sse2
LDFLAGS = -T linker.ld -nostdlib -lgcc

# Object files that make up our kernel
OBJECTS = boot/boot.o kernel/kernel.o

# Default target: build the kernel
all: kernel.bin

# Link all object files into the final kernel binary
kernel.bin: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^ -lgcc

# Assemble .asm files into .o object files
boot/boot.o: boot/boot.asm
	$(ASM) -f elf32 $< -o $@

# Compile .c files into .o object files
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Build and run in QEMU
# -kernel flag tells QEMU to load our binary as a Multiboot kernel
# (QEMU has a built-in Multiboot loader -- no GRUB ISO needed!)
run: kernel.bin
	qemu-system-i386 -kernel kernel.bin

# Clean up build artifacts
clean:
	rm -f boot/*.o kernel/*.o kernel.bin

.PHONY: all run clean
