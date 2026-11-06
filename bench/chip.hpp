// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// chip.hpp -- what the bench drives: any OPL3 behind this interface. One object = up to 4 chips (a
// trace has 4).
//
#pragma once
#include <stdint.h>
#include <stddef.h>

struct BenchChip {
	virtual ~BenchChip() {}
	virtual const char *name() const = 0;
	virtual uint32_t rate() const { return 48000; }          // its output rate: 48000 or 96000 (frames scale)
	virtual void reset(uint32_t chip) = 0;
	virtual void write(uint32_t chip, uint32_t reg, uint8_t v) = 0;
	virtual void pan(uint32_t, uint32_t /*channel 0..17*/, uint8_t /*0..127*/) {}
	virtual void sends(uint32_t, uint32_t, uint8_t /*rev*/, uint8_t /*cho*/) {}
	virtual void gain(uint32_t, uint32_t, int8_t /*0.75 dB steps*/) {}
	virtual bool has_buses() const { return false; }
	// n frames at rate(): written (not added) into dry, and rev/cho if has_buses(); stereo interleaved
	virtual void render(uint32_t chip, int32_t *dry, int32_t *rev, int32_t *cho, uint32_t n) = 0;
};

BenchChip *make_hvopl3(uint32_t rate, uint32_t flags = 0);   // hvopl3 (adapter_hvopl3.cpp)
// More chips, linked in beside the bench (Makefile EXTRA_SRC=): by name, nullptr if none of them.
BenchChip *make_extra(const char *name);
