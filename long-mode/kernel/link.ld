/* Kernel linker script.
 * Identity-mapped, loaded at 0x7E00 by bootloader.
 * _kernel_load_end marks where programs.bin follows on disk (after .data, before .bss).
 * _kernel_end marks where BSS ends (runtime zero-init boundary). */
SECTIONS {
    . = 0x7E00;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    _kernel_load_end = .;
    .bss    : { *(.bss*) }
    _kernel_end = .;
    /* NIC buffers must NOT live in .bss: exec_init zeroes
     * [_kernel_load_end, _kernel_end) at boot, and .bss growth would
     * collide with the kernel stack (0x8F000-0x90000, RSP=0x90000).
     * Place them above the stack — but NOT below 1MB: [0xA0000, 0xC0000)
     * is the legacy VGA framebuffer (writes get eaten by the device).
     * 0x180000 is free RAM (kernel image ends ~0x17400, user space
     * starts at 0x200000). NOLOAD → not in kernel.bin. */
    . = 0x180000;
    .netbuf (NOLOAD) : { *(.netbuf*) }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
