ENTRY(_start)
SECTIONS {
    . = 0x300000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
