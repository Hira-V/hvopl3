// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// count.cpp -- the A25 cost of a chip over a trace, under rv32emu: the trace is linked in (.incbin,
// run.sh), the chip driven through the bench's interface, minstret counted around render() alone.
// Prints "<chip> <instructions> insns, frames <n>, per frame <x>" (frames at 48 kHz, chip 0's).
// The emulator counts instructions, not cycles: a board gives the cycles.
//
#include <stdint.h>
#include <stddef.h>
#include "../hvo1.hpp"
#include "../chip.hpp"

#define UART_RXTX 0xe8002000u
#define UART_TXFULL 0xe8002004u
static void putc_(char c) { while (*(volatile uint32_t *)UART_TXFULL) {} *(volatile uint32_t *)UART_RXTX = uint8_t(c); }
static void puts_(const char *s) { for (; *s; ++s) { if (*s == '\n') putc_('\r'); putc_(*s); } }
static void dec(uint64_t v) { char b[24]; int n = 0; do { b[n++] = char('0' + v % 10); v /= 10; } while (v); while (n) putc_(b[--n]); }
static inline uint64_t instret() {
	uint32_t hi, lo, h2;
	do { asm volatile("csrr %0, minstreth" : "=r"(hi)); asm volatile("csrr %0, minstret" : "=r"(lo)); asm volatile("csrr %0, minstreth" : "=r"(h2)); } while (hi != h2);
	return uint64_t(hi) << 32 | lo;
}
extern "C" {
static char *brk_ = reinterpret_cast<char *>(0x00400000);
void *_sbrk(ptrdiff_t n) { char *p = brk_; brk_ += (n + 15) & ~15; return p; }
int _write(int, const char *b, int n) { for (int i = 0; i < n; ++i) putc_(b[i]); return n; }
int _read(int, char *, int) { return 0; } int _close(int) { return -1; } int _isatty(int) { return 1; }
int _lseek(int, int, int) { return 0; } void _exit(int) { for (;;) {} } int _kill(int, int) { return -1; } int _getpid() { return 1; }
struct stat; int _fstat(int, struct stat *) { return 0; }
void *__dso_handle = 0;
void __libc_init_array();
extern const uint32_t trace_bin[], trace_end[];
}
static int32_t g_buf[3][2 * 512 * 2];

int main() {
	__libc_init_array();
	hvo1::Trace t;
	if (!t.open(trace_bin, size_t(trace_end - trace_bin))) { puts_("not HVO1\n"); return 1; }
	BenchChip *c = CHIP;
	if (!c) { puts_("no chip\n"); return 1; }
	const uint32_t scale = c->rate() / 48000;
	uint64_t ins = 0, frames0 = 0;
	for (const hvo1::Rec *r = t.begin(); r != t.end(); ++r) {
		const uint32_t k = r->chip();
		switch (r->op()) {
		case hvo1::RESET: c->reset(k); break;
		case hvo1::REG: c->write(k, r->addr(), uint8_t(r->data())); break;
		case hvo1::PAN: c->pan(k, r->addr(), uint8_t(r->data())); break;
		case hvo1::SENDS: c->sends(k, r->addr(), uint8_t(r->data()), uint8_t(r->data2())); break;
		case hvo1::GAIN: c->gain(k, r->addr(), r->level()); break;
		case hvo1::RUN: {
			for (uint32_t left = r->frames() * scale; left;) {
				const uint32_t n = left < 512 ? left : 512;
				const uint64_t c0 = instret();
				c->render(k, g_buf[0], g_buf[1], g_buf[2], n);
				ins += instret() - c0;
				left -= n;
			}
			if (k == 0) frames0 += r->frames();
			break;
		}
		}
	}
	puts_(c->name()); puts_(" "); dec(ins); puts_(" insns, frames "); dec(frames0); puts_(", per frame "); dec(frames0 ? ins / frames0 : 0); puts_("\n");
	return 0;
}
