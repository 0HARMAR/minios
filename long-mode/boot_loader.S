/*
 * ------------------------------------------------------------
 *  Simple Bootloader
 * ------------------------------------------------------------
 *  Function:
 *      - Runs in 16-bit real mode
 *      - Loaded by BIOS at 0x7C00
 *      - Loads SECTORS sectors from boot disk into 0x7E00
 *      - Jumps to loaded kernel
 *
 *  Disk Layout:
 *      - Boot sector: LBA 0  (this file)
 *      - Kernel:      LBA 1~SECTORS
 *
 *  Author: <harmar>
 * ------------------------------------------------------------
 */

.code16
.section .text
.global _start

/* ------------------------------------------------------------
 * Boot Entry (BIOS loads here at 0x7C00)
 * ------------------------------------------------------------ */
_start:

    /* ---- Initialize segment registers ---- */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7C00, %sp        # stack grows downward from 0x7C00

    /* ---- Print loading message ---- */
    movw $loading_msg, %si
    call print_string

    /* ---- Save boot drive number and reset disk ---- */
    movb %dl, boot_drive
    xorw %ax, %ax            # AH=0 reset disk
    int $0x13
    jc disk_error            # CF set on error

    /* --------------------------------------------------------
     * Read kernel from disk (CHS: cylinder 0, sector 2, head 0)
     * -------------------------------------------------------- */
    movb $0x02, %ah          # function: read sectors
.ifndef SECTORS
    .set SECTORS, 55
.endif
    movb $SECTORS, %al        # number of sectors
    movb $0x00, %ch          # cylinder
    movb $0x02, %cl          # sector (starts from 1)
    movb $0x00, %dh          # head
    movw $0x7E00, %bx        # load address
    movb boot_drive, %dl      # use BIOS-provided drive number
    int $0x13
    jc disk_error

    /* ---- Jump to loaded kernel ---- */
    ljmp $0x0000, $0x7E00

/* ------------------------------------------------------------
 * Disk Error Handler
 * ------------------------------------------------------------ */
disk_error:
    movw $error_msg, %si
    call print_string
.hang:
    jmp .hang

/* ------------------------------------------------------------
 * print_string
 *
 * Input:
 *      DS:SI -> zero-terminated string
 *
 * Uses:
 *      INT 10h AH=0Eh (TTY output)
 * ------------------------------------------------------------ */
print_string:
    pusha
    movb $0x0E, %ah

.print_loop:
    lodsb                    # AL = [DS:SI++]
    testb %al, %al
    jz .done
    int $0x10
    jmp .print_loop

.done:
    popa
    ret


/* ------------------------------------------------------------
 * Data Section
 * ------------------------------------------------------------ */
boot_drive:  .byte 0
loading_msg: .asciz "Loading OS Kernel..."
error_msg:   .asciz "Disk Error"


/* ------------------------------------------------------------
 * Boot Signature (must be at byte 510-511)
 * ------------------------------------------------------------ */
.fill 510 - (. - _start), 1, 0
.word 0xAA55
