/*
 * ------------------------------------------------------------
 *  32-bit Kernel Loader (Stage 2)
 * ------------------------------------------------------------
 *  Loaded at 0x7E00 by bootloader
 *  Switches from real mode to protected mode
 *  Then boots into 64-bit mode
 * ------------------------------------------------------------
 */

.code16
.section .text
.global _start
.extern boot_64

/* ============================================================
 *  Constants
 * ============================================================ */

/* GDT selectors (index << 3) */
.set CODE_SEL, 0x08
.set DATA_SEL, 0x10

/* Stack location in protected mode */
.set PM_STACK_TOP, 0x90000

/* VGA text buffer */
.set VGA_TEXT_BUF, 0xB8000


/* ============================================================
 *  Entry (Real Mode)
 * ============================================================ */
_start:
    cli                         # Disable interrupts

    call enable_a20             # Enable A20 line

    lgdt gdt_desc               # Load GDT

    /* ---- Enable Protected Mode (CR0.PE = 1) ---- */
    movl %cr0, %eax
    orl  $0x1, %eax
    movl %eax, %cr0

    /* Far jump to flush pipeline and load CS */
    ljmp $CODE_SEL, $protected_mode_entry


/* ============================================================
 *  Protected Mode Entry
 * ============================================================ */
.code32
protected_mode_entry:

    /* ---- Load data segment registers ---- */
    movw $DATA_SEL, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    /* ---- Setup stack ---- */
    movl $PM_STACK_TOP, %esp

    /* ---- Debug output to VGA text memory ---- */
    movl $VGA_TEXT_BUF, %edi
    movw $0x0750, %ax           # 'P' + light grey attribute
    movw %ax, (%edi)

    /* ---- Boot 64-bit mode ---- */
    call boot_64

halt:
    hlt
    jmp halt


/* ============================================================
 *  Enable A20 (Fast A20 method via port 0x92)
 * ============================================================ */
enable_a20:
    inb  $0x92, %al
    orb  $0x02, %al             # Set A20 enable bit
    outb %al, $0x92
    ret


/* ============================================================
 *  Global Descriptor Table
 * ============================================================ */
.align 8
gdt:

    /* Null Descriptor (required) */
    .quad 0x0000000000000000

    /* Code Segment Descriptor
     * base=0x00000000
     * limit=0xFFFFF (4GB with granularity)
     * access=0x9A (present, ring0, code, readable)
     * flags=0xCF (4KB granularity, 32-bit)
     */
    .quad 0x00CF9A000000FFFF

    /* Data Segment Descriptor
     * access=0x92 (present, ring0, data, writable)
     */
    .quad 0x00CF92000000FFFF

gdt_end:


/* GDT Descriptor */
gdt_desc:
    .word (gdt_end - gdt - 1)    # limit
    .long gdt                   # base
    