// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// mid2wav -- a Standard MIDI File through the driver (drv/) to a WAV, for listening. Not part of the library.
//   mid2wav SONG.mid OUT.wav [options]
//     --bank FILE.wopl    the bank (default drv/banks/DMXOPL3-GS.wopl, from the repository root)
//     --levels FILE       level corrections from a C source holding a kGmLevels[128] table and
//                         {kit, {128 values}} kit tables (Hira-V's gm_levels.inc has that form)
//     --chips N           1..32 (default 2)      --96k     the chips at 96 kHz (the WAV too)
//     --hq                every high-quality flag  --hard-pan  the C0 bits      --linear  the linear curve
//     --no-corrections    --no-restrike           --stream  through the HVO2 stream and hvopl3::play
//     --buses             also OUT.rev.wav and OUT.cho.wav (the send buses)
//     --dump FILE         the timed messages for bench/a25/drv.sh (EVENTS=FILE), 8 seconds of them
//     --dump-from SEC     ... from this second on (the controllers and programs before it at the start)
// Writes 16-bit stereo (clipped) and prints the time spent and the most voices in use.
//
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "hvopl3.hpp"
#include "hvopl3_drv.hpp"
#include "../tests/song.hpp"

using namespace hvopl3;

static std::vector<uint8_t> slurp(const char *path) {
	std::vector<uint8_t> v;
	FILE *f = std::fopen(path, "rb");
	if (!f) return v;
	for (int c; (c = std::fgetc(f)) != EOF;) v.push_back(uint8_t(c));
	std::fclose(f);
	return v;
}

// ---- a Standard MIDI File, all tracks merged, in frames at `rate` ----
struct Msg { double sec; uint32_t order; std::vector<uint8_t> m; };
struct Tempo { uint64_t tick; uint32_t usq; };
static bool read_smf(const std::vector<uint8_t> &f, std::vector<Msg> &out) {
	size_t p = 0;
	const auto u32 = [&](size_t at) { return uint32_t(f[at]) << 24 | f[at + 1] << 16 | f[at + 2] << 8 | f[at + 3]; };
	if (f.size() < 14 || memcmp(f.data(), "MThd", 4)) return false;
	const uint32_t hl = u32(4);
	const uint32_t ntr = uint32_t(f[10]) << 8 | f[11];
	const uint16_t div = uint16_t(f[12] << 8 | f[13]);
	p = 8 + hl;
	struct Raw { uint64_t tick; uint32_t order; std::vector<uint8_t> m; };
	std::vector<Raw> raw;
	std::vector<Tempo> tempo;
	uint32_t order = 0;
	for (uint32_t t = 0; t < ntr && p + 8 <= f.size(); ++t) {
		if (memcmp(&f[p], "MTrk", 4)) { p += 8 + u32(p + 4); --t; continue; }
		const size_t end = std::min(f.size(), p + 8 + u32(p + 4));
		size_t q = p + 8;
		uint64_t tick = 0;
		uint8_t run = 0;
		const auto var = [&]() { uint32_t v = 0; for (int i = 0; i < 4 && q < end; ++i) { const uint8_t b = f[q++]; v = v << 7 | (b & 127); if (!(b & 128)) break; } return v; };
		while (q < end) {
			tick += var();
			if (q >= end) break;
			uint8_t s = f[q];
			if (s & 0x80) ++q; else s = run;
			if (s == 0xFF) {
				if (q >= end) break;
				const uint8_t type = f[q++];
				const uint32_t len = var();
				if (type == 0x51 && len == 3 && q + 3 <= end) tempo.push_back({tick, uint32_t(f[q]) << 16 | f[q + 1] << 8 | f[q + 2]});
				q += len;
				if (type == 0x2F) break;
			} else if (s == 0xF0 || s == 0xF7) {
				const uint32_t len = var();
				if (s == 0xF0 && q + len <= end) {
					Raw r{tick, order++, {0xF0}};
					r.m.insert(r.m.end(), f.begin() + long(q), f.begin() + long(q + len));
					if (r.m.back() != 0xF7) r.m.push_back(0xF7);
					raw.push_back(r);
				}
				q += len;
			} else if (s >= 0x80) {
				run = s;
				const uint32_t n = (s & 0xE0) == 0xC0 ? 1 : 2;
				if (q + n > end) break;
				Raw r{tick, order++, {s, f[q]}};
				if (n == 2) r.m.push_back(f[q + 1]);
				q += n;
				raw.push_back(r);
			} else {
				break;   // data with no running status: the track is damaged
			}
		}
		p = end;
	}
	std::stable_sort(raw.begin(), raw.end(), [](const Raw &a, const Raw &b) { return a.tick < b.tick; });
	std::stable_sort(tempo.begin(), tempo.end(), [](const Tempo &a, const Tempo &b) { return a.tick < b.tick; });
	// ticks to seconds through the tempo map (or SMPTE frames)
	const bool smpte = div & 0x8000;
	const double tps_smpte = smpte ? double(-int8_t(div >> 8)) * (div & 0xFF) : 0;
	size_t ti = 0;
	uint64_t t0 = 0;
	double s0 = 0, usq = 500000;
	for (const Raw &r : raw) {
		double sec;
		if (smpte) sec = r.tick / tps_smpte;
		else {
			while (ti < tempo.size() && tempo[ti].tick <= r.tick) {
				s0 += double(tempo[ti].tick - t0) * usq / 1e6 / div;
				t0 = tempo[ti].tick; usq = tempo[ti].usq; ++ti;
			}
			sec = s0 + double(r.tick - t0) * usq / 1e6 / div;
		}
		out.push_back({sec, r.order, r.m});
	}
	return true;
}

// Level corrections from C source: the 128 numbers after "kGmLevels", then each "{kit, {128}}" after "kGmKits".
static bool read_levels(const std::vector<uint8_t> &src, int8_t *prog, std::vector<drv::KitLevels> &kits) {
	std::string s(src.begin(), src.end());
	const auto numbers = [&](size_t from, size_t count, std::vector<int> &v) {
		size_t p = from;
		while (v.size() < count && p < s.size()) {
			while (p < s.size() && !(isdigit(uint8_t(s[p])) || (s[p] == '-' && p + 1 < s.size() && isdigit(uint8_t(s[p + 1]))))) {
				if (s[p] == '/' && p + 1 < s.size() && s[p + 1] == '/') { while (p < s.size() && s[p] != '\n') ++p; }
				else ++p;
			}
			if (p >= s.size()) break;
			char *e;
			v.push_back(int(strtol(&s[p], &e, 10)));
			p = size_t(e - s.data());
		}
		return p;
	};
	size_t a = s.find("kGmLevels");
	if (a == std::string::npos) return false;
	a = s.find('{', a);
	std::vector<int> v;
	numbers(a, 128, v);
	if (v.size() != 128) return false;
	for (int i = 0; i < 128; ++i) prog[i] = int8_t(v[i]);
	size_t k = s.find("kGmKits");
	if (k == std::string::npos) return true;
	k = s.find('{', k);
	for (;;) {
		k = s.find('{', k + 1);
		if (k == std::string::npos) break;
		std::vector<int> w;
		const size_t e = numbers(k, 129, w);
		if (w.size() != 129) break;
		drv::KitLevels kl;
		kl.kit = uint8_t(w[0]);
		for (int i = 0; i < 128; ++i) kl.levels[i] = int8_t(w[1 + i]);
		kits.push_back(kl);
		k = s.find('}', e);
		if (k == std::string::npos) break;
		k = s.find('}', k + 1);   // past the kit's closing brace
		if (k == std::string::npos) break;
	}
	return true;
}

static void wav16(const std::string &path, const std::vector<int32_t> &v, uint32_t rate) {
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) { std::fprintf(stderr, "mid2wav: cannot write %s\n", path.c_str()); return; }
	const uint32_t bytes = uint32_t(v.size() * 2);
	auto u32 = [&](uint32_t x) { std::fwrite(&x, 4, 1, f); }; auto u16 = [&](uint16_t x) { std::fwrite(&x, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(2);
	u32(rate); u32(rate * 4); u16(4); u16(16); std::fwrite("data", 1, 4, f); u32(bytes);
	for (int32_t x : v) { const int16_t s = int16_t(x > 32767 ? 32767 : x < -32768 ? -32768 : x); std::fwrite(&s, 2, 1, f); }
	std::fclose(f);
}

int main(int argc, char **argv) {
	if (argc < 3) { std::fprintf(stderr, "mid2wav SONG.mid OUT.wav [--bank F.wopl] [--levels F] [--chips N] [--96k] [--hq] [--hard-pan] [--linear] [--no-corrections] [--no-restrike] [--stream] [--buses] [--dump F]\n"); return 1; }
	const char *bankp = "drv/banks/DMXOPL3-GS.wopl", *levelsp = nullptr, *dump = nullptr;
	double dump_from = 0;
	drv::Config cfg = drv::default_config();
	cfg.chips = 2;
	bool buses = false;
	for (int i = 3; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--bank" && i + 1 < argc) bankp = argv[++i];
		else if (a == "--levels" && i + 1 < argc) levelsp = argv[++i];
		else if (a == "--chips" && i + 1 < argc) cfg.chips = uint32_t(atoi(argv[++i]));
		else if (a == "--dump" && i + 1 < argc) dump = argv[++i];
		else if (a == "--dump-from" && i + 1 < argc) dump_from = atof(argv[++i]);
		else if (a == "--96k") cfg.rate = RATE_96K;
		else if (a == "--hq") cfg.chip_flags |= HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION;
		else if (a == "--hard-pan") cfg.hard_pan = true;
		else if (a == "--linear") cfg.curve = drv::CURVE_LINEAR;
		else if (a == "--no-corrections") cfg.level_corrections = false;
		else if (a == "--no-restrike") cfg.restrike = false;
		else if (a == "--stream") cfg.stream = true;
		else if (a == "--buses") buses = true;
		else { std::fprintf(stderr, "mid2wav: %s?\n", a.c_str()); return 1; }
	}
	const std::vector<uint8_t> smf = slurp(argv[1]), bank = slurp(bankp);
	std::vector<Msg> msgs;
	if (!read_smf(smf, msgs)) { std::fprintf(stderr, "mid2wav: %s is not a Standard MIDI File\n", argv[1]); return 1; }
	static std::vector<uint8_t> poolmem(drv::pool_bytes(64));
	drv::Pool *pool = drv::pool_init(poolmem.data(), poolmem.size());
	const int lr = drv::load_wopl(*pool, bank.data(), bank.size());
	if (lr != drv::LOAD_OK) { std::fprintf(stderr, "mid2wav: %s: load_wopl %d\n", bankp, lr); return 1; }
	if (levelsp) {
		int8_t prog[128];
		std::vector<drv::KitLevels> kits;
		if (!read_levels(slurp(levelsp), prog, kits)) { std::fprintf(stderr, "mid2wav: no kGmLevels in %s\n", levelsp); return 1; }
		drv::set_levels(*pool, prog, kits.data(), uint32_t(kits.size()));
		std::printf("mid2wav: level corrections for 128 programs and %zu kits\n", kits.size());
	}
	const uint32_t rate = cfg.rate;
	if (dump) {   // 8 seconds of timed messages for the A25 count (bench/a25/drv.sh EVENTS=)
		std::vector<song::Ev> ev;
		for (const Msg &m : msgs) {
			const bool note = (m.m[0] & 0xE0) == 0x80;
			if (m.sec < dump_from && note) continue;
			const uint32_t at = m.sec < dump_from ? 0 : uint32_t((m.sec - dump_from) * 48000 + 0.5);
			if (at >= 8 * 48000 || m.m.size() > 11) continue;
			song::Ev e{at, uint8_t(m.m.size()), {}};
			memcpy(e.m, m.m.data(), m.m.size());
			ev.push_back(e);
		}
		FILE *f = std::fopen(dump, "wb");
		if (f) { std::fwrite(ev.data(), sizeof(song::Ev), ev.size(), f); std::fclose(f); }
		std::printf("mid2wav: %zu messages to %s\n", ev.size(), dump);
	}
	static drv::Driver d;
	static Chip chips[drv::MAX_CHIPS], replay[drv::MAX_CHIPS];
	drv::init(d, *pool, chips, cfg);
	const double length = (msgs.empty() ? 0 : msgs.back().sec) + 2.0;
	const uint32_t frames = uint32_t(length * rate);
	const uint32_t B = rate / 100;   // 10 ms blocks
	std::vector<int32_t> out[3];
	for (auto &o : out) o.assign(size_t(2) * frames + 2 * B, 0);
	std::vector<uint32_t> recs(drv::stream_reserve(d) * 4 + 4096);
	size_t e = 0;
	uint32_t most = 0;
	const auto t0 = std::chrono::steady_clock::now();
	for (uint32_t t = 0; t < frames;) {
		const uint32_t n = std::min(B, frames - t);
		while (e < msgs.size() && uint32_t(msgs[e].sec * rate) < t + n) {
			const uint32_t at = uint32_t(msgs[e].sec * rate);
			if (!drv::send(d, at > t ? at - t : 0, msgs[e].m.data(), uint32_t(msgs[e].m.size()))) break;
			++e;
		}
		const Output o = {&out[0][2 * size_t(t)], &out[1][2 * size_t(t)], &out[2][2 * size_t(t)], true};
		uint32_t done = n;
		if (!cfg.stream) drv::render(d, o, n);
		else {
			done = 0;
			while (done < n) {
				uint32_t cnt = 0;
				const uint32_t m = drv::render_stream(d, recs.data(), uint32_t(recs.size()), cnt, n - done);
				const Output p = {o.dry + 2 * done, o.rev + 2 * done, o.cho + 2 * done, true};
				play(replay, cfg.chips, recs.data(), cnt, p, cfg.rate, drv::chip_flags(d));
				done += m;
				if (!m && !cnt) break;
			}
		}
		uint32_t busy = 0;
		for (uint32_t k = 0; k < d.cfg.chips; ++k) busy += d.busy[k];
		most = std::max(most, busy);
		t += done;
	}
	const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	out[0].resize(size_t(2) * frames); out[1].resize(size_t(2) * frames); out[2].resize(size_t(2) * frames);
	const std::string base = argv[2];
	wav16(base, out[0], rate);
	if (buses) {
		const std::string stem = base.size() > 4 && base.substr(base.size() - 4) == ".wav" ? base.substr(0, base.size() - 4) : base;
		wav16(stem + ".rev.wav", out[1], rate);
		wav16(stem + ".cho.wav", out[2], rate);
	}
	int32_t peak = 0; uint32_t clipped = 0;
	for (int32_t x : out[0]) { peak = std::max(peak, std::abs(x)); clipped += x > 32767 || x < -32768; }
	std::printf("mid2wav: %s, %zu messages, %.1f s, %u chips at %u Hz: %.3f s (%.2f%% of real time), at most %u of %u channels, peak %d, %u samples clipped\n",
	            argv[1], msgs.size(), length, d.cfg.chips, rate, spent, 100 * spent / length, most, d.cfg.chips * 18, peak, clipped);
	return 0;
}
