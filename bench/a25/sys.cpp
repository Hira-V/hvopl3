// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// sys.cpp -- newlib's system calls for a host-style test (tests/t_ext.cpp) under the repo's rv32emu: its
// UART for stdout, a heap above the stack.
//
#include <stdint.h>
#include <stddef.h>
#define UART_RXTX 0xe8002000u
#define UART_TXFULL 0xe8002004u
static void putc_(char c) { while (*(volatile uint32_t *)UART_TXFULL) {} *(volatile uint32_t *)UART_RXTX = uint8_t(c); }
extern "C" {
static char *brk_ = reinterpret_cast<char *>(0x00400000);
void *_sbrk(ptrdiff_t n) { char *p = brk_; brk_ += (n + 15) & ~15; return p; }
int _write(int, const char *b, int n) { for (int i = 0; i < n; ++i) putc_(b[i]); return n; }
int _read(int, char *, int) { return 0; } int _close(int) { return -1; } int _isatty(int) { return 1; }
int _lseek(int, int, int) { return 0; } void _exit(int) { for (;;) {} } int _kill(int, int) { return -1; } int _getpid() { return 1; }
struct stat; int _fstat(int, struct stat *) { return 0; }
void *__dso_handle = 0;
}
