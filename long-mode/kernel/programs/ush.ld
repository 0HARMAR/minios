ENTRY(_start)
PHDRS
{
    text   PT_LOAD FLAGS(5);
    data   PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x300000;
    .text   : { *(.text*) }   :text
    .rodata : { *(.rodata*) } :text
    .data   : { *(.data*) }   :data
    .bss    : { *(.bss*) }    :data
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
