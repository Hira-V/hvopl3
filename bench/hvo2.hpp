// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvo2.hpp -- read an HVO2 trace (../docs/TRACE-FORMAT.md). Freestanding-friendly. An HVO1 file (the
// retired format) is refused by its magic, with a message naming the converter.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace hvo2 {
enum : uint32_t { MAGIC = 0x324F5648u, VERSION = 1u, MAX_CHIPS = 32 };
enum : uint32_t { HVO1_MAGIC = 0x314F5648u };   // the retired format's: refused
enum Op : uint32_t { REG = 1, PAN = 2, SENDS = 3, RESET = 4, RUN = 5, GAIN = 6 };
struct Rec {
	uint32_t w;
	Op op() const { return Op(w & 15); }
	uint32_t chip() const { return ((w >> 4) & 3) | (w >> 29) << 2; }   // 0..31
	uint32_t reg() const { return (w >> 6) & 0x1FF; }      // REG: the register
	uint32_t channel() const { return (w >> 6) & 0x1F; }   // PAN/SENDS/GAIN: the channel, 0..17
	uint32_t data() const { return (w >> 15) & 0xFF; }     // REG: the value; PAN: the pan; SENDS: the reverb send
	uint32_t chorus() const { return ((w >> 11) & 15) | ((w >> 23) & 7) << 4; }   // SENDS
	int8_t level() const { return int8_t(data()); }        // GAIN
	// RUN: the frames at a replay rate of scale x 48 kHz (bit 22: counted at the replay's rate already)
	uint32_t frames(uint32_t scale) const { return ((w >> 6) & 0xFFFF) * ((w >> 22 & 1) ? 1 : scale); }
};
struct Trace {
	const uint32_t *w = nullptr;
	size_t n = 0;
	uint32_t nchips = 0;          // the highest chip any record names, plus one
	const char *error = nullptr;  // why open() refused
	bool open(const uint32_t *words, size_t count) {
		w = nullptr; n = 0; nchips = 0; error = nullptr;
		if (count >= 1 && words[0] == HVO1_MAGIC) { error = "an HVO1 trace: that format is retired; convert it with tools/hvo1to2.py"; return false; }
		if (count < 4 || words[0] != MAGIC) { error = "not an HVO2 trace"; return false; }
		if (words[1] != VERSION || words[2] != 48000) { error = "an HVO2 trace of an unknown version or rate"; return false; }
		w = words; n = count;
		for (const Rec *r = begin(); r != end(); ++r) if (r->chip() >= nchips) nchips = r->chip() + 1;
		return true;
	}
	uint32_t flags() const { return w[3]; }           // bit 0: soft pan in use; bit 1: per-voice gains in use
	const Rec *begin() const { return reinterpret_cast<const Rec *>(n ? w + 4 : w); }
	const Rec *end() const { return reinterpret_cast<const Rec *>(w + n); }
};
}  // namespace hvo2
