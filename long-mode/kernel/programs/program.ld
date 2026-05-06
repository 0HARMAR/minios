/* Linker script for user programs.
 * Programs are linked at BASE (set by the build script).
 * Only .text / .rodata / .data kept; everything else discarded. */

ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    data   PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS {
    . = BASE;
    .text   : { *(.text*)   } :text
    .rodata : { *(.rodata*) } :text
    .data   : { *(.data*)   } :data
    .bss    : { *(.bss*)    } :data
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
