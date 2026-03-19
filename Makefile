# makefile for GraphCalcOS
#
# NASM assembles boot.asm -> boot.o  (assembly -> object file)
# i686-elf-gcc compiles kernel.c -> kernel.o  (C -> object file)
# i686-elf-gcc links boot.o + kernel.o -> kernel.bin  (final binary)
# QEMU boots kernel.bin as a multiboot kernel

# cross-compiler tools
ASM = nasm
CC = i686-elf-gcc
LD = i686-elf-gcc

# compiler flags:
#   -ffreestanding  = no standard library
#   -O2             = optimization level 2
#   -Wall -Wextra   = enable all warnings
#   -nostdlib       = dont link the standard C library
#   -mno-sse        = dont use SSE instructions
CFLAGS = -ffreestanding -O2 -Wall -Wextra -nostdlib -mno-sse -mno-sse2
LDFLAGS = -T linker.ld -nostdlib -lgcc

# object files that make up our kernel
OBJECTS = boot/boot.o kernel/kernel.o

# default target: build the kernel
all: kernel.bin

# link all object files into the kernel binary
kernel.bin: $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^ -lgcc

# assemble .asm files into .o object files
boot/boot.o: boot/boot.asm
	$(ASM) -f elf32 $< -o $@

# compile .c files into .o object files
kernel/%.o: kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# build and run in QEMU
# -kernel flag tells QEMU to load the binary as a multiboot kernel
# QEMU has a built-in multiboot loader, no grub ISO needed
run: kernel.bin
	qemu-system-i386 -vga std -kernel kernel.bin

clean:
	rm -f boot/*.o kernel/*.o kernel.bin

.PHONY: all run clean