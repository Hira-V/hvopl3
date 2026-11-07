// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// t_ext: the extensions and the output contract, exactly. Host only.
//
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "hvopl3.hpp"
#include "hvopl3_test.hpp"
#include "../bench/hvo2.hpp"
using namespace hvopl3;
static int fails = 0, checks = 0;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++fails; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #x); printf(__VA_ARGS__); printf("\n"); } } while (0)
static constexpr uint32_t N = 2048;
static int32_t D[2 * N], R[2 * N], C[2 * N], D2[2 * N];
static const uint8_t OP1[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
static uint32_t chr(uint32_t ch, uint32_t base) { return (ch >= 9 ? 0x100 : 0) + base + ch % 9; }
static uint32_t slot(uint32_t ch, uint32_t n) { return (ch >= 9 ? 0x100 : 0) + OP1[ch % 9] + (n ? 3 : 0); }
static void op(Chip &c, uint32_t ch, uint32_t n, uint32_t tl) {
	const uint32_t s = slot(ch, n);
	write(c, 0x20 + s, 0x21); write(c, 0x40 + s, uint8_t(tl)); write(c, 0x60 + s, 0xF0); write(c, 0x80 + s, 0x0F); write(c, 0xE0 + s, 0);
}
static void voice(Chip &c, uint32_t ch, uint8_t out = 0x30) {
	op(c, ch, 0, 63); op(c, ch, 1, 0);
	write(c, chr(ch, 0xC0), out); write(c, chr(ch, 0xA0), 0x44); write(c, chr(ch, 0xB0), 0x32);
}
static void start(Chip &c, uint32_t flags = 0) { init(c, RATE_48K, flags); write(c, 0x105, 1); }
static double rms(const int32_t *x, uint32_t n, uint32_t side) { double s = 0; for (uint32_t i = 512; i < n; ++i) s += double(x[2 * i + side]) * x[2 * i + side]; return std::sqrt(s / (n - 512)); }
static void go(Chip &c, bool buses = true) {
	memset(R, 0, sizeof R); memset(C, 0, sizeof C);
	const Output o = {D, buses ? R : nullptr, buses ? C : nullptr, false};
	render(c, o, N);
}
static double unity_rms;
static void t_pan() {
	static Chip c;
	start(c, NO_PAN_LAW); voice(c, 0); go(c); unity_rms = rms(D, N, 0);
	CHECK(unity_rms > 1000, "%f", unity_rms);
	for (int p : {0, 1, 32, 63, 64, 65, 96, 126, 127}) {
		start(c); set_pan(c, 0, uint8_t(p)); voice(c, 0); go(c);
		const double x = (p > 1 ? p - 1 : 0) / 126.0, l = std::cos(x * M_PI / 2), r = std::sin(x * M_PI / 2);
		CHECK(std::fabs(rms(D, N, 0) - l * unity_rms) < 1.5, "pan %d left %f want %f", p, rms(D, N, 0), l * unity_rms);
		CHECK(std::fabs(rms(D, N, 1) - r * unity_rms) < 1.5, "pan %d right %f want %f", p, rms(D, N, 1), r * unity_rms);
	}
	start(c); voice(c, 0); go(c);
	CHECK(std::fabs(20 * std::log10(rms(D, N, 0) / unity_rms) + 3.0103) < 0.01, "centre %f", 20 * std::log10(rms(D, N, 0) / unity_rms));
}
static void t_sends() {
	static Chip c;
	for (int s : {0, 64, 127}) {
		start(c); voice(c, 0); set_sends(c, 0, uint8_t(s), uint8_t(127 - s)); go(c);
		const double want_r = rms(D, N, 0) * s / 127.0, want_c = rms(D, N, 0) * (127 - s) / 127.0;
		CHECK(std::fabs(rms(R, N, 0) - want_r) < 1.5, "send %d rev %f want %f", s, rms(R, N, 0), want_r);
		CHECK(std::fabs(rms(C, N, 1) - want_c) < 1.5, "send %d cho %f want %f", s, rms(C, N, 1), want_c);
	}
	// the whole voice at 127, exactly
	start(c); voice(c, 0); set_sends(c, 0, 127, 127); go(c);
	bool same = true; for (uint32_t i = 0; i < 2 * N; ++i) same &= D[i] == R[i] && D[i] == C[i];
	CHECK(same, "send 127 is the voice");
	// the dry output is the same with and without buses
	start(c); voice(c, 0); voice(c, 9); set_sends(c, 0, 90, 40); go(c, true); memcpy(D2, D, sizeof D);
	start(c); voice(c, 0); voice(c, 9); set_sends(c, 0, 90, 40); go(c, false);
	CHECK(!memcmp(D, D2, sizeof D), "dry differs with buses");
}
static void t_gain() {
	static Chip c;
	start(c); voice(c, 0); go(c); const double base = rms(D, N, 0);
	for (int g : {-32, -1, 0, 1, 32}) {
		start(c, NO_CLIP); voice(c, 0); set_gain(c, 0, int8_t(g)); go(c);
		const double db = 20 * std::log10(rms(D, N, 0) / base);
		CHECK(std::fabs(db - .75 * g) < 0.05, "gain %d: %f dB", g, db);
	}
	start(c); voice(c, 0); set_gain(c, 0, 100); CHECK(c.gain[0] == 32, "clamp"); set_gain(c, 0, -100); CHECK(c.gain[0] == -32, "clamp");
	// a gain does not touch a voice's modulation: the same wave scaled
	start(c, NO_CLIP); op(c, 0, 0, 10); op(c, 0, 1, 0); write(c, 0xC0, 0x30 | 4 << 1); write(c, 0xA0, 0x44); write(c, 0xB0, 0x32); go(c); memcpy(D2, D, sizeof D);
	start(c, NO_CLIP); set_gain(c, 0, 8); op(c, 0, 0, 10); op(c, 0, 1, 0); write(c, 0xC0, 0x30 | 4 << 1); write(c, 0xA0, 0x44); write(c, 0xB0, 0x32); go(c);
	double err = 0, sig = 0; const double k = std::pow(10, 6.0 / 20); for (uint32_t i = 0; i < 2 * N; ++i) { err += (D[i] - k * D2[i]) * (D[i] - k * D2[i]); sig += double(D[i]) * D[i]; }
	CHECK(10 * std::log10(sig / err) > 50, "gain scales the voice: SNR %f", 10 * std::log10(sig / err));
}
static void t_rules() {
	static Chip c;
	// a side whose C0 bit is clear is silent under the pan law
	start(c); voice(c, 0, 0x10); set_pan(c, 0, 127); go(c);
	CHECK(rms(D, N, 1) == 0 && rms(D, N, 0) < 2, "left only, panned right: %f %f", rms(D, N, 0), rms(D, N, 1));
	start(c); voice(c, 0, 0x20); go(c); CHECK(rms(D, N, 0) == 0 && rms(D, N, 1) > 500, "right only");
	// C and D add to the same sides
	start(c, NO_PAN_LAW); voice(c, 0, 0x50); go(c); CHECK(std::fabs(rms(D, N, 0) - 2 * unity_rms) < 3, "A+C: %f", rms(D, N, 0));
	// a 4-op pair takes its first channel's extensions, and the second's C0 bits
	start(c); write(c, 0x104, 1);
	for (uint32_t ch : {0u, 3u}) { op(c, ch, 0, 63); op(c, ch, 1, 63); }
	op(c, 3, 1, 0);                               // O4 alone heard
	write(c, chr(0, 0xC0), 0x10 | 1); write(c, chr(3, 0xC0), 0x20 | 1);
	set_pan(c, 0, 127); set_pan(c, 3, 1);
	set_sends(c, 0, 127, 0); set_gain(c, 0, -8); set_gain(c, 3, 16);
	write(c, chr(0, 0xA0), 0x44); write(c, chr(0, 0xB0), 0x32); go(c);
	CHECK(rms(D, N, 0) == 0, "second channel's bits: right only: %f", rms(D, N, 0));
	CHECK(std::fabs(rms(D, N, 1) - unity_rms * std::pow(10, -6.0 / 20)) < 20, "first's pan (hard right) + gain -6 dB: %f", rms(D, N, 1));
	CHECK(std::fabs(rms(R, N, 1) - rms(D, N, 1)) < 1.5, "first's send");
	// the drums their own channels': each pan moves only its own
	start(c);
	for (uint32_t ch : {6u, 7u, 8u}) { op(c, ch, 0, 0); op(c, ch, 1, 0); write(c, chr(ch, 0xC0), 0x30); write(c, chr(ch, 0xA0), 0x44); write(c, chr(ch, 0xB0), 0x12); }
	set_pan(c, 8, 1);            // tom and cymbal hard left
	write(c, 0xBD, 0x20 | 0x04); go(c);
	CHECK(rms(D, N, 1) == 0 && rms(D, N, 0) > 500, "tom panned by channel 8");
	start(c);
	for (uint32_t ch : {6u, 7u, 8u}) { op(c, ch, 0, 0); op(c, ch, 1, 0); write(c, chr(ch, 0xC0), 0x30); write(c, chr(ch, 0xA0), 0x44); write(c, chr(ch, 0xB0), 0x12); }
	set_pan(c, 6, 127); set_pan(c, 8, 1);
	write(c, 0xBD, 0x20 | 0x10); go(c);
	CHECK(rms(D, N, 0) == 0 && rms(D, N, 1) > 500, "bass drum panned by channel 6");
	// OPL2 mode: no pan law, both sides
	init(c, RATE_48K); voice(c, 0, 0x10); set_pan(c, 0, 1); go(c);
	CHECK(std::fabs(rms(D, N, 0) - unity_rms) < 2 && std::fabs(rms(D, N, 1) - unity_rms) < 2, "OPL2: %f %f", rms(D, N, 0), rms(D, N, 1));
	// the clip: 16 bits on dry, unless asked; never on the buses
	start(c);
	for (uint32_t ch = 0; ch < 18; ++ch) { voice(c, ch); set_sends(c, ch, 127, 0); set_gain(c, ch, 32); }
	go(c);
	int32_t dmax = 0, rmax = 0; for (uint32_t i = 0; i < 2 * N; ++i) { dmax = D[i] > dmax ? D[i] : dmax; rmax = R[i] > rmax ? R[i] : rmax; }
	CHECK(dmax == 32767, "clip %d", dmax); CHECK(rmax > 32767, "buses unclipped %d", rmax);
}
static void t_silent() {
	static Chip c;
	start(c);
	for (auto &x : D) x = 12345;
	for (auto &x : R) x = 777;
	const Output add = {D, R, nullptr, true};
	CHECK(render(c, add, N) == 0, "a silent chip reports nothing");
	bool same = true; for (uint32_t i = 0; i < 2 * N; ++i) same &= D[i] == 12345 && R[i] == 777;
	CHECK(same, "a silent chip writes nothing when adding");
	const Output wr = {D, nullptr, nullptr, false};
	CHECK(render(c, wr, N) == 0, "silent");
	same = true; for (uint32_t i = 0; i < 2 * N; ++i) same &= D[i] == 0;
	CHECK(same, "a silent chip writes zeros when writing");
	voice(c, 0);
	CHECK(render(c, wr, N) == SOUNDED, "a keyed voice sounds");
	// adding = writing + what was there, and a write between calls lands on the next sample
	start(c); voice(c, 0); const Output w2 = {D2, nullptr, nullptr, false}; render(c, w2, N);
	start(c); voice(c, 0); for (auto &x : D) x = 5; render(c, add, N);
	same = true; for (uint32_t i = 0; i < 2 * N; ++i) same &= D[i] == D2[i] + 5;
	CHECK(same, "add");
	// any way of cutting a run gives the same samples
	start(c); op(c, 0, 0, 20); op(c, 0, 1, 0); write(c, 0xC0, 0x30 | 6); write(c, 0xA0, 0x44); write(c, 0xB0, 0x32); render(c, w2, N);
	start(c); op(c, 0, 0, 20); op(c, 0, 1, 0); write(c, 0xC0, 0x30 | 6); write(c, 0xA0, 0x44); write(c, 0xB0, 0x32);
	for (uint32_t i = 0, n = 1; i < N; i += n, n = n % 7 + 1) { if (i + n > N) n = N - i; const Output p = {D + 2 * i, nullptr, nullptr, false}; render(c, p, n); }
	CHECK(!memcmp(D, D2, sizeof D), "1..7 frame runs differ from one long run");
}
// High quality on the four drums that have their own kernel, each on its own channel's output.
static void drums(Chip &c, uint32_t flags, uint8_t bits, uint8_t ws = 0) {
	start(c, flags);
	for (uint32_t ch : {6u, 7u, 8u}) {
		for (uint32_t n : {0u, 1u}) {
			const uint32_t s = slot(ch, n);
			write(c, 0x20 + s, 0x21); write(c, 0x40 + s, 0); write(c, 0x60 + s, 0xF0); write(c, 0x80 + s, 0x0F); write(c, 0xE0 + s, ws);
		}
		write(c, chr(ch, 0xC0), 0x30); write(c, chr(ch, 0xA0), 0x44); write(c, chr(ch, 0xB0), 0x12);
	}
	write(c, 0xBD, uint8_t(0x20 | bits));
}
static void t_hq_drums() {
	static Chip c;
	const struct { uint8_t bit; const char *name; } D_[] = {{0x01, "hi-hat"}, {0x08, "snare"}, {0x04, "tom"}, {0x02, "cymbal"}};
	for (const auto &d : D_) {
		drums(c, 0, d.bit); go(c); const double chip = rms(D, N, 0);
		CHECK(chip > 300, "%s sounds in chip mode: %f", d.name, chip);
		for (uint32_t fl : {uint32_t(HQ_PHASE), uint32_t(HQ_ENVELOPE), uint32_t(HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION)}) {
			drums(c, fl, d.bit); go(c); const double hq = rms(D, N, 0);
			CHECK(std::fabs(20 * std::log10(hq / chip)) < 0.6, "%s HQ %u level %+.2f dB from chip mode", d.name, fl, 20 * std::log10(hq / chip));
			memcpy(D2, D, sizeof D); drums(c, fl, d.bit); go(c);
			CHECK(!memcmp(D, D2, sizeof D), "%s HQ %u is deterministic", d.name, fl);
		}
	}
	// nothing keyed: silent, exactly, on both sides of the rhythm pair
	drums(c, HQ_PHASE | HQ_ENVELOPE, 0x04); set_pan(c, 8, 1); go(c);
	CHECK(rms(D, N, 1) == 0, "the tom alone, hard left: nothing right: %f", rms(D, N, 1));
	drums(c, HQ_PHASE | HQ_ENVELOPE, 0x01); set_pan(c, 8, 127); go(c);
	CHECK(rms(D, N, 1) > 300, "the hi-hat is channel 7's, centred, whatever channel 8 does");
	// HQ_PHASE reads the tom at its whole phase: more distinct sample values than a 1024-step table gives
	drums(c, 0, 0x04); go(c); memcpy(D2, D, sizeof D);
	drums(c, HQ_PHASE, 0x04); go(c);
	bool same = true; for (uint32_t i = 0; i < 2 * N; ++i) same &= D[i] == D2[i];
	CHECK(!same, "HQ_PHASE changes the tom");
	// HQ_ENVELOPE: a decaying drum's gain ramps: fewer equal neighbours in a square wave's plateau
	const auto steps = [](uint32_t flags) {
		static Chip e;
		start(e, flags);
		for (uint32_t n : {0u, 1u}) { const uint32_t s = slot(8, n); write(e, 0x20 + s, 0x21); write(e, 0x40 + s, 0); write(e, 0x60 + s, 0xF6); write(e, 0x80 + s, 0xFF); write(e, 0xE0 + s, 6); }
		write(e, chr(8, 0xC0), 0x30); write(e, chr(8, 0xA0), 0x44); write(e, chr(8, 0xB0), 0x12);
		write(e, 0xBD, 0x24);
		const Output o = {D, nullptr, nullptr, false}; render(e, o, N);
		uint32_t flat = 0; for (uint32_t i = 200; i + 1 < N; ++i) flat += D[2 * i] == D[2 * i + 2];
		return flat;
	};
	CHECK(steps(HQ_ENVELOPE) < steps(0), "HQ_ENVELOPE smooths a drum's decay: %u vs %u flat neighbours", steps(HQ_ENVELOPE), steps(0));
}
// However a run is cut into calls, the samples are the same: in every mode, voices and drums.
static void t_cuts() {
	static Chip c;
	const auto setup = [](Chip &x, uint32_t fl, bool rhythm) {
		init(x, RATE_48K, fl); write(x, 0x105, 1);
		for (uint32_t ch : {6u, 7u, 8u}) {
			for (uint32_t n : {0u, 1u}) {
				const uint32_t s = slot(ch, n);
				write(x, 0x20 + s, 0x01); write(x, 0x40 + s, n ? 0 : 12); write(x, 0x60 + s, 0xF6); write(x, 0x80 + s, 0x76); write(x, 0xE0 + s, 0);
			}
			write(x, chr(ch, 0xC0), 0x30 | 4 << 1); write(x, chr(ch, 0xA0), 0x44); write(x, chr(ch, 0xB0), rhythm ? 0x12 : 0x32);
		}
		if (rhythm) write(x, 0xBD, 0x3F);
	};
	static int32_t A[2 * 24000], B[2 * 24000];
	for (bool rhythm : {false, true})
		for (uint32_t fl : {uint32_t(0), uint32_t(1), uint32_t(2), uint32_t(4), uint32_t(7), MIX_SLOW, uint32_t(7 | MIX_SLOW)}) {
			setup(c, fl, rhythm);
			const Output o = {A, nullptr, nullptr, false};
			render(c, o, 24000);
			setup(c, fl, rhythm);
			for (uint32_t i = 0, n = 1; i < 24000; i += n, n = n % 7 + 1) {
				if (i + n > 24000) n = 24000 - i;
				const Output p = {B + 2 * i, nullptr, nullptr, false};
				render(c, p, n);
			}
			int diff = 0;
			for (uint32_t i = 0; i < 2 * 24000; ++i) diff += A[i] != B[i];
			CHECK(diff == 0, "%s flags %u: %d samples depend on the cuts", rhythm ? "drums" : "voices", fl, diff);
		}
}
// The mix, against its definition computed here in 64 bits from each voice's own row (the voice alone,
// at unity: NO_PAN_LAW, coefficient 4096, so its dry output is its row) and the chip's coefficients:
// dry = round(sum of s x c / 2^12), a bus = round(sum of s x q / 2^14), q = round(c x send / 2^10).
// Ordinary pans, gains and sends take the fast path; gains past its bound, the slow path; MIX_SLOW forces
// the slow path everywhere, which must give the same bits.
static void t_mix() {
	static Chip c, one;
	enum : uint32_t { V = 9, M = 1024 };
	static int32_t row[V][M], d[2 * M], r[2 * M], h[2 * M];
	// same: every voice additive at one pitch, in phase, so their sums peak together (past 32 bits when loud)
	const auto voices = [](Chip &x, uint32_t fl, uint32_t only, bool same = false) {
		start(x, fl);
		for (uint32_t ch = 0; ch < V; ++ch) {
			if (only < V && ch != only) continue;
			op(x, ch, 0, same ? 0 : 20 + ch); op(x, ch, 1, same ? 0 : ch);
			write(x, chr(ch, 0xC0), uint8_t(0x30 | (same ? 1 : ch & 1)));   // odd channels additive: two carriers
			write(x, chr(ch, 0xA0), uint8_t(same ? 0x40 : 0x40 + 17 * ch)); write(x, chr(ch, 0xB0), 0x32);
		}
	};
	for (uint32_t ch = 0; ch < V; ++ch) {
		voices(one, NO_PAN_LAW | NO_CLIP, ch);
		const Output o = {d, nullptr, nullptr, false};
		render(one, o, M);
		for (uint32_t j = 0; j < M; ++j) row[ch][j] = d[2 * j];
	}
	struct Case { const char *name; int8_t gain; uint32_t fl; };
	const Case cases[] = {{"fast", 0, NO_CLIP}, {"fast, loud", 6, NO_CLIP}, {"past the bound", 32, NO_CLIP}, {"MIX_SLOW", 0, NO_CLIP | MIX_SLOW},
	                      {"clipped", 32, 0}};
	for (const Case &k : cases) {
		voices(c, k.fl, V);
		for (uint32_t ch = 0; ch < V; ++ch) {
			set_pan(c, ch, uint8_t(ch * 15)); set_sends(c, ch, uint8_t(30 + 10 * ch), uint8_t(120 - 13 * ch));
			set_gain(c, ch, int8_t(k.gain >= 32 ? 32 : ch == 2 ? k.gain : ch % 3));   // 32: every voice, past 32 bits
		}
		memset(r, 0, sizeof r); memset(h, 0, sizeof h);
		const Output o = {d, r, h, false};
		render(c, o, M);
		int bad_d = 0, bad_b = 0;
		for (uint32_t j = 0; j < M; ++j) {
			int64_t sl = 0, sr = 0, vl = 0, vr = 0, gl = 0, gr = 0;
			for (uint32_t ch = 0; ch < V; ++ch) {
				const int64_t s = row[ch][j], cl = c.coef_l[ch], cr = c.coef_r[ch];
				sl += s * cl; sr += s * cr;
				vl += s * ((cl * c.rev_q[ch] + 512) >> 10); vr += s * ((cr * c.rev_q[ch] + 512) >> 10);
				gl += s * ((cl * c.cho_q[ch] + 512) >> 10); gr += s * ((cr * c.cho_q[ch] + 512) >> 10);
			}
			int64_t el = (sl + 2048) >> 12, er = (sr + 2048) >> 12;
			if (!(k.fl & NO_CLIP)) { el = el > 32767 ? 32767 : el < -32768 ? -32768 : el; er = er > 32767 ? 32767 : er < -32768 ? -32768 : er; }
			bad_d += (d[2 * j] != el) + (d[2 * j + 1] != er);
			bad_b += (r[2 * j] != (vl + 8192) >> 14) + (r[2 * j + 1] != (vr + 8192) >> 14) + (h[2 * j] != (gl + 8192) >> 14) + (h[2 * j + 1] != (gr + 8192) >> 14);
		}
		CHECK(bad_d == 0 && bad_b == 0, "the mix (%s): %d dry and %d bus samples off its definition", k.name, bad_d, bad_b);
	}
	CHECK(c.coef_l[2] > 32767 || c.coef_r[2] > 32767, "the gain of 32 is past the fast path's bound");
	// the slow path's sums, after the fast path's bound was known to hold: a gain raised past it since (a
	// chip that has not seen the bound fail, so what it keeps is "holds")
	static Chip fresh;
	voices(fresh, NO_CLIP, V, true); { const Output o = {d, nullptr, nullptr, false}; render(fresh, o, 64); }
	for (uint32_t ch = 0; ch < V; ++ch) { set_pan(fresh, ch, 0); set_gain(fresh, ch, 32); }   // hard left: past 32 bits
	{ const Output o = {d, nullptr, nullptr, false}; render(fresh, o, M); }
	static int32_t d2[2 * M];
	memcpy(d2, d, sizeof d2);
	voices(c, NO_CLIP | MIX_SLOW, V, true); { const Output o = {d, nullptr, nullptr, false}; render(c, o, 64); }
	for (uint32_t ch = 0; ch < V; ++ch) { set_pan(c, ch, 0); set_gain(c, ch, 32); }   // hard left: past 32 bits
	{ const Output o = {d, nullptr, nullptr, false}; render(c, o, M); }
	CHECK(!memcmp(d, d2, sizeof d2), "a gain raised past the bound after it held: the slow path's bits");
}
// play(): the same samples as driving each call by hand, with the extensions in the stream (HVO2 records).
static void t_play() {
	static Chip a[2], b[2];
	static const uint32_t rec[] = {
		4 | 0u << 4, 4 | 1u << 4, 1 | 0x105u << 6 | 1u << 15, 1 | 1u << 4 | 0x105u << 6 | 1u << 15,
		1 | 0x23u << 6 | 0x21u << 15, 1 | 0x43u << 6, 1 | 0x63u << 6 | 0xF0u << 15, 1 | 0x83u << 6 | 0x0Fu << 15,
		1 | 1u << 4 | 0x23u << 6 | 0x21u << 15, 1 | 1u << 4 | 0x43u << 6, 1 | 1u << 4 | 0x63u << 6 | 0xF0u << 15, 1 | 1u << 4 | 0x83u << 6 | 0x0Fu << 15,
		1 | 0xC0u << 6 | 0x30u << 15, 1 | 1u << 4 | 0xC0u << 6 | 0x30u << 15,
		2 | 0u << 6 | 20u << 15, 3 | 0u << 6 | 90u << 15 | (40u & 15) << 11 | (40u >> 4) << 23, 6 | 0u << 6 | uint32_t(uint8_t(-4)) << 15,
		1 | 0xA0u << 6 | 0x44u << 15, 1 | 0xB0u << 6 | 0x32u << 15, 1 | 1u << 4 | 0xA0u << 6 | 0x10u << 15, 1 | 1u << 4 | 0xB0u << 6 | 0x36u << 15,
		5 | 700u << 6, 5 | 1u << 4 | 700u << 6, 1 | 0xB0u << 6 | 0x12u << 15, 5 | 300u << 6, 5 | 1u << 4 | 300u << 6};
	memset(D, 0, sizeof D); memset(R, 0, sizeof R); memset(C, 0, sizeof C);
	const Output o = {D, R, C, true};
	const uint32_t res = play(a, 2, rec, sizeof rec / sizeof *rec, o, RATE_48K);
	CHECK(res == SOUNDED, "play sounded");
	static int32_t d2[2 * N], r2[2 * N], c2[2 * N];
	const Output q = {d2, r2, c2, true};
	const Output q2 = {d2 + 1400, r2 + 1400, c2 + 1400, true};
	for (Chip &x : b) { init(x, RATE_48K); write(x, 0x105, 1); write(x, 0x23, 0x21); write(x, 0x43, 0); write(x, 0x63, 0xF0); write(x, 0x83, 0x0F); write(x, 0xC0, 0x30); }
	set_pan(b[0], 0, 20); set_sends(b[0], 0, 90, 40); set_gain(b[0], 0, -4);
	write(b[0], 0xA0, 0x44); write(b[0], 0xB0, 0x32); write(b[1], 0xA0, 0x10); write(b[1], 0xB0, 0x36);
	render(b[0], q, 700); render(b[1], q, 700); write(b[0], 0xB0, 0x12); render(b[0], q2, 300); render(b[1], q2, 300);
	CHECK(!memcmp(D, d2, 8 * 1000) && !memcmp(R, r2, 8 * 1000) && !memcmp(C, c2, 8 * 1000), "play differs from by-hand");
	// the layout: chip 31's high bits in 31..29, chorus in 14..11 and 25..23
	static Chip many[32];
	const uint32_t k = 31, chorus = 100;
	const uint32_t v2[] = {4 | (k & 3) << 4 | (k >> 2) << 29, 3 | (k & 3) << 4 | (k >> 2) << 29 | 5u << 6 | 90u << 15 | (chorus & 15) << 11 | (chorus >> 4) << 23};
	play(many, 32, v2, 2, o, RATE_48K);
	CHECK(many[31].cho_q[5] == (100u * 4096 + 63) / 127 && many[31].rev_q[5] == (90u * 4096 + 63) / 127, "HVO2: chorus 100 on chip 31");
	// play reads HVO2 only: an HVO1 SENDS (chorus 100 in 29..23, so bit 29 set) is not chip 0's chorus 100
	for (Chip &x : many) init(x, RATE_48K);
	const uint32_t v1[] = {4, 4 | 1u << 29, 3 | 5u << 6 | 90u << 15 | 100u << 23};
	play(many, 32, v1, 3, o, RATE_48K);
	CHECK(many[0].cho_q[5] == 0 && many[0].rev_q[5] == 0, "an HVO1 record is not read as HVO1: chip 0 untouched");
	CHECK(many[4].rev_q[5] == (90u * 4096 + 63) / 127 && many[4].cho_q[5] != (100u * 4096 + 63) / 127, "... it is HVO2's chip 4");
}
// The bench's reader (bench/hvo2.hpp): HVO2 decoded field by field; HVO1 refused by its magic, naming the converter.
static void t_reader() {
	const uint32_t k = 29, chorus = 77;
	const uint32_t f[] = {hvo2::MAGIC, 1, 48000, 3,
		4 | (k & 3) << 4 | (k >> 2) << 29,
		1 | (k & 3) << 4 | (k >> 2) << 29 | 0x1A5u << 6 | 0xC3u << 15,
		2 | 17u << 6 | 127u << 15,
		3 | (k & 3) << 4 | (k >> 2) << 29 | 9u << 6 | 90u << 15 | (chorus & 15) << 11 | (chorus >> 4) << 23,
		6 | 3u << 6 | uint32_t(uint8_t(-32)) << 15,
		5 | 1000u << 6, 5 | 1u << 4 | 1000u << 6 | 1u << 22};
	hvo2::Trace t;
	CHECK(t.open(f, sizeof f / 4) && !t.error, "an HVO2 trace opens");
	const hvo2::Rec *r = t.begin();
	CHECK(t.end() - r == 7 && t.flags() == 3 && t.nchips == 30, "records %d, flags %u, chips %u", int(t.end() - r), t.flags(), t.nchips);
	CHECK(r[0].op() == hvo2::RESET && r[0].chip() == 29, "RESET chip %u", r[0].chip());
	CHECK(r[1].op() == hvo2::REG && r[1].chip() == 29 && r[1].reg() == 0x1A5 && r[1].data() == 0xC3, "REG %u %x %x", r[1].chip(), r[1].reg(), r[1].data());
	CHECK(r[2].op() == hvo2::PAN && r[2].chip() == 0 && r[2].channel() == 17 && r[2].data() == 127, "PAN");
	CHECK(r[3].op() == hvo2::SENDS && r[3].chip() == 29 && r[3].channel() == 9 && r[3].data() == 90 && r[3].chorus() == 77, "SENDS %u %u %u %u", r[3].chip(), r[3].channel(), r[3].data(), r[3].chorus());
	CHECK(r[4].op() == hvo2::GAIN && r[4].channel() == 3 && r[4].level() == -32, "GAIN");
	CHECK(r[5].frames(1) == 1000 && r[5].frames(2) == 2000 && r[6].chip() == 1 && r[6].frames(2) == 1000, "RUN: 48 kHz frames scale, bit 22 frames do not");
	uint32_t g[sizeof f / 4];
	memcpy(g, f, sizeof f); g[0] = 0x314F5648u;   // the HVO1 magic
	CHECK(!t.open(g, sizeof g / 4) && t.error && strstr(t.error, "HVO1") && strstr(t.error, "tools/hvo1to2.py"), "HVO1 refused, naming the converter: %s", t.error ? t.error : "(none)");
	CHECK(t.begin() == t.end(), "a refused trace has no records");
	g[0] = 0x12345678u;
	CHECK(!t.open(g, sizeof g / 4) && t.error && !strstr(t.error, "HVO1"), "not a trace: %s", t.error ? t.error : "(none)");
	memcpy(g, f, sizeof f); g[2] = 44100;
	CHECK(!t.open(g, sizeof g / 4) && t.error, "another rate refused");
	CHECK(!t.open(f, 3) && t.error, "a short header refused");
}
// A register write between calls is heard from the next sample in every mode, two voices at once (the RVV
// high-quality lanes keep a voice's state from call to call: a write must drop it).
static void t_live() {
	static Chip c;
	for (uint32_t fl : {0u, 1u, 2u, 4u, 7u}) {
		start(c, fl); voice(c, 0); voice(c, 1); go(c);
		const double before = rms(D, N, 0);
		write(c, 0x40 + slot(0, 1), 24); write(c, 0x40 + slot(1, 1), 24);   // 18 dB down
		go(c);
		const double db = 20 * std::log10(rms(D, N, 0) / before);
		CHECK(std::fabs(db + 18) < 0.5, "flags %u: a level write gives %+.2f dB, want -18", fl, db);
	}
}
// The version: the library's matches the header's, and the string matches the number.
static void t_version() {
	CHECK(version() == HVOPL3_VERSION_NUMBER, "%u", unsigned(version()));
	CHECK(strcmp(version_string(), HVOPL3_VERSION) == 0, "%s", version_string());
	CHECK(hvopl3_version() == version() && hvopl3_version_string() == version_string(), "the C ABI");
	char s[16];
	snprintf(s, sizeof s, "%d.%d.%d", HVOPL3_VERSION_MAJOR, HVOPL3_VERSION_MINOR, HVOPL3_VERSION_PATCH);
	CHECK(strcmp(s, HVOPL3_VERSION) == 0, "%s", HVOPL3_VERSION);
	CHECK(HVOPL3_VERSION_MINOR < 100 && HVOPL3_VERSION_PATCH < 100, "the number's fields overlap");
}

// inaudible(): a released voice reported so before its envelope ends (active still set), and from
// then on exactly silent -- FM and additive, 2-op and 4-op, plain and high quality, at +24 dB of gain,
// with tremolo.
static void t_inaudible() {
	for (uint32_t flags : {0u, uint32_t(HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION | NO_CLIP)})
		for (uint32_t kind = 0; kind < 3; ++kind) {   // 0 FM 2-op, 1 additive 2-op, 2 4-op (FM-FM)
			static Chip c;
			start(c, flags);
			if (kind == 2) write(c, 0x104, 1);
			const uint32_t chans = kind == 2 ? 2 : 1;
			for (uint32_t x = 0; x < chans; ++x)
				for (uint32_t n = 0; n < 2; ++n) {
					const uint32_t sl = slot(3 * x, n);
					write(c, 0x20 + sl, 0xA1);                    // tremolo, sustained, x1
					write(c, 0x40 + sl, uint8_t(n || kind == 1 ? 6 : 20));
					write(c, 0x60 + sl, 0xF0); write(c, 0x80 + sl, 0x05);   // release rate 5
				}
			write(c, 0xBD, 0x80);                                 // deep tremolo
			write(c, chr(0, 0xC0), uint8_t(0x30 | (kind == 1 ? 1 : 0)));
			if (kind == 2) write(c, chr(3, 0xC0), 0x30);
			write(c, chr(0, 0xA0), 0x44); write(c, chr(0, 0xB0), 0x32);
			set_gain(c, 0, 32);
			go(c, false);
			CHECK(!inaudible(c, 0), "keyed: not inaudible");
			write(c, chr(0, 0xB0), 0x12);                         // key off
			uint32_t blocks = 0;
			bool loud_before = false;
			while (!inaudible(c, 0) && blocks < 4000) {
				memset(R, 0, sizeof R);
				const Output o = {D, nullptr, nullptr, false};
				render(c, o, 64);
				loud_before = false;
				for (uint32_t i = 0; i < 128; ++i) loud_before = loud_before || D[i];
				++blocks;
			}
			CHECK(blocks < 4000 && (c.active & 1), "kind %u, flags %x: inaudible after %u blocks, its envelope not ended (active)", kind, flags, blocks);
			bool zero = true;
			for (int b = 0; b < 40 && (c.active & 1); ++b) {
				const Output o = {D, nullptr, nullptr, false};
				render(c, o, 1024);
				for (uint32_t i = 0; i < 2048; ++i) zero = zero && D[i] == 0;
			}
			CHECK(zero, "kind %u, flags %x: silent from then on, to its envelope's end", kind, flags);
			(void)loud_before;
		}
	// a voice in a slow attack from silence: as quiet as that, but rising
	static Chip c;
	start(c);
	voice(c, 0);
	write(c, 0x60 + slot(0, 1), 0x10);                            // attack rate 1
	go(c, false);
	CHECK(!inaudible(c, 0), "a slow attack, still far down: not inaudible");
}

int main() {
	t_version();
	t_live();
	t_cuts();
	t_mix();
	t_play();
	t_reader();
	t_hq_drums();
	t_pan(); t_sends(); t_gain(); t_rules(); t_silent(); t_inaudible();
	printf("t_ext: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
