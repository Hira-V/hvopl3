// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// digest: a trace through hvopl3 (chip mode), a digest of everything it makes. Same source on the host
// (the trace from argv) and bare metal under qemu (linked in): the digests must be identical.
//
#include <stdint.h>
#include <stddef.h>
#include "../hvo1.hpp"
#include "hvopl3.hpp"
#include "hvopl3_test.hpp"
#ifdef HV_BARE
extern "C" void *memset(void *d, int v, size_t n) { auto *p = static_cast<volatile uint8_t *>(d); while (n--) *p++ = uint8_t(v); return d; }
#ifdef HV_RV32EMU   // the repo's rv32emu (a25/digest.sh): its UART, and main returns through crt0's tohost
static void putc_(char c) { while (*(volatile uint32_t *)0xe8002004u) {} *(volatile uint32_t *)0xe8002000u = uint8_t(c); }
#else
#define UART ((volatile uint8_t *)0x10000000)
static void putc_(char c) { *UART = uint8_t(c); }
#endif
extern "C" const uint32_t trace_bin[], trace_end[];
#else
#include <cstdio>
#include <vector>
static void putc_(char c) { putchar(c); }
#endif
static void puts_(const char *s) { for (; *s; ++s) putc_(*s); }
static void hex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static hvopl3::Chip chips[4];
static int32_t D[2 * 1024], R[2 * 1024], C[2 * 1024];
#ifdef HV_COUNT
static inline uint64_t instret() {
#if __riscv_xlen == 64
	uint64_t v; asm volatile("csrr %0, minstret" : "=r"(v)); return v;
#else
	uint32_t hi, lo, h2;
	do { asm volatile("csrr %0, minstreth" : "=r"(hi)); asm volatile("csrr %0, minstret" : "=r"(lo)); asm volatile("csrr %0, minstreth" : "=r"(h2)); } while (hi != h2);
	return uint64_t(hi) << 32 | lo;
#endif
}
static uint64_t g_ins, g_frames;
static uint32_t div_u64(uint64_t a, uint64_t d) { uint32_t q = 0; for (int i = 31; i >= 0; --i) if ((d << i) <= a) { a -= d << i; q |= 1u << i; } return q; }
static void dec(uint32_t v) { char b[12]; int n = 0; do { b[n++] = char('0' + v % 10); v /= 10; } while (v); while (n) putc_(b[--n]); }
#endif
static inline uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
// add: the dry output added to (a pattern already there), as play() does it.
static void run(const hvo1::Trace &t, hvopl3::Rate rate, uint32_t flags, const char *label, bool add = false) {
	const uint32_t scale = rate / 48000;
	for (auto &x : chips) hvopl3::init(x, rate, flags);
	uint32_t hd = 2166136261u, hb = 2166136261u;
	for (const hvo1::Rec *r = t.begin(); r != t.end(); ++r) {
		hvopl3::Chip &c = chips[r->chip()];
		switch (r->op()) {
		case hvo1::RESET: hvopl3::init(c, rate, flags); break;
		case hvo1::REG: hvopl3::write(c, r->addr(), uint8_t(r->data())); break;
		case hvo1::PAN: hvopl3::set_pan(c, r->addr(), uint8_t(r->data())); break;
		case hvo1::SENDS: hvopl3::set_sends(c, r->addr(), uint8_t(r->data()), uint8_t(r->data2())); break;
		case hvo1::GAIN: hvopl3::set_gain(c, r->addr(), r->level()); break;
		case hvo1::RUN:
			for (uint32_t left = r->frames() * scale; left;) {
				const uint32_t n = left < 1024 ? left : 1024;
				for (uint32_t i = 0; i < 2 * n; ++i) R[i] = C[i] = 0;
				if (add) for (uint32_t i = 0; i < 2 * n; ++i) D[i] = int32_t(i * 2654435761u) >> 17;
				const hvopl3::Output o = {D, R, C, add};
#ifdef HV_COUNT
				const uint64_t c0 = instret();
				hvopl3::render(c, o, n);
				g_ins += instret() - c0;
				if (r->chip() == 0) g_frames += n;
#else
				hvopl3::render(c, o, n);
#endif
				for (uint32_t i = 0; i < 2 * n; ++i) { hd = mix(hd, uint32_t(D[i])); hb = mix(mix(hb, uint32_t(R[i])), uint32_t(C[i])); }
				left -= n;
			}
			break;
		}
	}
	puts_(label); putc_(' '); hex(hd); putc_(' '); hex(hb); putc_('\n');
}
int main(int argc, char **argv) {
	hvo1::Trace t;
#ifdef HV_BARE
	(void)argc; (void)argv;
	if (!t.open(trace_bin, size_t(trace_end - trace_bin))) { puts_("bad trace\n"); return 1; }
#else
	std::vector<uint32_t> w; FILE *f = fopen(argv[argc - 1], "rb"); if (!f) return 1;
	for (uint32_t x; fread(&x, 4, 1, f) == 1;) w.push_back(x);
	fclose(f);
	if (!t.open(w.data(), w.size())) return 1;
#endif
#ifndef HV_FLAGS
#define HV_FLAGS 0
#endif
	run(t, hvopl3::RATE_48K, HV_FLAGS, "48k");
#ifdef HV_COUNT
	puts_("insns/frame "); dec(div_u64(g_ins, g_frames)); putc_('\n');
#else
	run(t, hvopl3::RATE_96K, 0, "96k");
	run(t, hvopl3::RATE_48K, 7, "hq");
#ifdef HV_HQ   // run.sh HQ=1: each high-quality flag alone and together, unclipped too (as the consumer sets them)
	static const uint8_t HQF[] = {1, 2, 4, 3, 5, 6, 7, 17, 18, 20, 23};
	for (uint8_t f : HQF) { char l[8] = {'h', 'q', char('0' + f / 10), char('0' + f % 10), 0}; run(t, hvopl3::RATE_48K, f, l); }
	run(t, hvopl3::RATE_96K, 7, "96hq07");
	run(t, hvopl3::RATE_96K, 23, "96hq23");
	run(t, hvopl3::RATE_48K, 23, "hq23add", true);
#endif
#ifdef HV_MORE   // a25/digest.sh: every way the mix can write its output
	run(t, hvopl3::RATE_48K, 0, "add", true);
	run(t, hvopl3::RATE_48K, hvopl3::NO_CLIP, "noclip");
	run(t, hvopl3::RATE_48K, hvopl3::NO_CLIP, "noclip+add", true);
	run(t, hvopl3::RATE_48K, hvopl3::MIX_SLOW, "slow");   // the mix's slow path: the 48k run's bits
#endif
#endif
#if defined(HV_BARE) && !defined(HV_RV32EMU)
	*(volatile uint32_t *)0x100000 = 0x5555;   // qemu virt: power off
	for (;;) {}
#endif
	return 0;
}
