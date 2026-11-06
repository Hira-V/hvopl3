/*
 * hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
 * Copyright (C) 2026 Kevin H. Patterson
 * SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
 * Hira-V: https://hira-v.com
 */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256M }
SECTIONS {
  . = 0x80000000;
  .text : { *(.text.start) *(EXCLUDE_FILE(*hvopl3.o) .text*) __hv_lo = .; *hvopl3.o(.text*) __hv_hi = .; *(.rodata*) *(.srodata*) } > RAM
  .data : { *(.data*) *(.sdata*) } > RAM
  .bss : { __bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(8); __bss_end = .; } > RAM
  __stack = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.riscv.attributes) }
}
