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

# === ISO BUILD TARGET ===
#
# Creates a bootable CD-ROM image (.iso) using GRUB as the bootloader.
# This is useful for:
#   - booting on real hardware (burn to USB/CD)
#   - distributing your OS as a single file anyone can run
#   - testing with GRUB's boot menu (closer to real hardware behavior)
#
# How it works:
#   1. Create a directory structure that GRUB expects:
#        iso/boot/kernel.bin    (our kernel)
#        iso/boot/grub/grub.cfg (GRUB's configuration file)
#   2. Run grub-mkrescue, which packages everything into a bootable ISO
#      that includes GRUB itself + our kernel + the config
#
# The ISO contains a tiny GRUB bootloader that loads our kernel using
# the multiboot protocol -- the same protocol QEMU's -kernel flag uses.
iso: kernel.bin
	mkdir -p iso/boot/grub
	cp kernel.bin iso/boot/kernel.bin
	echo 'set timeout=0'                    >  iso/boot/grub/grub.cfg
	echo 'set default=0'                    >> iso/boot/grub/grub.cfg
	echo ''                                 >> iso/boot/grub/grub.cfg
	echo 'menuentry "GraphCalcOS" {'        >> iso/boot/grub/grub.cfg
	echo '    multiboot /boot/kernel.bin'   >> iso/boot/grub/grub.cfg
	echo '    boot'                         >> iso/boot/grub/grub.cfg
	echo '}'                                >> iso/boot/grub/grub.cfg
	i686-elf-grub-mkrescue -o graphcalcos.iso iso

# boot the ISO in QEMU (uses -cdrom instead of -kernel)
# this tests the full GRUB boot path, just like real hardware would
run-iso: graphcalcos.iso
	qemu-system-i386 -vga std -cdrom graphcalcos.iso

clean:
	rm -f boot/*.o kernel/*.o kernel.bin graphcalcos.iso
	rm -rf iso

.PHONY: all run iso run-iso clean
