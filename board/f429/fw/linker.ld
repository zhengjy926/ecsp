/*
 * STM32F429IGT6, 1 MB Flash, single bank (DB1M clear).
 * Sector 11 (0x080E0000, 128 KB) is reserved for identity records (#13/#18);
 * this image must not occupy it.
 */

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 896K
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 192K
}

ENTRY(Reset_Handler)

SECTIONS
{
  .isr_vector : {
    KEEP(*(.isr_vector))
  } > FLASH

  .text : {
    *(.text*)
    *(.rodata*)
    KEEP(*(.init))
    KEEP(*(.fini))
    _etext = .;
  } > FLASH

  _sidata = LOADADDR(.data);

  .data : {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > RAM AT> FLASH

  .bss : {
    _sbss = .;
    *(.bss*)
    *(COMMON)
    _ebss = .;
  } > RAM

  ._user_heap_stack : {
    . = ALIGN(8);
    . = . + 0x400;
    . = ALIGN(8);
    _estack = ORIGIN(RAM) + LENGTH(RAM);
  } > RAM
}
