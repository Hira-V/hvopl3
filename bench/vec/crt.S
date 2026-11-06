// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
#if __riscv_xlen == 64
#define STX sd
#define WORD 8
#else
#define STX sw
#define WORD 4
#endif
	.section .text.start
	.globl _start
_start:
	la sp, __stack
	li t0, 0x2000      /* FS */
	csrs mstatus, t0
#ifdef __riscv_vector
	li t0, 0x200       /* VS */
	csrs mstatus, t0
#endif
	la t0, __bss_start
	la t1, __bss_end
1:	bgeu t0, t1, 2f
	STX zero, 0(t0)
	addi t0, t0, WORD
	j 1b
2:	call main
	li t0, 0x100000   /* qemu virt: power off */
	li t1, 0x5555
	sw t1, 0(t0)
3:	j 3b
