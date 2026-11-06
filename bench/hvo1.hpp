// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvo1.hpp -- read an HVO1 trace (../docs/TRACE-FORMAT.md). Freestanding-friendly.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace hvo1 {
enum : uint32_t { MAGIC = 0x314F5648u, VERSION = 1u };
enum Op : uint32_t { REG = 1, PAN = 2, SENDS = 3, RESET = 4, RUN = 5, GAIN = 6 };
struct Rec {
	uint32_t w;
	Op op() const { return Op(w & 15); }
	uint32_t chip() const { return (w >> 4) & 3; }
	uint32_t addr() const { return (w >> 6) & 0x1FF; }   // REG: register; PAN/SENDS/GAIN: channel 0..17
	uint32_t data() const { return (w >> 15) & 0xFF; }
	uint32_t data2() const { return (w >> 23) & 0xFF; }
	int8_t level() const { return int8_t(data()); }       // GAIN
	uint32_t frames() const { return (w >> 6) & 0xFFFF; } // RUN, at 48 kHz
};
struct Trace {
	const uint32_t *w = nullptr;
	size_t n = 0;
	bool open(const uint32_t *words, size_t count) {
		if (count < 4 || words[0] != MAGIC || words[1] != VERSION || words[2] != 48000) return false;
		w = words; n = count; return true;
	}
	uint32_t flags() const { return w[3]; }           // bit 0: soft pan in use; bit 1: per-voice gains in use
	const Rec *begin() const { return reinterpret_cast<const Rec *>(w + 4); }
	const Rec *end() const { return reinterpret_cast<const Rec *>(w + n); }
};
}  // namespace hvo1
