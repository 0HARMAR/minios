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
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
