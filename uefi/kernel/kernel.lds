/*
 * kernel.lds - flat-binary kernel linked at physical 0x100000.
 *
 * BOOTX64.EFI loads the resulting binary at exactly this address, so the
 * loader and this linker script must agree on KERNEL_LOAD_ADDR.
 */
ENTRY(_start)
SECTIONS
{
  . = 0x100000;
  .text   : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data   : { *(.data*) }
  .bss    : { *(.bss*) *(COMMON) }
  _end = .;

  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
