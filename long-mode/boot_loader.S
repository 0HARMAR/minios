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
     * Read kernel from disk using LBA (int 13h AH=42h).
     * One sector per BIOS call: a single CHS read of >127
     * sectors is rejected by the BIOS, and large transfers can
     * cross a 64 KB boundary.
     * -------------------------------------------------------- */
.ifndef SECTORS
    .set SECTORS, 55
.endif

    /* ---- Check int 13h LBA extensions ---- */
    movb $0x41, %ah
    movw $0x55AA, %bx
    movb boot_drive, %dl
    int $0x13
    jc disk_error
    cmpw $0xAA55, %bx
    jne disk_error

    movw $SECTORS, remaining
    movl $1, dap_lba

read_loop:
    movb $0x42, %ah          # function: extended read
    xorb %al, %al
    movw $dap, %si
    movb boot_drive, %dl
    int $0x13
    jc disk_error

    /* advance destination: offset += 512, carry -> segment += 0x1000 */
    addw $0x200, dap_off
    jnc .no_seg_wrap
    addw $0x1000, dap_seg
.no_seg_wrap:
    addl $1, dap_lba
    decw remaining
    jnz read_loop

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
remaining:   .word 0
loading_msg: .asciz "Loading OS Kernel..."
error_msg:   .asciz "Disk Error"

/* Disk Address Packet for int 13h AH=42h (16 bytes) */
    .align 4
dap:
    .byte 0x10, 0x00         # packet size, reserved
    .word 1                  # sector count (one per call)
dap_off:
    .word 0                  # destination offset (starts at 0x7E00 via seg)
dap_seg:
    .word 0x07E0             # destination segment (0x07E0:0 = 0x7E00)
dap_lba:
    .long 1                  # LBA (payload starts at sector 1)
    .long 0


/* ------------------------------------------------------------
 * Boot Signature (must be at byte 510-511)
 * ------------------------------------------------------------ */
.fill 510 - (. - _start), 1, 0
.word 0xAA55
