// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// drv_digest: the MIDI driver's test song (tests/song.hpp) with DMXOPL3, a digest of everything it makes,
// in several set-ups. Same source on the host (the bank from argv) and bare metal (linked in: vec/drv.sh
// under qemu, a25/drv.sh under rv32emu): the digests must be identical. The stream set-up replays its
// records with hvopl3::play and must give the 4-chip set-up's digest.
// HV_COUNT (the A25): instructions a 48 kHz frame in the driver alone and in the chips' render, instead.
//
#include <stdint.h>
#include <stddef.h>
#include "hvopl3.hpp"
#include "hvopl3_drv.hpp"
#include "../tests/song.hpp"

#ifdef HV_BARE
extern "C" void *memset(void *d, int v, size_t n) { auto *p = static_cast<volatile uint8_t *>(d); while (n--) *p++ = uint8_t(v); return d; }
extern "C" void *memcpy(void *d, const void *s, size_t n) { auto *p = static_cast<volatile uint8_t *>(d); auto *q = static_cast<const uint8_t *>(s); while (n--) *p++ = *q++; return d; }
#ifdef HV_RV32EMU
static void putc_(char c) { while (*(volatile uint32_t *)0xe8002004u) {} *(volatile uint32_t *)0xe8002000u = uint8_t(c); }
#else
static void putc_(char c) { *(volatile uint8_t *)0x10000000 = uint8_t(c); }
#endif
extern "C" const uint8_t wopl_bin[], wopl_end[];
#ifdef HV_EVENTS   // a25/drv.sh EVENTS=: a real song's messages (mid2wav --dump) in place of the test song
extern "C" const song::Ev events_bin[], events_end[];
#endif
#else
#include <cstdio>
#include <vector>
static void putc_(char c) { putchar(c); }
#endif
static void puts_(const char *s) { for (; *s; ++s) putc_(*s); }
static void hex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static void dec(uint32_t v) { char b[12]; int n = 0; do { b[n++] = char('0' + v % 10); v /= 10; } while (v); while (n) putc_(b[--n]); }

using namespace hvopl3;
enum : uint32_t { SECONDS = 8, BLOCK_ = 480, MAXEV = 6000 };
static song::Ev ev[MAXEV], tmp[MAXEV];
static uint32_t nev;
static Chip chips[drv::MAX_CHIPS], replay[drv::MAX_CHIPS];
static drv::Driver d;
alignas(16) static uint8_t poolmem[80000];
static drv::Pool *pool;
static int32_t D[2 * 2 * BLOCK_], R[2 * 2 * BLOCK_], C[2 * 2 * BLOCK_];
static uint32_t recs[16384];

#ifdef HV_COUNT
static inline uint64_t instret() {
	uint32_t hi, lo, h2;
	do { asm volatile("csrr %0, minstreth" : "=r"(hi)); asm volatile("csrr %0, minstret" : "=r"(lo)); asm volatile("csrr %0, minstreth" : "=r"(h2)); } while (hi != h2);
	return uint64_t(hi) << 32 | lo;
}
static uint64_t g_chip;
static uint32_t div_u64(uint64_t a, uint64_t d) { uint32_t q = 0; for (int i = 31; i >= 0; --i) if ((d << i) <= a) { a -= d << i; q |= 1u << i; } return q; }
// the driver's calls of the chips' render, counted apart (HVOPL3_DRV_CHIP_RENDER)
uint32_t bench_chip_render(Chip &c, const Output &o, uint32_t n) {
	const uint64_t t0 = instret();
	const uint32_t r = render(c, o, n);
	g_chip += instret() - t0;
	return r;
}
#endif

static inline uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static void run(const char *label, uint32_t nchips, Rate rate, uint32_t flags, bool stream, bool hard = false, uint8_t curve = 0) {
	drv::Config c = drv::default_config();
	c.chips = nchips; c.rate = rate; c.chip_flags = flags; c.stream = stream; c.hard_pan = hard; c.curve = curve;
	drv::init(d, *pool, chips, c);
	const uint32_t scale = rate / 48000, frames = SECONDS * 48000 * scale;
	uint32_t hd = 2166136261u, hb = 2166136261u, e = 0;
#ifdef HV_COUNT
	uint64_t total = 0;
	g_chip = 0;
#endif
	for (uint32_t t = 0; t < frames;) {
		const uint32_t n = BLOCK_ * scale < frames - t ? BLOCK_ * scale : frames - t;
		while (e < nev && ev[e].at * scale < t + n) { drv::send(d, ev[e].at * scale - t, ev[e].m, ev[e].len); ++e; }
		for (uint32_t i = 0; i < 2 * n; ++i) R[i] = C[i] = 0;
		uint32_t done = n;
		if (!stream) {
			const Output o = {D, R, C, false};
#ifdef HV_COUNT
			const uint64_t t0 = instret();
			drv::render(d, o, n);
			total += instret() - t0;
#else
			drv::render(d, o, n);
#endif
		} else {
			for (uint32_t i = 0; i < 2 * n; ++i) D[i] = 0;
			done = 0;
			while (done < n) {
				uint32_t cnt = 0;
				const uint32_t m = drv::render_stream(d, recs, sizeof recs / sizeof *recs, cnt, n - done);
				const Output o = {D + 2 * done, R + 2 * done, C + 2 * done, true};
				play(replay, nchips, recs, cnt, o, rate, drv::chip_flags(d));
				done += m;
				if (!m && !cnt) break;
			}
		}
		for (uint32_t i = 0; i < 2 * done; ++i) { hd = mix(hd, uint32_t(D[i])); hb = mix(mix(hb, uint32_t(R[i])), uint32_t(C[i])); }
		t += done;
	}
#ifdef HV_COUNT
	const uint32_t f48 = SECONDS * 48000;
	puts_(label); puts_(" driver "); dec(div_u64(total - g_chip, f48)); puts_(" chips "); dec(div_u64(g_chip, f48));
	puts_(" insns/frame, driver/chips "); dec(div_u64((total - g_chip) * 1000, g_chip ? g_chip : 1)); puts_(" per mille\n");
	(void)hd; (void)hb;
#else
	puts_(label); putc_(' '); hex(hd); putc_(' '); hex(hb); putc_('\n');
#endif
}

int main(int argc, char **argv) {
#ifdef HV_BARE
	(void)argc; (void)argv;
	const uint8_t *bank = wopl_bin;
	const size_t bank_n = size_t(wopl_end - wopl_bin);
#else
	std::vector<uint8_t> w;
	FILE *f = fopen(argc > 1 ? argv[1] : "drv/banks/DMXOPL3-GS.wopl", "rb");
	if (!f) { puts_("no bank\n"); return 1; }
	for (int ch; (ch = fgetc(f)) != EOF;) w.push_back(uint8_t(ch));
	fclose(f);
	const uint8_t *bank = w.data();
	const size_t bank_n = w.size();
#endif
	pool = drv::pool_init(poolmem, sizeof poolmem);
	if (!pool || drv::load_wopl(*pool, bank, bank_n) != drv::LOAD_OK) { puts_("bank?\n"); return 1; }
#ifdef HV_EVENTS
	nev = uint32_t(events_end - events_bin);
	if (nev > MAXEV) nev = MAXEV;
	for (uint32_t i = 0; i < nev; ++i) ev[i] = events_bin[i];
#else
	nev = song::make(ev, tmp, MAXEV, SECONDS);
#endif
#ifdef HV_COUNT
	run("1chip", 1, RATE_48K, 0, false);
	run("4chips", 4, RATE_48K, 0, false);
#else
	run("1chip", 1, RATE_48K, 0, false);
	run("4chips", 4, RATE_48K, 0, false);
	run("stream", 4, RATE_48K, 0, true);   // must be 4chips'
	run("96k", 2, RATE_96K, 0, false);
	run("hq", 2, RATE_48K, HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION, false);
	run("hard", 1, RATE_48K, 0, false, true, drv::CURVE_LINEAR);
	puts_("events "); dec(nev); putc_('\n');
#endif
	return 0;
}
