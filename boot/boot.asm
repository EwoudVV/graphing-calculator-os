; boot.asm - multiboot entry point
; this is the first code that runs when grub loads the kernel.
;
; what it does:
; declares a multiboot header so grub recognizes it as a valid kernel
; sets up a stack (the cpu needs a stack before it can call C functions)
; calls the C kernel main function (kmain)
; halts the CPU forever (nothing left to do)

; tell NASM this is generating 32-bit code (x86 protected mode)
bits 32

; ============================================================
; MULTIBOOT HEADER
; ============================================================
; grub scans the first 8KB of the kernel looking for the signature
; if it finds it, it knows this is a valid multiboot kernel and loads it
;
; the header has 3 fields that must add up correctly:
;   number + flags + checksum = 0

section .multiboot
    align 4                         ; must be 4-byte aligned
    dd 0x1BADB002                   ; grub looks for this exact value
    dd 0x00                         ; flags - 0 means no special requirements
    dd -(0x1BADB002 + 0x00)         ; checksum - must make all three sum to zero

; ============================================================
; STACK
; ============================================================
; the cpu needs a stack to call functions, it pushes return addresses there.
; it reserves 16KB of uninitialized memory for the stack
; stacks grow downward on x86, so it points the stack pointer to the top

section .bss
    align 16                        ; align to 16 bytes
    stack_bottom:
        resb 16384                  ; reserve 16KB (16384 bytes) for the stack
    stack_top:

; ============================================================
; ENTRY POINT
; ============================================================
; execution begins after grub loads it

section .text
    global _start                   ; make _start visible to the linker
    extern kmain                    ; kmain is defined in kernel.c

_start:
    ; set up the stack pointer
    ; ESP (Extended Stack Pointer) tells the cpu where the stack is
    mov esp, stack_top

    ; initialize the FPU (Floating Point Unit)
    ; the FPU is a separate math processor that handles decimal numbers.
    ; historically it was a separate chip (the 8087/80287/80387).
    ; now it's built into the CPU, but we still need to enable it.
    ;
    ; CR0 is a control register that configures CPU features:
    ;   bit 2 (EM) = "emulate FPU" - we CLEAR this (we have a real FPU)
    ;   bit 1 (MP) = "monitor coprocessor" - we SET this
    mov eax, cr0
    and eax, 0xFFFFFFFB         ; clear bit 2 (EM) - don't emulate, use real FPU
    or eax, 0x00000002          ; set bit 1 (MP) - enable FPU monitoring
    mov cr0, eax
    fninit                      ; initialize the FPU registers

    ; call the C kernel
    call kmain

    ; if kmain ever returns (it shouldnt), halt the cpu
    cli                             ; disable interrupts (so nothing wakes it up)
.halt:
    hlt                             ; halt the CPU
    jmp .halt