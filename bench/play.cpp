// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// play -- replay an HVO2 trace (up to 4 chips) into a chip and write what it makes.
//   play TRACE.hvo2 CHIP OUTPREFIX       CHIP: hvopl | hvopl96 | hvoplhq | hvopl96hq | another linked in
// Writes OUTPREFIX.dry.wav (int32 stereo; plus .rev/.cho when the chip has buses) at the chip's
// rate, and prints the time spent in render() as a share of real time. On the A25 build
// (-DHV_BARE, a25/run.sh) the trace is linked in and minstret is counted around render() instead.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include "hvo2.hpp"
#include "chip.hpp"

__attribute__((weak)) BenchChip *make_extra(const char *) { return nullptr; }

static void wav(const std::string &path, const std::vector<int32_t> &v, uint32_t rate) {
	FILE *f = std::fopen(path.c_str(), "wb");
	const uint32_t bytes = uint32_t(v.size() * 4);
	auto u32 = [&](uint32_t x) { std::fwrite(&x, 4, 1, f); }; auto u16 = [&](uint16_t x) { std::fwrite(&x, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2);
	u32(rate); u32(rate * 8); u16(8); u16(32); std::fwrite("data", 1, 4, f); u32(bytes);
	std::fwrite(v.data(), 4, v.size(), f); std::fclose(f);
}

int main(int argc, char **argv) {
	if (argc < 4) { std::fprintf(stderr, "play TRACE.hvo2 CHIP OUTPREFIX\n"); return 1; }
	std::vector<uint32_t> words;
	FILE *f = std::fopen(argv[1], "rb");
	if (!f) { std::fprintf(stderr, "play: cannot open %s\n", argv[1]); return 1; }
	for (uint32_t w; std::fread(&w, 4, 1, f) == 1;) words.push_back(w);
	std::fclose(f);
	hvo2::Trace t;
	if (!t.open(words.data(), words.size())) { std::fprintf(stderr, "play: %s: %s\n", argv[1], t.error); return 1; }
	if (t.nchips > 4) { std::fprintf(stderr, "play: %s: %u chips; the bench replays up to 4\n", argv[1], t.nchips); return 1; }
	const std::string which = argv[2];
	BenchChip *c = which == "hvopl" ? make_hvopl3(48000) : which == "hvopl96" ? make_hvopl3(96000) :
	               which == "hvoplhq" ? make_hvopl3(48000, 7) : which == "hvopl96hq" ? make_hvopl3(96000, 7) :
	               which == "hvoplphase" ? make_hvopl3(48000, 1) : which == "hvoplenv" ? make_hvopl3(48000, 2) : which == "hvoplmod" ? make_hvopl3(48000, 4) : make_extra(argv[2]);
	if (!c) { std::fprintf(stderr, "play: no chip %s\n", argv[2]); return 1; }
	const uint32_t scale = c->rate() / 48000;
	std::vector<int32_t> mix[3];
	std::vector<int32_t> buf[3];
	for (auto &b : buf) b.resize(2 * 65536 * scale);
	uint64_t cur[4] = {};
	double spent = 0;
	for (const hvo2::Rec *r = t.begin(); r != t.end(); ++r) {
		const uint32_t k = r->chip();
		switch (r->op()) {
		case hvo2::RESET: c->reset(k); break;
		case hvo2::REG: c->write(k, r->reg(), uint8_t(r->data())); break;
		case hvo2::PAN: c->pan(k, r->channel(), uint8_t(r->data())); break;
		case hvo2::SENDS: c->sends(k, r->channel(), uint8_t(r->data()), uint8_t(r->chorus())); break;
		case hvo2::GAIN: c->gain(k, r->channel(), r->level()); break;
		case hvo2::RUN: {
			const uint32_t n = r->frames(scale);
			const auto t0 = std::chrono::steady_clock::now();
			c->render(k, buf[0].data(), buf[1].data(), buf[2].data(), n);
			spent += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			for (int b = 0; b < (c->has_buses() ? 3 : 1); ++b) {
				if (mix[b].size() < 2 * (cur[k] + n)) mix[b].resize(2 * (cur[k] + n));
				for (uint32_t j = 0; j < 2 * n; ++j) mix[b][2 * cur[k] + j] += buf[b][j];
			}
			cur[k] += n;
			break;
		}
		}
	}
	const double secs = double(cur[0]) / c->rate();
	std::printf("play: %s, %s: %.2f s of sound, render %.3f s = %.3f%% of real time\n", argv[1], c->name(), secs, spent, 100 * spent / secs);
	const char *bus[3] = {"dry", "rev", "cho"};
	for (int b = 0; b < (c->has_buses() ? 3 : 1); ++b) wav(std::string(argv[3]) + "." + bus[b] + ".wav", mix[b], c->rate());
	delete c;
	return 0;
}
