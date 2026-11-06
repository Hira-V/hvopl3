// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// sysq.cpp -- newlib's system calls for a host-style test (tests/t_ext.cpp) bare metal on qemu's virt
// machine: its UART for stdout, a heap after the program, exit powers off.
//
#include <stdint.h>
#include <stddef.h>
extern "C" {
extern char __bss_end[];
static char *brk_ = __bss_end;
void *_sbrk(ptrdiff_t n) { char *p = brk_; brk_ += (n + 15) & ~15; return p; }
int _write(int, const char *b, int n) { for (int i = 0; i < n; ++i) *(volatile uint8_t *)0x10000000 = uint8_t(b[i]); return n; }
int _read(int, char *, int) { return 0; } int _close(int) { return -1; } int _isatty(int) { return 1; }
int _lseek(int, int, int) { return 0; } int _kill(int, int) { return -1; } int _getpid() { return 1; }
void _exit(int) { *(volatile uint32_t *)0x100000 = 0x5555; for (;;) {} }
struct stat; int _fstat(int, struct stat *) { return 0; }
void *__dso_handle = 0;
}
