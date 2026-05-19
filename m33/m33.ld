/* Linker script for QEMU mps2-an505 (Cortex-M33). Shared by all
 * firmware targets in this tree. */

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
    *(.text*) *(.rodata*)
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

  /* JIT code cache lives in RAM, marked executable.
   * Size is configurable via the linker symbol `_jit_cache_size`, which
   * the Makefile passes through with --defsym (default 256K). This lets
   * the cache-size sweep benchmark rebuild for each size without
   * patching the linker script. */
  PROVIDE(_jit_cache_size = 256 * 1024);
  .jitcache (NOLOAD) : ALIGN(64) {
    _jit_cache_start = .;
    . = . + _jit_cache_size;
    _jit_cache_end = .;
  } > RAM

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
