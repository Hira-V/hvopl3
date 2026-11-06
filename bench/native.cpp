// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// native -- a trace through hvopl3 at the chip's own rate (one tick a sample), to compare sample by
// sample with a chip-rate recording.
//
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "hvo1.hpp"
#include "hvopl3.hpp"
#include "hvopl3_test.hpp"
int main(int argc, char **argv) {
	if (argc < 3) return 1;
	std::vector<uint32_t> w; FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	for (uint32_t x; fread(&x, 4, 1, f) == 1;) w.push_back(x);
	fclose(f);
	hvo1::Trace t; if (!t.open(w.data(), w.size())) return 1;
	static hvopl3::Chip c[4];
	for (auto &x : c) hvopl3::init_chip_rate(x);
	const uint32_t seed = argc > 3 ? uint32_t(strtoul(argv[3], nullptr, 16)) : 0;   // tests: the noise's state
	const uint32_t nflags = argc > 5 ? uint32_t(strtoul(argv[5], nullptr, 0)) : 0;    // tests: render flags
	const uint32_t tick0 = argc > 4 ? uint32_t(strtoul(argv[4], nullptr, 0)) : 0;     // tests: where the chip's counter starts
	std::vector<int32_t> mix[4]; uint64_t pos[4] = {}; uint64_t q[4] = {};
	std::vector<int32_t> buf(2 * 200000), r(2 * 200000), s(2 * 200000);
	constexpr uint64_t HZ = 14318180, DEN = 288 * 48000ull;
	for (const hvo1::Rec *p = t.begin(); p != t.end(); ++p) {
		const uint32_t k = p->chip();
		switch (p->op()) {
		case hvo1::RESET: hvopl3::init_chip_rate(c[k], nflags); if (seed) c[k].noise = seed; c[k].tick += tick0; break;
		case hvo1::REG: hvopl3::write(c[k], p->addr(), uint8_t(p->data())); break;
		case hvo1::RUN: {
			q[k] += p->frames();
			const uint64_t until = q[k] * HZ / DEN;
			const uint32_t n = uint32_t(until - pos[k]);
			const hvopl3::Output o = {buf.data(), nullptr, nullptr, false};
			hvopl3::render(c[k], o, n);
			if (mix[0].size() < 2 * (pos[k] + n)) mix[0].resize(2 * (pos[k] + n));
			for (uint32_t j = 0; j < 2 * n; ++j) mix[0][2 * pos[k] + j] += buf[j];
			pos[k] = until;
			break;
		}
		default: break;
		}
	}
	FILE *o = fopen(argv[2], "wb");
	fwrite(mix[0].data(), 4, mix[0].size(), o);
	fclose(o);
}
