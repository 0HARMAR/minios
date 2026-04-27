/* Linker script for user programs.
 * Programs are linked at BASE (set by the build script).
 * Only .text / .rodata / .data kept; everything else discarded. */

SECTIONS {
    . = BASE;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }
    /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
