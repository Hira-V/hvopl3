/*
 * hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
 * Copyright (C) 2026 Kevin H. Patterson
 * SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
 * Hira-V: https://hira-v.com
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  . = 0x00001000;
  .text   : { KEEP(*(.text.start)) *(.text*) }
  .rodata : { *(.rodata*) *(.srodata*) }
  .init_array : { PROVIDE(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE(__init_array_end = .); }
  .preinit_array : { PROVIDE(__preinit_array_start = .); KEEP(*(.preinit_array)) PROVIDE(__preinit_array_end = .); }
  .fini_array : { PROVIDE(__fini_array_start = .); KEEP(*(.fini_array)) PROVIDE(__fini_array_end = .); }
  .data   : { *(.data*) *(.sdata*) }
  . = ALIGN(8);
  .tohost : { *(.tohost) }
  . = ALIGN(4);
  _sbss = .;
  .bss    : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(4);
  _ebss = .;
  _end = .;
  PROVIDE(__global_pointer$ = 0);
  _estack = 0x00400000;
}
