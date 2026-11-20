/*
 * handoff.S - leave the UEFI environment and enter the 64-bit kernel.
 *
 * void minios_handoff(u64 boot_info, u64 entry, u64 page_table, u64 stack_top)
 *
 * The firmware console and boot services are already gone when this runs.
 * Load our own CR3/GDT, switch to the loader-provided stack and enter the
 * kernel with the SysV ABI (boot_info in RDI).
 */
.section .text
.global minios_handoff
.type minios_handoff, @function
minios_handoff:
    cli

    /* Switch to the identity-mapped page tables built by the loader. */
    movq %rdx, %cr3

    /* Relocate the GDT base at runtime, so the image works even if the
       firmware applied PE base relocations and loaded us elsewhere. */
    leaq gdt64(%rip), %rax
    movq %rax, gdt64_ptr+2(%rip)
    lgdt gdt64_ptr(%rip)

    /* Far return to a 64-bit code selector. */
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq
1:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movq %rcx, %rsp
    xorl %ebp, %ebp

    /* RDI still holds boot_info; RSI holds the kernel entry point. */
    call *%rsi

2:
    cli
    hlt
    jmp 2b
.size minios_handoff, . - minios_handoff

.section .data
.align 8
gdt64:
    .quad 0x0000000000000000        /* null descriptor                     */
    .quad 0x00AF9A000000FFFF        /* 64-bit code: L=1, G=1, ring 0       */
    .quad 0x00CF92000000FFFF        /* data: G=1, ring 0                   */
gdt64_end:

gdt64_ptr:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

.section .note.GNU-stack,"",@progbits
