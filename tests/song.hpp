// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// song.hpp -- the driver's test song: a synthetic General MIDI message stream, not a real song. Busy on
// purpose: 14 channels and two drum channels at 125 bpm, every patch kind of DMXOPL3 (2-op, pseudo-4-op,
// both of its 4-op melodic patches and a 4-op drum), pedals, bends, vibrato, portamento, sends, pan and
// expression sweeps, program changes, resets and master volume. Integer code only, no library: the same
// stream on every target. make() writes the events in time order (frames at 48 kHz).
//
#pragma once
#include <stdint.h>

namespace song {

struct Ev { uint32_t at; uint8_t len, m[11]; };

namespace detail {
struct Gen {
	Ev *out; uint32_t cap, n, end; uint32_t rng;
	uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
	void add(uint32_t at, uint8_t a, uint8_t b, uint8_t c, uint8_t len = 3) {
		if (n >= cap || at >= end) return;
		Ev &e = out[n++];
		e.at = at; e.len = len; e.m[0] = a; e.m[1] = b; e.m[2] = c;
	}
	void sx(uint32_t at, const uint8_t *m, uint8_t len) {
		if (n >= cap || at >= end) return;
		Ev &e = out[n++];
		e.at = at; e.len = len;
		for (uint8_t i = 0; i < len; ++i) e.m[i] = m[i];
	}
	void note(uint32_t at, uint32_t dur, uint8_t ch, uint8_t key, uint8_t vel) {
		add(at, uint8_t(0x90 | ch), key, vel);
		add(at + dur, uint8_t(0x80 | ch), key, 64);
	}
	void cc(uint32_t at, uint8_t ch, uint8_t c, uint8_t v) { add(at, uint8_t(0xB0 | ch), c, v); }
	void prog(uint32_t at, uint8_t ch, uint8_t msb, uint8_t p) { cc(at, ch, 0, msb); cc(at, ch, 32, 0); add(at, uint8_t(0xC0 | ch), p, 0, 2); }
};
// Stable merge sort by time: the order of events at one frame is the order they were made.
inline void sort(Ev *a, Ev *tmp, uint32_t n) {
	for (uint32_t w = 1; w < n; w *= 2) {
		for (uint32_t lo = 0; lo < n; lo += 2 * w) {
			const uint32_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
			uint32_t i = lo, j = mid, k = lo;
			while (i < mid && j < hi) tmp[k++] = a[j].at < a[i].at ? a[j++] : a[i++];
			while (i < mid) tmp[k++] = a[i++];
			while (j < hi) tmp[k++] = a[j++];
		}
		for (uint32_t i = 0; i < n; ++i) a[i] = tmp[i];
	}
}
}  // namespace detail

// The song's first `seconds` into `out` (`tmp`: as large, for the sort). Returns the events made.
inline uint32_t make(Ev *out, Ev *tmp, uint32_t cap, uint32_t seconds) {
	detail::Gen g = {out, cap, 0, seconds * 48000, 2026};
	const uint32_t S = 5760;   // a sixteenth at 125 bpm
	static const uint8_t ROOT[4] = {48, 45, 41, 43};   // C, Am, F, G
	static const uint8_t GM_ON[6] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
	g.sx(0, GM_ON, 6);
	const uint8_t mv0[8] = {0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 110, 0xF7};
	g.sx(0, mv0, 8);
	// the set-up: programs (ch 4: GS bank 8's 4-op piano; ch 5: bank 16's 4-op guitar; ch 10: GM2 drums, kit 16)
	static const uint8_t P[16] = {0, 33, 48, 80, 0, 30, 61, 89, 24, 0, 16, 11, 52, 73, 95, 4};
	static const uint8_t MSB[16] = {0, 0, 0, 0, 8, 16, 0, 0, 0, 0, 120, 0, 0, 0, 0, 0};
	for (uint8_t c = 0; c < 16; ++c) {
		g.prog(0, c, MSB[c], P[c]);
		g.cc(0, c, 7, uint8_t(90 + (c * 7) % 37));
		g.cc(0, c, 10, uint8_t((c * 37 + 20) % 128));
		g.cc(0, c, 91, uint8_t(20 + (c * 13) % 80));
		g.cc(0, c, 93, uint8_t((c * 29) % 60));
	}
	// RPN 0 = 12 on the lead; portamento on the bass
	g.cc(0, 3, 101, 0); g.cc(0, 3, 100, 0); g.cc(0, 3, 6, 12); g.cc(0, 3, 38, 0); g.cc(0, 3, 101, 127); g.cc(0, 3, 100, 127);
	g.cc(0, 1, 65, 127); g.cc(0, 1, 5, 6);
	const uint32_t bars = seconds * 48000 / (16 * S) + 1;
	for (uint32_t bar = 0; bar < bars; ++bar) {
		const uint32_t b0 = bar * 16 * S;
		const uint8_t r = ROOT[bar % 4];
		const bool minor = bar % 4 == 1;
		const uint8_t third = minor ? 3 : 4;
		// ch 0: piano chords on each beat, the sustain pedal through the bar
		g.cc(b0, 0, 64, 127);
		for (uint32_t beat = 0; beat < 4; ++beat) {
			const uint32_t t = b0 + beat * 4 * S;
			const uint8_t v = uint8_t(70 + g.rnd() % 50);
			g.note(t, 3 * S, 0, uint8_t(r + 12), v); g.note(t, 3 * S, 0, uint8_t(r + 12 + third), v);
			g.note(t, 3 * S, 0, uint8_t(r + 19), v); g.note(t, 3 * S, 0, uint8_t(r + 24), uint8_t(v - 10));
		}
		g.cc(b0 + 16 * S - 200, 0, 64, 0);
		// ch 1: bass eighths, legato (portamento)
		for (uint32_t e = 0; e < 8; ++e) g.note(b0 + e * 2 * S, 2 * S + 100, 1, uint8_t(r - 12 + (e % 3 == 2 ? 7 : e % 4 == 3 ? 12 : 0)), uint8_t(90 + g.rnd() % 30));
		// ch 2: strings, the whole bar, an expression swell and a pan sweep
		g.note(b0, 16 * S - 50, 2, uint8_t(r + 12), 80); g.note(b0, 16 * S - 50, 2, uint8_t(r + 16 - (minor ? 1 : 0)), 80); g.note(b0, 16 * S - 50, 2, uint8_t(r + 19), 80);
		for (uint32_t s = 0; s < 16; ++s) { g.cc(b0 + s * S, 2, 11, uint8_t(60 + s * 4)); g.cc(b0 + s * S + S / 2, 2, 10, uint8_t(s * 8)); }
		// ch 3: the lead: a line with bends and the mod wheel
		for (uint32_t s = 0; s < 16; s += 2) {
			const uint8_t k = uint8_t(r + 24 + (g.rnd() % 5) * 2);
			g.note(b0 + s * S, 2 * S - 300, 3, k, uint8_t(85 + g.rnd() % 40));
			g.add(b0 + s * S + S, 0xE3, 0, uint8_t(64 + (s % 4 ? 6 : -6)));
			g.add(b0 + s * S + 2 * S - 400, 0xE3, 0, 64);
			g.cc(b0 + s * S, 3, 1, uint8_t(s * 8));
		}
		// ch 4: the 4-op piano: arpeggio sixteenths
		for (uint32_t s = 0; s < 16; ++s) g.note(b0 + s * S, S - 200, 4, uint8_t(r + 24 + (s % 4 == 0 ? 0 : s % 4 == 1 ? third : s % 4 == 2 ? 7 : 12)), uint8_t(60 + g.rnd() % 40));
		// ch 5: the 4-op guitar: power chords on 2 and 4
		for (uint32_t beat = 1; beat < 4; beat += 2) { g.note(b0 + beat * 4 * S, 3 * S, 5, uint8_t(r), 110); g.note(b0 + beat * 4 * S, 3 * S, 5, uint8_t(r + 7), 105); }
		// ch 6: brass stabs, the same note struck again (re-strikes)
		for (uint32_t s = 2; s < 16; s += 3) g.note(b0 + s * S, S, 6, uint8_t(r + 12 + 7), uint8_t(70 + (s * 9) % 50));
		// ch 7: a pad held by sostenuto across the bar line
		g.note(b0, S, 7, uint8_t(r + 24), 70);
		g.cc(b0 + S / 2, 7, 66, 127);
		g.note(b0 + 4 * S, S, 7, uint8_t(r + 28), 60);   // not held
		g.cc(b0 + 15 * S, 7, 66, 0);
		// ch 8, 11..15: plucks, mallets and others, with pressure, junk NRPNs and program changes
		for (uint32_t s = 0; s < 16; ++s) {
			const uint8_t c = uint8_t(s % 6 == 0 ? 8 : 10 + s % 6);
			if (c == 10) continue;
			g.note(b0 + s * S + S / 3, S + (g.rnd() % 3) * S, c, uint8_t(r + 24 + (g.rnd() % 12)), uint8_t(40 + g.rnd() % 80));
			if (s % 4 == 1) g.add(b0 + s * S, uint8_t(0xD0 | c), uint8_t(g.rnd() % 128), 0, 2);
		}
		if (bar % 2 == 1) { g.cc(b0, 12, 99, 1); g.cc(b0, 12, 98, 8); g.cc(b0, 12, 6, 64); g.add(b0, 0xCD, uint8_t(73 + bar % 5), 0, 2); }
		// ch 9: drums, kit 0
		for (uint32_t s = 0; s < 16; ++s) {
			const uint32_t t = b0 + s * S;
			if (s % 8 == 0) g.note(t, S, 9, 36, 120);
			if (s % 8 == 4) g.note(t, S, 9, 38, 110);
			if (s % 2 == 0) g.note(t, S / 2, 9, uint8_t(s % 8 == 6 ? 46 : 42), uint8_t(70 + (s % 4) * 10));
			if (s == 0) g.note(t, 8 * S, 9, 49, 100);
		}
		// ch 10: a second drum channel (GM2, kit 16): fills on the last beat, its 4-op snare
		for (uint32_t s = 12; s < 16; ++s) g.note(b0 + s * S, S, 10, uint8_t(s % 2 ? 38 : 45 + (s - 12) * 2), 100);
		if (bar == 2) { const uint8_t mv[8] = {0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 127, 0xF7}; g.sx(b0, mv, 8); }
		if (bar == 3) g.cc(b0, 4, 121, 0);
	}
	// the end: every note off
	for (uint8_t c = 0; c < 16; ++c) { g.cc(seconds * 48000 - 4800, c, 64, 0); g.cc(seconds * 48000 - 4800, c, 123, 0); }
	detail::sort(out, tmp, g.n);
	return g.n;
}

}  // namespace song
