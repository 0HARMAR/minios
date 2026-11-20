/*
 * start.S - kernel entry point.
 *
 * The UEFI loader jumps here with RDI = struct minios_boot_info * and a
 * loader-provided stack.  kernel_main() is not expected to return.
 */
.section .text
.global _start
.type _start, @function
_start:
    xorl %ebp, %ebp
    subq $8, %rsp          /* align the stack for the SysV call below */
    call kernel_main

1:
    cli
    hlt
    jmp 1b
.size _start, . - _start

.section .note.GNU-stack,"",@progbits
