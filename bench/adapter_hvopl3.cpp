// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// adapter_hvopl3.cpp -- hvopl3 behind the bench's interface.
//
#include "chip.hpp"
#include "hvopl3.hpp"
#include <string.h>
namespace {
struct Hv final : BenchChip {
	hvopl3::Chip c[4];
	uint32_t hz;
	uint32_t fl;
	explicit Hv(uint32_t rate, uint32_t flags) : hz(rate), fl(flags) { for (auto &x : c) hvopl3::init(x, hvopl3::Rate(hz), fl); }
	const char *name() const override { return hz == 96000 ? "hvopl96" : "hvopl"; }
	uint32_t rate() const override { return hz; }
	void reset(uint32_t k) override { hvopl3::init(c[k], hvopl3::Rate(hz), fl); }
	void write(uint32_t k, uint32_t a, uint8_t v) override { hvopl3::write(c[k], a, v); }
	void pan(uint32_t k, uint32_t ch, uint8_t p) override { hvopl3::set_pan(c[k], ch, p); }
	void sends(uint32_t k, uint32_t ch, uint8_t r, uint8_t s) override { hvopl3::set_sends(c[k], ch, r, s); }
	void gain(uint32_t k, uint32_t ch, int8_t l) override { hvopl3::set_gain(c[k], ch, l); }
	bool has_buses() const override { return true; }
	void render(uint32_t k, int32_t *dry, int32_t *rev, int32_t *cho, uint32_t n) override {
#ifndef __riscv   // the caller owns clearing its buses; the A25 count (which reads nothing) leaves it out
		memset(rev, 0, n * 8);
		memset(cho, 0, n * 8);
#endif
		const hvopl3::Output o = {dry, rev, cho, false};
		hvopl3::render(c[k], o, n);
	}
};
}
BenchChip *make_hvopl3(uint32_t rate, uint32_t flags) { return new Hv(rate, flags); }
