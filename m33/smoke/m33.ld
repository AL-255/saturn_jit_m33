/* Linker script for QEMU mps2-an505 (Cortex-M33).
 * Per the MPS2 AN505 doc, code memory is at 0x10000000 (NS image
 * region) and SRAM at 0x38000000 (data SRAM, 4 MiB). We don't use
 * TrustZone so we boot from 0x10000000 as the NS reset vector. */

ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x10000000, LENGTH = 4M
  RAM   (rwx): ORIGIN = 0x38000000, LENGTH = 4M
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

  .text : {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > FLASH

  _sidata = LOADADDR(.data);
  .data : {
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) : {
    _sbss = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
