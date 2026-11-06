// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3.cpp -- the chip.
//
// The vibrato steps follow Opal (public domain, Reality). Adapted from ymfm (ymfm_opl.cpp, ymfm_fm.ipp): the envelope increment patterns, the key scale
// attenuation table, the waveform shapes, the rhythm phase shapes and the LFO counters:
//
//   BSD 3-Clause License. Copyright (c) 2021, Aaron Giles. All rights reserved.
//   Redistribution and use in source and binary forms, with or without modification, are permitted
//   provided that the following conditions are met: (1) Redistributions of source code must retain
//   the above copyright notice, this list of conditions and the following disclaimer. (2)
//   Redistributions in binary form must reproduce the above copyright notice, this list of
//   conditions and the following disclaimer in the documentation and/or other materials provided
//   with the distribution. (3) Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from this software without
//   specific prior written permission.
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
//   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
//   FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
//   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//   CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
//   OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
//   POSSIBILITY OF SUCH DAMAGE.
//
#include "hvopl3.hpp"
#include "hvopl3_test.hpp"
#define HV_MIX_STALE(c) ((c).mix_bound = 0)   // a coefficient changed: the mix's fast-path bound is not known
#include <string.h>

#define HV_INLINE inline __attribute__((always_inline))
#if defined(__riscv_vector) || defined(__riscv_xtheadvector)
#define HV_RVV 1
#include <riscv_vector.h>
#if defined(__riscv_zve32f) || defined(__riscv_xtheadvector)
#define HV_RVV_HQ 1   // high quality in float lanes: RVV 1.0 with float, or the C920's RVV 0.7.1; not Zve32x
#if defined(__riscv_xtheadvector) || defined(HVOPL_NO_RTZ)
#define HV_NO_RTZ 1   // no truncating conversion (RVV 0.7.1): v_trunc corrects the rounding one instead
#endif
#endif
#elif defined(__riscv_dsp) && !defined(HVOPL_NO_DSP)
#define HV_DSP 1   // the Andes DSP extension (-mext-dsp: the A25); HVOPL_NO_DSP keeps the scalar path
#endif
#ifdef HV_RVV_HQ
#define HV_LANES_STALE(c) ((c).h_ok = 0)   // the high-quality lanes no longer hold the voices' state
#else
#define HV_LANES_STALE(c) ((void)0)
#endif

namespace hvopl3 {
namespace {

#include "hvopl3_tables.inc"

enum { ATTACK, DECAY, SUSTAIN, RELEASE };
enum { K_NONE, K_2OP, K_4OP, K_BD, K_RHY };   // K_NONE: the second half of a pair; K_RHY: 7 and 8
// `tick` is the envelope clock: the chip's envelopes step this many samples after the counter's multiples.
enum : uint32_t { EG_LAG = 4 };
enum : uint32_t { HQ_ANY = HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION, ENV_MAX = 0x3FE, QUIET = 0xC00, CHIP_HZ = 14318180, CHIP_DIV = 288 };

// The chip's output: exponent ROM + hidden bit, one bit up, shifted by the attenuation's whole part.
// Reversed, so the attenuation's fraction indexes it directly.
struct PowTable {
	uint16_t v[256];
	constexpr PowTable() : v() {
		for (int i = 0; i < 256; ++i) v[i] = uint16_t((EXPO[255 - i] + 1024) << 1);
	}
};
constexpr PowTable POW;

// Each waveform: 10-bit phase -> attenuation, bit 15 the sign. Eight tables, read-only, built here.
struct WaveTables {
	uint16_t w[8][1024];
	static constexpr uint16_t sine(uint32_t i) {
		const uint32_t q = (i & 256) ? (~i & 255) : (i & 255);
		return uint16_t(LOGSIN[q] | ((i >> 9) & 1) << 15);
	}
	constexpr WaveTables() : w() {
		const uint16_t zero = 0xFFF;   // exactly 0 out, always positive (as the chip)
		for (uint32_t i = 0; i < 1024; ++i) {
			const uint32_t neg = (i >> 9) & 1;
			w[0][i] = sine(i);
			w[1][i] = neg ? zero : sine(i);
			w[2][i] = sine(i) & 0x7FFF;
			w[3][i] = (i & 256) ? zero : uint16_t(sine(i) & 0x7FFF);
			// twice the pitch: the fold is taken before the doubling, so only even log-sine entries are used
			const uint32_t half = ((i & 128) ? (~i & 127) : (i & 127)) * 2;
			w[4][i] = neg ? zero : uint16_t(LOGSIN[half] | ((i >> 8) & 1) << 15);
			w[5][i] = neg ? zero : LOGSIN[half];
			w[6][i] = uint16_t(neg << 15);
			// a falling saw in the log domain; capped where the output is 0 anyway, so the shift stays below 32
			const uint32_t saw = (neg ? (i ^ 0x3FF) : i) << 3;
			w[7][i] = uint16_t((saw > 0xFFF ? 0xFFF : saw) | neg << 15);
		}
	}
};
constexpr WaveTables WAVES;

constexpr uint8_t KSL_ROM[16] = {0, 24, 32, 37, 40, 43, 45, 47, 48, 50, 51, 52, 53, 54, 55, 56};

// Eight steps of each rate's pattern, a nibble a step: below 48, by the rate's low two bits (as the
// chip: rates 4-7 too).
constexpr uint32_t STEPS_LOW[4] = {0x10101010, 0x10111010, 0x11101110, 0x11111110};
constexpr uint32_t STEPS_HIGH[16] = {
	0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
	0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888};
HV_INLINE uint32_t rate_step(uint32_t rate, uint32_t idx) {
	uint32_t p;
	if (rate >= 48) p = STEPS_HIGH[rate - 48];
	else if (rate < 4) return 0;
	else p = STEPS_LOW[rate & 3];
	return (p >> (4 * idx)) & 15;
}

// The slot offset in a register's low 5 bits -> 2 x channel + n, within a bank; -1: none.
constexpr int8_t SLOT_OP[32] = {0, 2, 4, 1, 3, 5, -1, -1, 6, 8, 10, 7, 9, 11, -1, -1,
                                12, 14, 16, 13, 15, 17, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

// The lowest set bit's index, m != 0 (no target may need libgcc's: de Bruijn).
HV_INLINE uint32_t lowest_bit(uint32_t m) {
	static constexpr uint8_t T[32] = {0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
	                                  31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9};
	return T[((m & (0u - m)) * 0x077CB531u) >> 27];
}

// ---- time ------------------------------------------------------------------------------------

HV_INLINE void pos_at(const Chip &c, uint32_t at, uint32_t &ti, uint32_t &f) {
	const uint32_t j = at - c.now;
	const uint64_t a = uint64_t(c.tfrac) + uint64_t(j) * c.r_frac;
	ti = c.tick + j * c.r_int + uint32_t(a >> 32);
	f = uint32_t(a);
}
// The ticks clocked before sample `at` is made.
HV_INLINE uint32_t ticks_before(const Chip &c, uint32_t at) {
	uint32_t ti, f;
	pos_at(c, at, ti, f);
	return ti + c.r_int + uint32_t((uint64_t(f) + c.r_frac) >> 32);
}
// How many samples from `at` can be made before `target` is clocked.
HV_INLINE uint32_t samples_before(const Chip &c, uint32_t at, uint32_t target) {
	uint32_t ti, f;
	pos_at(c, at, ti, f);
	const int32_t d = int32_t(target - ti);
	if (d <= 0) return 0;
	const uint32_t cap = uint32_t(d) > 0x7FFF ? 0x7FFFu : uint32_t(d);
	const uint32_t x = (cap << 16) - (f >> 16);
	const uint32_t n = uint32_t((uint64_t(x) * c.rinv_q16 + 0xFFFFFFFFull) >> 32);
	return n ? n - 1 : 0;
}

// ---- derived values ---------------------------------------------------------------------------

// A pair's second half takes pitch and key from its first.
constexpr int8_t PAIR_FIRST[18] = {-1, -1, -1, 0, 1, 2, -1, -1, -1, -1, -1, -1, 9, 10, 11, -1, -1, -1};
constexpr int8_t PAIR_BIT[18] = {0, 1, 2, 0, 1, 2, -1, -1, -1, 3, 4, 5, 3, 4, 5, -1, -1, -1};
HV_INLINE bool pair_on(const Chip &c, uint32_t ch) {
	return PAIR_BIT[ch] >= 0 && (c.r_105 & 1) && (c.r_104 >> PAIR_BIT[ch] & 1);
}
HV_INLINE uint32_t freq_chan(const Chip &c, uint32_t ch) {
	return (PAIR_FIRST[ch] >= 0 && pair_on(c, ch)) ? uint32_t(PAIR_FIRST[ch]) : ch;
}
// The channel whose voice this channel's operators sound in.
HV_INLINE uint32_t voice_of(const Chip &c, uint32_t ch) {
	if ((c.r_bd & 0x20) && ch == 8) return 7;
	return freq_chan(c, ch);
}

void update_att(Chip &c, uint32_t i) {
	uint32_t a = c.env[i] + c.base[i];
	if (c.r_20[i] & 0x80) a += c.am;
	if (a > ENV_MAX) a = ENV_MAX;
	c.att[i] = uint16_t(a << 2);
}

void sync_idle(Chip &c, uint32_t ch);

void update_inc(Chip &c, uint32_t i) {
	sync_idle(c, i / 2);
	const uint32_t fb = c.fnblk[freq_chan(c, i / 2)];
	const uint32_t fnum = fb & 1023, block = fb >> 10;
	uint32_t f = fnum;
	if ((c.r_20[i] & 0x40) && (c.pm & 3)) {    // eight steps: 0, 1/2, 1, 1/2, 0, -1/2, -1, -1/2
		uint32_t v = fnum >> 7;
		if (!(c.r_bd & 0x40)) v >>= 1;
		if (c.pm & 1) v >>= 1;
		f = (c.pm & 4) ? f - v : f + v;
	}
	const uint32_t step = (((f & 0x3FF) << block) * c.mult2[i]) >> 1;
	c.inc[i] = uint32_t((uint64_t(step) * c.inc_k) >> 19);
}

HV_INLINE uint32_t state_rate(const Chip &c, uint32_t i) { return c.rate[i][c.state[i]]; }

// After tick `after`: take the state changes due, and find the next tick that changes the envelope.
void eg_schedule(Chip &c, uint32_t i, uint32_t after) {
	if (c.state[i] == ATTACK && c.env[i] == 0) c.state[i] = DECAY;
	if (c.state[i] == DECAY && c.env[i] >= c.sl[i]) c.state[i] = SUSTAIN;
	const uint32_t rate = state_rate(c, i);
	const bool attack = c.state[i] == ATTACK;
	if (rate < 4 || (attack && rate >= 62) || (!attack && c.env[i] >= ENV_MAX)) {
		c.pending[i] = 0;
		return;
	}
	const uint32_t rs = rate >> 2;
	const uint32_t shift = (rs < 11 ? 11 - rs : 0) + 1;
	uint32_t k = ((after >> shift) + 1) << shift;
	while (!rate_step(rate, (k >> shift) & 7)) k += 1u << shift;
	c.next_tick[i] = k;
	c.pending[i] = 1;
}

void eg_clock(Chip &c, uint32_t i, uint32_t k) {
	const uint32_t rate = state_rate(c, i);
	const uint32_t rs = rate >> 2;
	const uint32_t shift = (rs < 11 ? 11 - rs : 0) + 1;
	const uint32_t inc = rate_step(rate, (k >> shift) & 7);
	int32_t e = c.env[i] >> 1;       // nine bits, every other tick
	if (c.state[i] == ATTACK) {
		if (rate < 62) e += (~e * int32_t(inc)) >> 3;
		if (e < 0) e = 0;
	} else {
		e += int32_t(inc);
		if (e > 511) e = 511;
	}
	c.env[i] = uint16_t(e << 1);
}

// The operators of a voice.
struct OpList { uint8_t n, o[4]; };
HV_INLINE OpList voice_ops(const Chip &c, uint32_t ch) {
	OpList l;
	const uint8_t a = uint8_t(2 * ch);
	switch (c.kind[ch]) {
	case K_4OP: l.n = 4; l.o[0] = a; l.o[1] = a + 1; l.o[2] = a + 6; l.o[3] = a + 7; break;
	case K_RHY: l.n = 4; l.o[0] = 14; l.o[1] = 15; l.o[2] = 16; l.o[3] = 17; break;
	case K_NONE: l.n = 0; break;
	default: l.n = 2; l.o[0] = a; l.o[1] = a + 1; break;
	}
	return l;
}

// Sets next_event, and whether the voice is still alive at all.
void voice_resched(Chip &c, uint32_t ch, uint32_t at, uint32_t least) {
	const OpList l = voice_ops(c, ch);
	uint32_t best = 0x40000000;
	bool alive = false;
	for (uint32_t j = 0; j < l.n; ++j) {
		const uint32_t i = l.o[j];
		if (c.keyed[i] || c.env[i] < ENV_MAX || c.state[i] != RELEASE) alive = true;
		if (c.pending[i]) {
			const uint32_t s = samples_before(c, at, c.next_tick[i]);
			if (s < best) best = s;
		}
	}
	if (best < least) best = least;
	c.next_event[ch] = at + best;
	if (alive) c.active |= 1u << ch;
	else {
		if (c.active >> ch & 1) {
			c.idle_at[ch] = at;
			if (c.kind[ch] == K_4OP) c.idle_at[ch + 3] = at;
		}
		c.active &= ~(1u << ch);
	}
}

void refresh_op(Chip &c, uint32_t i) {
	const uint32_t fc = freq_chan(c, i / 2), fb = c.fnblk[fc];
	const uint32_t fnum = fb & 1023, block = fb >> 10;
	uint32_t lvl = (c.r_40[i] & 63u) << 3;
	const uint32_t ksl = c.r_40[i] >> 6;
	if (ksl) {
		const int32_t a = int32_t(KSL_ROM[fnum >> 6]) - 8 * int32_t(block ^ 7);
		// the register's two bits are in the opposite order: 1 is the largest shift but one
		if (a > 0) lvl += uint32_t(a) << ((ksl >> 1) | (ksl & 1) << 1);
	}
	c.base[i] = uint16_t(lvl);
	const uint32_t code = block << 1 | ((fnum >> (9 - ((c.r_08 >> 6) & 1))) & 1);
	const uint32_t ksr = (c.r_20[i] & 0x10) ? code : code >> 2;
	const auto eff = [ksr](uint32_t r) -> uint8_t {
		if (!r) return 0;
		const uint32_t v = r * 4 + ksr;
		return uint8_t(v > 63 ? 63 : v);
	};
	c.rate[i][ATTACK] = eff(c.r_60[i] >> 4);
	c.rate[i][DECAY] = eff(c.r_60[i] & 15);
	c.rate[i][RELEASE] = eff(c.r_80[i] & 15);
	c.rate[i][SUSTAIN] = (c.r_20[i] & 0x20) ? 0 : c.rate[i][RELEASE];
	uint32_t s = c.r_80[i] >> 4;
	s |= (s + 1) & 0x10;
	c.sl[i] = uint16_t(s << 5);
	update_inc(c, i);
	update_att(c, i);
}

// After a change to an operator's rates, level or state: in force at sample `now`.
void op_changed(Chip &c, uint32_t i) {
	eg_schedule(c, i, c.tick);
	voice_resched(c, voice_of(c, i / 2), c.now, 0);
}

// A silent operator's outputs are 0 or -1, by the sign of the wave at its phase. Its feedback history
// is the seed of the loop's orbit when it is next heard, so it is kept, without running the operator.
HV_INLINE int16_t silent_out(const Chip &c, uint32_t i, uint32_t back) {
	const uint32_t p = c.ph[i] - back * c.inc[i];
	return int16_t(-int32_t(WAVES.w[c.wave[i]][(p >> 22) & 1023] >> 15));
}
HV_INLINE void silent_history(Chip &c, uint32_t a, uint32_t ch) {
	c.fb0[ch] = silent_out(c, a, 0);
	c.fb1[ch] = silent_out(c, a, 1);
	if (c.fb_step == 2) { c.fb2[ch] = silent_out(c, a, 2); c.fb3[ch] = silent_out(c, a, 3); }
}

// A voice that is off makes no samples, but the chip's phases run: catch them up when they matter.
void sync_idle(Chip &c, uint32_t ch) {
	const uint32_t v = voice_of(c, ch);
	if ((c.active >> v & 1) || ((c.r_bd & 0x20) && v >= 6 && v <= 8)) { c.idle_at[ch] = c.now; return; }
	const uint32_t gap = c.now - c.idle_at[ch];
	if (gap) {
		c.ph[2 * ch] += c.inc[2 * ch] * gap;
		c.ph[2 * ch + 1] += c.inc[2 * ch + 1] * gap;
		c.idle_at[ch] = c.now;
	}
}

// A key takes effect at the next sample: an off and an on between two samples are no edge at all.
HV_INLINE void set_key(Chip &c, uint32_t i, uint32_t bit, bool on) {
	c.key[i] = on ? (c.key[i] | bit) : (c.key[i] & ~bit);
	c.key_dirty[i / 32] |= 1u << (i & 31);
}

HV_INLINE float atten_gain(float a);

// The gain line starts afresh at this level (a key-on, or high quality being turned on).
void hq_start(Chip &c, uint32_t i) {
	c.hq_gain[i] = atten_gain(float(c.att[i]));
	c.hq_slope[i] = 0.0f;
	c.hq_t0[i] = c.now;
	c.hq_att[i] = 0xFFFF;
}

void apply_keys(Chip &c) {
	HV_LANES_STALE(c);
	for (uint32_t w = 0; w < 2; ++w) {
		for (uint32_t m = c.key_dirty[w]; m; m &= m - 1) {
			const uint32_t i = w * 32 + lowest_bit(m);
			const bool on = c.key[i], was = c.keyed[i];
			if (on == was) continue;
			c.keyed[i] = on;
			if (on) {
				sync_idle(c, i / 2);
				// the loop starts from what it held: the silent outputs before this key-on
				if (!(i & 1) && !(c.active >> voice_of(c, i / 2) & 1)) silent_history(c, i, i / 2);
				c.state[i] = ATTACK;
				c.ph[i] = 0;
				if (c.rate[i][ATTACK] >= 62) c.env[i] = 0;
			} else {
				c.state[i] = RELEASE;
			}
			update_att(c, i);
			if (c.flags & HQ_ENVELOPE) hq_start(c, i);
			op_changed(c, i);
		}
		c.key_dirty[w] = 0;
	}
}

// The output coefficients of a voice, from its gain, its pan and its C0 bits.
void update_coef(Chip &c, uint32_t ch) {
	const uint32_t oc = c.kind[ch] == K_4OP ? ch + 3 : ch;
	const uint32_t bits = (c.r_105 & 1) ? c.r_c0[oc] >> 4 : 3;
	const uint32_t nl = (bits & 1) + (bits >> 2 & 1), nr = (bits >> 1 & 1) + (bits >> 3 & 1);
	const bool law = (c.r_105 & 1) && !(c.flags & NO_PAN_LAW);
	const uint32_t pl = law ? PAN_L[c.pan[ch]] : 32768, pr = law ? PAN_R[c.pan[ch]] : 32768;
	const uint32_t g = GAIN[c.gain[ch] + 32];
	const uint32_t scale = ((c.r_bd & 0x20) && ch >= 6 && ch <= 8) ? 2 : 1;
	c.coef_l[ch] = int32_t(((uint64_t(pl) * g + (1u << 16)) >> 17) * nl * scale);   // Q15 x Q14 -> Q12
	c.coef_r[ch] = int32_t(((uint64_t(pr) * g + (1u << 16)) >> 17) * nr * scale);
	HV_MIX_STALE(c);
}

// The kinds of voice, after anything that can change them.
void update_kinds(Chip &c) {
	const bool rhythm = c.r_bd & 0x20;
	for (uint32_t ch = 0; ch < CHANNELS; ++ch) {
		uint8_t k = K_2OP;
		if (PAIR_BIT[ch] >= 0 && pair_on(c, ch)) k = PAIR_FIRST[ch] >= 0 ? K_NONE : K_4OP;
		if (rhythm && ch == 6) k = K_BD;
		if (rhythm && ch == 7) k = K_RHY;
		if (rhythm && ch == 8) k = K_NONE;
		c.kind[ch] = k;
	}
	c.active = 0;
	for (uint32_t ch = 0; ch < CHANNELS; ++ch) {
		update_coef(c, ch);
		voice_resched(c, ch, c.now, 0);
	}
	uint32_t am = 0, vib = 0;
	for (uint32_t i = 0; i < OPERATORS; ++i) {
		const uint32_t v = voice_of(c, i / 2);
		if (c.r_20[i] & 0x80) am |= 1u << v;
		if (c.r_20[i] & 0x40) vib |= 1u << v;
	}
	c.am_chans = am;
	c.vib_chans = vib;
}

// A channel's own key, as its operators see it.
void apply_channel_key(Chip &c, uint32_t ch) {
	const uint32_t fc = freq_chan(c, ch);
	const bool on = c.kon[fc];
	set_key(c, 2 * ch, 1, on);
	set_key(c, 2 * ch + 1, 1, on);
}

void apply_drum_keys(Chip &c) {
	const uint32_t d = (c.r_bd & 0x20) ? c.r_bd : 0;
	set_key(c, 12, 2, d & 0x10);
	set_key(c, 13, 2, d & 0x10);
	set_key(c, 14, 2, d & 0x01);
	set_key(c, 15, 2, d & 0x08);
	set_key(c, 16, 2, d & 0x04);
	set_key(c, 17, 2, d & 0x02);
}

void lfo_update(Chip &c, uint32_t tick) {
	const uint32_t t = tick + EG_LAG;       // the LFOs are not behind: only the envelope clock is
	const uint32_t n = t % (210 * 64);
	const uint32_t tri = n < 105 * 64 ? n : 210 * 64 + 63 - n;
	const uint16_t am = uint16_t(tri >> (9 - 2 * (c.r_bd >> 7)));
	const int8_t pm = int8_t((t >> 10) & 7);
	if (am != c.am) {
		HV_LANES_STALE(c);
		c.am = am;
		for (uint32_t m = c.am_chans & c.active; m; m &= m - 1) {
			const uint32_t ch = lowest_bit(m);
			const OpList l = voice_ops(c, ch);
			for (uint32_t j = 0; j < l.n; ++j) update_att(c, l.o[j]);
		}
	}
	if (pm != c.pm) {
		HV_LANES_STALE(c);
		c.pm = pm;
		for (uint32_t m = c.vib_chans & c.active; m; m &= m - 1) {
			const uint32_t ch = lowest_bit(m);
			const OpList l = voice_ops(c, ch);
			for (uint32_t j = 0; j < l.n; ++j) update_inc(c, l.o[j]);
		}
	}
	c.lfo_next = ((t | 127) + 1) - EG_LAG;
}

// The envelopes of a voice, up to the ticks clocked before `at`.
void voice_event(Chip &c, uint32_t ch, uint32_t at) {
	const uint32_t t = ticks_before(c, at);
	const OpList l = voice_ops(c, ch);
	for (uint32_t j = 0; j < l.n; ++j) {
		const uint32_t i = l.o[j];
		if (!c.pending[i] || int32_t(t - c.next_tick[i]) < 0) continue;
		do {
			const uint32_t k = c.next_tick[i];
			eg_clock(c, i, k);
			eg_schedule(c, i, k);
		} while (c.pending[i] && int32_t(t - c.next_tick[i]) >= 0);
		update_att(c, i);
	}
	voice_resched(c, ch, at, 1);
}

// ---- kernels: a segment in which every attenuation is a constant --------------------------------

// A table pointer the compiler must keep whole in a register: on the A25 it otherwise splits it into the
// table and an offset and adds both at every read.
// And loops with a feedback history unrolled twice, so the history needs no register moves.
#ifdef HV_DSP
#define HV_KEEP(p) asm("" : "+r"(p))
#define HV_UNROLL2 _Pragma("GCC unroll 2")
#else
#define HV_KEEP(p) ((void)0)
#define HV_UNROLL2
#endif

#ifdef HV_DSP
// The A25's form, the same bits: the entry read signed, so the sign is bit 31 of the sum (the attenuation
// never carries into bit 15) and its mask one arithmetic shift.
HV_INLINE int32_t op_out(const uint16_t *wave, uint32_t idx, uint32_t att) {
	const int32_t e = int32_t(reinterpret_cast<const int16_t *>(wave)[idx & 1023]) + int32_t(att);
	const int32_t v = POW.v[e & 255] >> ((e >> 8) & 0x7F);
	return v ^ (e >> 31);
}
#else
HV_INLINE int32_t op_out(const uint16_t *wave, uint32_t idx, uint32_t att) {
	const uint32_t e = wave[idx & 1023] + att;
	const int32_t v = POW.v[e & 255] >> ((e >> 8) & 0x7F);
	const int32_t s = -int32_t(e >> 15);
	return v ^ s;   // the chip negates by inverting
}
#endif

#ifndef HV_RVV
// Op 2 alone: a carrier with no modulation, or an additive operator.
void k_one(const Chip &c, uint32_t i, uint32_t &ph, int16_t *out, uint32_t n) {
	const uint16_t *w = WAVES.w[c.wave[i]];
	HV_KEEP(w);
	const uint32_t inc = c.inc[i], att = c.att[i];
	uint32_t p = ph;
	for (uint32_t j = 0; j < n; ++j) {
		p += inc;
		out[j] = int16_t(op_out(w, p >> 22, att));
	}
	ph = p;
}

// Op 1 with feedback, its outputs.
#endif  // !HV_RVV

// At 96 kHz the chip's feedback (the mean of its last two outputs) spans twice as many samples: the
// mean of samples 2 and 4 back keeps its delay and its null at 24 kHz.
void k_fb96(Chip &c, uint32_t i, uint32_t ch, uint32_t shift, int16_t *out, uint32_t n) {
	const uint16_t *w = WAVES.w[c.wave[i]];
	HV_KEEP(w);
	const uint32_t inc = c.inc[i], att = c.att[i];
	uint32_t p = c.ph[i];
	int32_t y1 = c.fb0[ch], y2 = c.fb1[ch], y3 = c.fb2[ch], y4 = c.fb3[ch];
	for (uint32_t j = 0; j < n; ++j) {
		p += inc;
		const int32_t v = op_out(w, (p >> 22) + uint32_t((y2 + y4) >> shift), att);
		y4 = y3; y3 = y2; y2 = y1; y1 = v;
		out[j] = int16_t(v);
	}
	c.ph[i] = p;
	c.fb0[ch] = int16_t(y1); c.fb1[ch] = int16_t(y2); c.fb2[ch] = int16_t(y3); c.fb3[ch] = int16_t(y4);
}

void k_fb(const Chip &c, uint32_t i, uint32_t &ph, int32_t &f0io, int32_t &f1io, uint32_t shift, int16_t *out, uint32_t n) {
	const uint16_t *w = WAVES.w[c.wave[i]];
	HV_KEEP(w);
	const uint32_t inc = c.inc[i], att = c.att[i];
	uint32_t p = ph;
	int32_t f0 = f0io, f1 = f1io;
	const uint32_t on = shift < 16 ? ~0u : 0u;   // shift 31 alone would give -1 on a negative history
	HV_UNROLL2
	for (uint32_t j = 0; j < n; ++j) {
		p += inc;
		const int32_t v = op_out(w, (p >> 22) + (uint32_t((f0 + f1) >> shift) & on), att);
		f1 = f0;
		f0 = v;
		out[j] = int16_t(v);
	}
	ph = p;
	f0io = f0;
	f1io = f1;
}

#ifdef HV_DSP
// The A25: an additive voice in one loop, op 1 (with its feedback) + op 2, in place of k_fb, k_one and
// add_row (the same sum: each operator's output fits 16 bits).
void k_add(const Chip &c, uint32_t a, uint32_t b, uint32_t &pha, uint32_t &phb, int32_t &f0io, int32_t &f1io, uint32_t shift, int16_t *out, uint32_t n) {
	const uint16_t *wa = WAVES.w[c.wave[a]], *wb = WAVES.w[c.wave[b]];
	HV_KEEP(wa); HV_KEEP(wb);
	const uint32_t ia = c.inc[a], ib = c.inc[b], aa = c.att[a], ab = c.att[b];
	uint32_t p = pha, q = phb;
	int32_t f0 = f0io, f1 = f1io;
	const uint32_t on = shift < 16 ? ~0u : 0u;
	HV_UNROLL2
	for (uint32_t j = 0; j < n; ++j) {
		p += ia;
		q += ib;
		const int32_t v = op_out(wa, (p >> 22) + (uint32_t((f0 + f1) >> shift) & on), aa);
		f1 = f0;
		f0 = v;
		out[j] = int16_t(v + op_out(wb, q >> 22, ab));
	}
	pha = p;
	phb = q;
	f0io = f0;
	f1io = f1;
}
#endif

#ifndef HV_RVV
// Op 1 -> op 2, no feedback.
void k_fm(const Chip &c, uint32_t a, uint32_t b, uint32_t &pha, uint32_t &phb, int16_t *out, uint32_t n) {
	const uint16_t *wa = WAVES.w[c.wave[a]], *wb = WAVES.w[c.wave[b]];
	HV_KEEP(wa); HV_KEEP(wb);
	const uint32_t ia = c.inc[a], ib = c.inc[b], aa = c.att[a], ab = c.att[b];
	uint32_t p = pha, q = phb;
	for (uint32_t j = 0; j < n; ++j) {
		p += ia;
		q += ib;
		const int32_t m = op_out(wa, p >> 22, aa);
		out[j] = int16_t(op_out(wb, (q >> 22) + uint32_t(m), ab));
	}
	pha = p;
	phb = q;
}

// Op 1 with feedback -> op 2, at 96 kHz.
void k_fm_fb96(Chip &c, uint32_t a, uint32_t b, uint32_t ch, uint32_t shift, int16_t *out, uint32_t n) {
	const uint16_t *wa = WAVES.w[c.wave[a]], *wb = WAVES.w[c.wave[b]];
	HV_KEEP(wa); HV_KEEP(wb);
	const uint32_t ia = c.inc[a], ib = c.inc[b], aa = c.att[a], ab = c.att[b];
	uint32_t p = c.ph[a], q = c.ph[b];
	int32_t y1 = c.fb0[ch], y2 = c.fb1[ch], y3 = c.fb2[ch], y4 = c.fb3[ch];
	for (uint32_t j = 0; j < n; ++j) {
		p += ia;
		q += ib;
		const int32_t m = op_out(wa, (p >> 22) + uint32_t((y2 + y4) >> shift), aa);
		y4 = y3; y3 = y2; y2 = y1; y1 = m;
		out[j] = int16_t(op_out(wb, (q >> 22) + uint32_t(m), ab));
	}
	c.ph[a] = p; c.ph[b] = q;
	c.fb0[ch] = int16_t(y1); c.fb1[ch] = int16_t(y2); c.fb2[ch] = int16_t(y3); c.fb3[ch] = int16_t(y4);
}

// Op 1 with feedback -> op 2.
void k_fm_fb(const Chip &c, uint32_t a, uint32_t b, uint32_t &pha, uint32_t &phb, int32_t &f0io, int32_t &f1io, uint32_t shift, int16_t *out, uint32_t n) {
	const uint16_t *wa = WAVES.w[c.wave[a]], *wb = WAVES.w[c.wave[b]];
	HV_KEEP(wa); HV_KEEP(wb);
	const uint32_t ia = c.inc[a], ib = c.inc[b], aa = c.att[a], ab = c.att[b];
	uint32_t p = pha, q = phb;
	int32_t f0 = f0io, f1 = f1io;
	HV_UNROLL2
	for (uint32_t j = 0; j < n; ++j) {
		p += ia;
		q += ib;
		const int32_t m = op_out(wa, (p >> 22) + uint32_t((f0 + f1) >> shift), aa);
		f1 = f0;
		f0 = m;
		out[j] = int16_t(op_out(wb, (q >> 22) + uint32_t(m), ab));
	}
	pha = p;
	phb = q;
	f0io = f0;
	f1io = f1;
}

#else
#include "hvopl3_rvv.inc"
#endif  // !HV_RVV

HV_INLINE void add_row(int16_t *out, const int16_t *x, uint32_t n) {
	for (uint32_t j = 0; j < n; ++j) out[j] = int16_t(out[j] + x[j]);
}

// Four operators: the general chain, a connection a template.
template <uint32_t CONN>
void k_4op(Chip &c, const uint8_t *o, uint32_t ch, int16_t *out, uint32_t n) {
	const uint16_t *w0 = WAVES.w[c.wave[o[0]]], *w1 = WAVES.w[c.wave[o[1]]], *w2 = WAVES.w[c.wave[o[2]]], *w3 = WAVES.w[c.wave[o[3]]];
	HV_KEEP(w0); HV_KEEP(w1); HV_KEEP(w2); HV_KEEP(w3);
	const uint32_t i0 = c.inc[o[0]], i1 = c.inc[o[1]], i2 = c.inc[o[2]], i3 = c.inc[o[3]];
	const uint32_t a0 = c.att[o[0]], a1 = c.att[o[1]], a2 = c.att[o[2]], a3 = c.att[o[3]];
	uint32_t p0 = c.ph[o[0]], p1 = c.ph[o[1]], p2 = c.ph[o[2]], p3 = c.ph[o[3]];
	int32_t y1 = c.fb0[ch], y2 = c.fb1[ch], y3 = c.fb2[ch], y4 = c.fb3[ch];
	const uint32_t shift = c.fb_shift[ch];
	const bool r96 = c.fb_step == 2;
	const uint32_t on = shift < 16 ? ~0u : 0u;
	for (uint32_t j = 0; j < n; ++j) {
		p0 += i0; p1 += i1; p2 += i2; p3 += i3;
		const int32_t v0 = op_out(w0, (p0 >> 22) + (uint32_t(((r96 ? y2 + y4 : y1 + y2)) >> shift) & on), a0);
		y4 = y3; y3 = y2; y2 = y1; y1 = v0;
		int32_t r;
		if (CONN == 0) {         // 1 -> 2 -> 3 -> 4
			const int32_t v1 = op_out(w1, (p1 >> 22) + uint32_t(v0), a1);
			const int32_t v2 = op_out(w2, (p2 >> 22) + uint32_t(v1), a2);
			r = op_out(w3, (p3 >> 22) + uint32_t(v2), a3);
		} else if (CONN == 1) {  // 1 + (2 -> 3 -> 4)
			const int32_t v1 = op_out(w1, p1 >> 22, a1);
			const int32_t v2 = op_out(w2, (p2 >> 22) + uint32_t(v1), a2);
			r = v0 + op_out(w3, (p3 >> 22) + uint32_t(v2), a3);
		} else if (CONN == 2) {  // (1 -> 2) + (3 -> 4)
			const int32_t v1 = op_out(w1, (p1 >> 22) + uint32_t(v0), a1);
			const int32_t v2 = op_out(w2, p2 >> 22, a2);
			r = v1 + op_out(w3, (p3 >> 22) + uint32_t(v2), a3);
		} else {                 // 1 + (2 -> 3) + 4
			const int32_t v1 = op_out(w1, p1 >> 22, a1);
			const int32_t v2 = op_out(w2, (p2 >> 22) + uint32_t(v1), a2);
			r = v0 + v2 + op_out(w3, p3 >> 22, a3);
		}
		out[j] = int16_t(r);
	}
	c.ph[o[0]] = p0; c.ph[o[1]] = p1; c.ph[o[2]] = p2; c.ph[o[3]] = p3;
	c.fb0[ch] = int16_t(y1); c.fb1[ch] = int16_t(y2); c.fb2[ch] = int16_t(y3); c.fb3[ch] = int16_t(y4);
}

// One sample's clocking of the rhythm pair: the noise on the chip's ticks, the four phases, and the phase
// bits the drums are made of. The chip-mode and high-quality kernels share it.
struct RhythmStep { uint32_t sel, hsel, nz, p13; };
HV_INLINE RhythmStep rhythm_step(Chip &c, uint32_t &f, uint32_t &lfsr) {
	const uint64_t a = uint64_t(f) + c.r_frac;
	f = uint32_t(a);
	for (uint32_t t = c.r_int + uint32_t(a >> 32); t; --t) {
		lfsr <<= 1;
		lfsr |= ((lfsr >> 23) ^ (lfsr >> 9) ^ (lfsr >> 8) ^ (lfsr >> 1)) & 1;
	}
	c.ph[14] += c.inc[14]; c.ph[15] += c.inc[15]; c.ph[16] += c.inc[16]; c.ph[17] += c.inc[17];
	const uint32_t p13 = c.ph[14] >> 22, p17 = c.ph[17] >> 22;
	RhythmStep r;
	r.sel = (((p13 >> 2) ^ (p13 >> 7)) | ((p13 >> 3) ^ (p17 >> 5)) | ((p17 >> 5) ^ (p17 >> 3))) & 1;
	// the hi-hat reads the selector a slot apart from the cymbal: its own phase already a sample on
	const uint32_t ph13 = (c.ph[14] + c.inc[14]) >> 22;
	r.hsel = (((ph13 >> 2) ^ (ph13 >> 7)) | ((ph13 >> 3) ^ (p17 >> 5)) | ((p17 >> 5) ^ (p17 >> 3))) & 1;
	r.nz = ~(lfsr >> 23) & 1;
	r.p13 = p13;
	return r;
}
// The table index each drum reads, from those bits.
HV_INLINE uint32_t hihat_index(const RhythmStep &r) { return r.hsel << 9 | (0xD0u >> (2 * (r.nz ^ r.hsel))); }
HV_INLINE uint32_t snare_index(const RhythmStep &r) { return (0x100u << ((r.p13 >> 8) & 1)) ^ (r.nz << 8); }
HV_INLINE uint32_t cymbal_index(const RhythmStep &r) { return 0x80u | r.sel << 9; }

// The rhythm pair: hi-hat + snare (channel 7), tom + cymbal (channel 8).
void k_rhythm(Chip &c, uint32_t at, int16_t *o7, int16_t *o8, uint32_t n) {
	const uint16_t *wh = WAVES.w[c.wave[14]], *ws = WAVES.w[c.wave[15]], *wt = WAVES.w[c.wave[16]], *wc = WAVES.w[c.wave[17]];
	HV_KEEP(wh); HV_KEEP(ws); HV_KEEP(wt); HV_KEEP(wc);
	const uint32_t ah = c.att[14], as = c.att[15], at_ = c.att[16], ac = c.att[17];
	uint32_t ti, f;
	pos_at(c, at, ti, f);
	uint32_t lfsr = c.noise;
	for (uint32_t j = 0; j < n; ++j) {
		const RhythmStep r = rhythm_step(c, f, lfsr);
		const int32_t hh = ah >= QUIET ? 0 : op_out(wh, hihat_index(r), ah);
		const int32_t sd = as >= QUIET ? 0 : op_out(ws, snare_index(r), as);
		const int32_t tt = at_ >= QUIET ? 0 : op_out(wt, c.ph[16] >> 22, at_);
		const int32_t tc = ac >= QUIET ? 0 : op_out(wc, cymbal_index(r), ac);
		o7[j] = int16_t(hh + sd);
		o8[j] = int16_t(tt + tc);
	}
	c.noise = lfsr;
	c.rhy_at = at + n;
}

HV_INLINE void advance_op(Chip &c, uint32_t i, uint32_t n) { c.ph[i] += c.inc[i] * n; }

// One segment of a two-operator voice (or the bass drum), the kernel chosen by what can be heard.
// Returns false: silence, nothing written.
bool seg_2op_fb96(Chip &c, uint32_t ch, int16_t *out, uint32_t n);

bool seg_2op(Chip &c, uint32_t ch, int16_t *out, uint32_t n) {
	if (c.fb_step == 2 && c.fb_shift[ch] < 16 && c.att[2 * ch] < QUIET && seg_2op_fb96(c, ch, out, n)) return true;
	const uint32_t a = 2 * ch, b = a + 1;
	const bool qa = c.att[a] >= QUIET, qb = c.att[b] >= QUIET;
	const uint32_t cnt = c.r_c0[ch] & 1;
	const uint32_t shift = c.fb_shift[ch];
	int32_t f0 = c.fb0[ch], f1 = c.fb1[ch];
	bool sounded = true;
	if (cnt == 0 || c.kind[ch] == K_BD) {
		if (qb) {
			advance_op(c, b, n);
			if (qa) { advance_op(c, a, n); f0 = f1 = 0; }
			else {   // heard by nothing, but its feedback runs: it may be heard later
				int16_t sink[BLOCK];
				k_fb(c, a, c.ph[a], f0, f1, shift, sink, n);
			}
			sounded = false;
		} else if (qa || (cnt && c.kind[ch] == K_BD)) {
			if (qa) { f0 = f1 = 0; advance_op(c, a, n); }
			else { int16_t sink[BLOCK]; k_fb(c, a, c.ph[a], f0, f1, shift, sink, n); }
			k_one(c, b, c.ph[b], out, n);
		} else if (shift < 16) {
			k_fm_fb(c, a, b, c.ph[a], c.ph[b], f0, f1, shift, out, n);
		} else {
			k_fm(c, a, b, c.ph[a], c.ph[b], out, n);
			f0 = f1 = 0;
		}
	} else {
		if (qa && qb) {
			advance_op(c, a, n); advance_op(c, b, n);
			f0 = f1 = 0;
			sounded = false;
		} else if (qa) {
			advance_op(c, a, n);
			f0 = f1 = 0;
			k_one(c, b, c.ph[b], out, n);
		} else {
#ifdef HV_DSP
			if (!qb) k_add(c, a, b, c.ph[a], c.ph[b], f0, f1, shift, out, n);
			else { k_fb(c, a, c.ph[a], f0, f1, shift, out, n); advance_op(c, b, n); }
#else
			k_fb(c, a, c.ph[a], f0, f1, shift, out, n);
			if (qb) advance_op(c, b, n);
			else {
				int16_t tmp[BLOCK];
				k_one(c, b, c.ph[b], tmp, n);
				add_row(out, tmp, n);
			}
#endif
		}
	}
	c.fb0[ch] = int16_t(f0);
	c.fb1[ch] = int16_t(f1);
	if (qa) silent_history(c, a, ch);
	return sounded;
}

// The common feedback cases at 96 kHz. False: not one of them, seg_2op goes on.
bool seg_2op_fb96(Chip &c, uint32_t ch, int16_t *out, uint32_t n) {
	const uint32_t a = 2 * ch, b = a + 1, shift = c.fb_shift[ch];
	const bool qb = c.att[b] >= QUIET;
	const uint32_t cnt = c.r_c0[ch] & 1;
	if (c.kind[ch] == K_BD && cnt) return false;
	if (cnt == 0) {
		if (qb) return false;
		k_fm_fb96(c, a, b, ch, shift, out, n);
		return true;
	}
	k_fb96(c, a, ch, shift, out, n);
	if (qb) advance_op(c, b, n);
	else {
		int16_t tmp[BLOCK];
		k_one(c, b, c.ph[b], tmp, n);
		add_row(out, tmp, n);
	}
	return true;
}

bool seg_4op(Chip &c, uint32_t ch, int16_t *out, uint32_t n) {
	const uint8_t o[4] = {uint8_t(2 * ch), uint8_t(2 * ch + 1), uint8_t(2 * ch + 6), uint8_t(2 * ch + 7)};
	const uint32_t conn = (c.r_c0[ch] & 1) | (c.r_c0[ch + 3] & 1) << 1;
	// which operators reach the output, per connection: silent if all of those are
	static constexpr uint8_t HEARD[4] = {0x8, 0x9, 0xA, 0xD};
	bool quiet = true;
	for (uint32_t j = 0; j < 4; ++j)
		if ((HEARD[conn] >> j & 1) && c.att[o[j]] < QUIET) quiet = false;
	if (quiet) {
		for (uint32_t j = 0; j < 4; ++j) advance_op(c, o[j], n);
		return false;
	}
	switch (conn) {
	case 0: k_4op<0>(c, o, ch, out, n); break;
	case 1: k_4op<1>(c, o, ch, out, n); break;
	case 2: k_4op<2>(c, o, ch, out, n); break;
	default: k_4op<3>(c, o, ch, out, n); break;
	}
	return true;
}

// ---- high quality: single precision, no tables, on every target ----------------------------------
// Polynomials rather than table lookups (a core that cannot read a table can run them too); one
// generic kernel, since quality is what is paid for here. Conversions truncate: no rounding-mode
// changes. Build with -ffp-contract=off where one target must match another.

// 2^x for x in [0, 1).
HV_INLINE float exp2_frac(float x) {
	return 1.0f + x * (0.69315462f + x * (0.24014077f + x * (0.05586328f + x * (0.00894621f + x * 0.00189511f))));
}
// The chip's law: an attenuation in log-sine units -> linear, 4096-ish at 0.
HV_INLINE float atten_gain(float a) {
	const int32_t w = int32_t(a);                     // a >= 0
	const float f = a - float(w);
	const int32_t sh = w >> 8;
	if (sh > 24) return 0.0f;
	const float m = exp2_frac(1.0f - (float(w & 255) + f) / 256.0f);   // in (1, 2]
	union { float v; uint32_t u; } p;
	p.u = uint32_t(127 + 11 - sh) << 23;              // 2^(11 - sh)
	return m * p.v;
}
// sin(2 pi x) for x in [0, 1).
HV_INLINE float sin_cycle(float x) {
	const float h = x < 0.5f ? x : x - 0.5f;          // [0, 0.5): the positive half
	const float q = h < 0.25f ? h : 0.5f - h;         // [0, 0.25]
	const float t = q * 4.0f, t2 = t * t;
	const float v = t * (1.57079632f + t2 * (-0.64596371f + t2 * (0.07968968f + t2 * (-0.00467377f + t2 * 0.00015148f))));
	return x < 0.5f ? v : -v;
}
HV_INLINE float frac_cycle(float x) {
	const float t = float(int32_t(x));
	const float f = x - t;
	return f < 0.0f ? f + 1.0f : f;
}
// The waveform at phase x (cycles), on a -1..1 scale.
HV_INLINE float wave_at(uint32_t ws, float x) {
	switch (ws) {
	case 0: return sin_cycle(x);
	case 1: return x < 0.5f ? sin_cycle(x) : 0.0f;
	case 2: { const float s = sin_cycle(x); return s < 0.0f ? -s : s; }
	case 3: { const float h = x < 0.5f ? x : x - 0.5f; if (h >= 0.25f) return 0.0f; return sin_cycle(h); }
	case 4: return x < 0.5f ? sin_cycle(x * 2.0f) : 0.0f;
	case 5: { if (x >= 0.5f) return 0.0f; const float d = x * 2.0f; const float s = sin_cycle(d); return s < 0.0f ? -s : s; }
	case 6: return x < 0.5f ? 1.0f : -1.0f;
	default: {   // a falling saw in the log domain: 8 log-sine units per 1/1024 of a cycle
		const float d = x < 0.5f ? x : 1.0f - x;
		const float g = atten_gain(d * 8192.0f) / 4096.0f;
		return x < 0.5f ? g : -g;
	}
	}
}

// The attenuation an operator will have after its next envelope step.
float next_att(const Chip &c, uint32_t i) {
	if (!c.pending[i]) return float(c.att[i]);
	const uint32_t rate = state_rate(c, i), rs = rate >> 2;
	const uint32_t shift = (rs < 11 ? 11 - rs : 0) + 1;
	const uint32_t inc = rate_step(rate, (c.next_tick[i] >> shift) & 7);
	int32_t e = c.env[i] >> 1;
	if (c.state[i] == ATTACK) {
		if (rate < 62) e += (~e * int32_t(inc)) >> 3;
		if (e < 0) e = 0;
	} else {
		e += int32_t(inc);
		if (e > 511) e = 511;
	}
	uint32_t a = uint32_t(e << 1) + c.base[i];
	if (c.r_20[i] & 0x80) a += c.am;
	if (a > ENV_MAX) a = ENV_MAX;
	return float(a << 2);
}

// HQ_ENVELOPE: an operator's gain is a closed-form line from its last change: from + slope x (sample - t0).
// It is redrawn only when the envelope or level changes, so no cut between calls can change a sample.
struct GainLine { float from, slope, d; };
GainLine gain_line(Chip &c, uint32_t i, uint32_t at, bool quiet_is_zero) {
	const uint32_t next = c.pending[i] ? c.next_tick[i] : 0xFFFFFFFFu;
	if (next != c.hq_tick[i] || c.att[i] != c.hq_att[i]) {
		const float now = c.hq_gain[i] + c.hq_slope[i] * float(at - c.hq_t0[i]);
		const float to = next_att(c, i);
		const float target = (quiet_is_zero && to >= float(QUIET)) ? 0.0f : atten_gain(to);
		c.hq_tick[i] = next;
		c.hq_att[i] = c.att[i];
		c.hq_t0[i] = at;
		c.hq_gain[i] = c.pending[i] ? now : target;
		c.hq_slope[i] = c.pending[i] ? (target - now) / float(samples_before(c, at, c.next_tick[i]) + 1) : 0.0f;
	}
	GainLine l = {c.hq_gain[i], c.hq_slope[i], float(at - c.hq_t0[i])};
	return l;
}

// A voice's operators in chain order, whom each is modulated by, and who is heard.
struct Route { int8_t mod[4]; uint8_t heard; };
HV_INLINE Route voice_route(const Chip &c, uint32_t ch, uint32_t nops) {
	Route r = {{-1, -1, -1, -1}, 0};
	if (nops == 2) {
		const bool cnt = c.r_c0[ch] & 1;
		if (c.kind[ch] == K_BD) { r.heard = 2; if (!cnt) r.mod[1] = 0; }
		else if (cnt) r.heard = 3;
		else { r.heard = 2; r.mod[1] = 0; }
		return r;
	}
	switch ((c.r_c0[ch] & 1) | (c.r_c0[ch + 3] & 1) << 1) {
	case 0: r.mod[1] = 0; r.mod[2] = 1; r.mod[3] = 2; r.heard = 8; break;   // 1 -> 2 -> 3 -> 4
	case 1: r.mod[2] = 1; r.mod[3] = 2; r.heard = 9; break;                 // 1 + (2 -> 3 -> 4)
	case 2: r.mod[1] = 0; r.mod[3] = 2; r.heard = 10; break;                // (1 -> 2) + (3 -> 4)
	default: r.mod[2] = 1; r.heard = 13; break;                             // 1 + (2 -> 3) + 4
	}
	return r;
}

bool seg_hq(Chip &c, uint32_t ch, uint32_t at, int16_t *out, uint32_t n) {
	const OpList l = voice_ops(c, ch);
	const Route route = voice_route(c, ch, l.n);
	const bool hq_phase = c.flags & HQ_PHASE, hq_env = c.flags & HQ_ENVELOPE, hq_mod = c.flags & HQ_MODULATION;
	bool quiet = true;
	for (uint32_t j = 0; j < l.n; ++j)
		if ((route.heard >> j & 1) && c.att[l.o[j]] < QUIET) quiet = false;
	GainLine g[4];
	for (uint32_t j = 0; j < l.n; ++j) {
		const uint32_t i = l.o[j];
		if (hq_env) g[j] = gain_line(c, i, at, false);
		else { g[j].from = atten_gain(float(c.att[i])); g[j].slope = 0.0f; g[j].d = 0.0f; }
	}
	if (quiet) {
		for (uint32_t j = 0; j < l.n; ++j) {
			advance_op(c, l.o[j], n);
		}
		c.hq_fb0[ch] = c.hq_fb1[ch] = c.hq_fb2[ch] = c.hq_fb3[ch] = 0.0f;
		return false;
	}
	const uint32_t fbs = c.fb_shift[ch];
	const float fbk = fbs < 16 ? 1.0f / float(1u << fbs) / 1024.0f : 0.0f;
	float f0 = c.hq_fb0[ch], f1 = c.hq_fb1[ch], f2 = c.hq_fb2[ch], f3 = c.hq_fb3[ch];
	const bool r96 = c.fb_step == 2;
	for (uint32_t k = 0; k < n; ++k) {
		float v[4], sum = 0.0f;
		for (uint32_t j = 0; j < l.n; ++j) {
			const uint32_t i = l.o[j];
			c.ph[i] += c.inc[i];
			float m = 0.0f;                               // in cycles
			if (j == 0 && fbs < 16) {
				const float a = r96 ? f1 : f0, b = r96 ? f3 : f1;
				m = hq_mod ? (a + b) * fbk : float((int32_t(a) + int32_t(b)) >> fbs) / 1024.0f;
			}
			else if (route.mod[j] >= 0) m = (hq_mod ? v[route.mod[j]] : float(int32_t(v[route.mod[j]]))) / 1024.0f;
			float w;
			if (hq_phase) {
				w = wave_at(c.wave[i], frac_cycle(float(c.ph[i] >> 8) * (1.0f / 16777216.0f) + m));
			} else {
				const uint32_t idx = uint32_t(int32_t(frac_cycle(float(c.ph[i] >> 22) / 1024.0f + m) * 1024.0f));
				const uint16_t e = WAVES.w[c.wave[i]][idx & 1023];
				const float a = atten_gain(float(e & 0x7FFF)) / 4096.0f;
				w = (e & 0x8000) ? -a : a;
			}
			v[j] = w * (g[j].from + g[j].slope * g[j].d);
			g[j].d += 1.0f;
			if (route.heard >> j & 1) sum += v[j];
		}
		f3 = f2; f2 = f1; f1 = f0;
		f0 = v[0];
		const float r = sum < 0.0f ? sum - 0.5f : sum + 0.5f;
		out[k] = int16_t(int32_t(r));
	}
	c.hq_fb0[ch] = f0; c.hq_fb1[ch] = f1; c.hq_fb2[ch] = f2; c.hq_fb3[ch] = f3;
	return true;
}

// The rhythm pair in high quality. The drums' phases stay the chip's selections: that is what makes them
// these drums. HQ_PHASE reads each at its exact place (the tom at its whole phase) instead of through the
// table; HQ_ENVELOPE makes their gains continuous. They have no modulation or feedback for the third flag.
void k_rhythm_hq(Chip &c, uint32_t at, int16_t *o7, int16_t *o8, uint32_t n) {
	const bool hq_phase = c.flags & HQ_PHASE, hq_env = c.flags & HQ_ENVELOPE;
	GainLine g[4];
	for (uint32_t j = 0; j < 4; ++j) {
		const uint32_t i = 14 + j;
		if (hq_env) g[j] = gain_line(c, i, at, true);
		else { g[j].from = c.att[i] >= QUIET ? 0.0f : atten_gain(float(c.att[i])); g[j].slope = 0.0f; g[j].d = 0.0f; }
	}
	const auto gain_of = [](const GainLine &x) -> float { return x.from + x.slope * x.d; };
	const auto read = [&c, hq_phase](uint32_t i, uint32_t idx) -> float {
		if (hq_phase) return wave_at(c.wave[i], (float(idx & 1023) + 0.5f) / 1024.0f);
		const uint16_t e = WAVES.w[c.wave[i]][idx & 1023];
		const float a = atten_gain(float(e & 0x7FFF)) / 4096.0f;
		return (e & 0x8000) ? -a : a;
	};
	uint32_t ti, f;
	pos_at(c, at, ti, f);
	uint32_t lfsr = c.noise;
	for (uint32_t k = 0; k < n; ++k) {
		const RhythmStep r = rhythm_step(c, f, lfsr);
		const float tom = hq_phase ? wave_at(c.wave[16], float(c.ph[16] >> 8) * (1.0f / 16777216.0f)) : read(16, c.ph[16] >> 22);
		const float v7 = read(14, hihat_index(r)) * gain_of(g[0]) + read(15, snare_index(r)) * gain_of(g[1]);
		const float v8 = tom * gain_of(g[2]) + read(17, cymbal_index(r)) * gain_of(g[3]);
		for (uint32_t j = 0; j < 4; ++j) g[j].d += 1.0f;
		o7[k] = int16_t(int32_t(v7 < 0.0f ? v7 - 0.5f : v7 + 0.5f));
		o8[k] = int16_t(int32_t(v8 < 0.0f ? v8 - 0.5f : v8 + 0.5f));
	}
	c.noise = lfsr;
	c.rhy_at = at + n;
}

// ---- the mix ------------------------------------------------------------------------------------
//
// Each output is the exact sum of the voices' samples times their coefficients, rounded once:
//   dry (l, r)   (sum of s x c + 2^11) >> 12, then clipped to 16 bits (unless NO_CLIP); c the voice's Q12
//                coefficient (pan, gain, its C0 bits)
//   a bus (l, r) (sum of s x q + 2^13) >> 14, q = (c x send + 2^9) >> 10 the combined Q14 coefficient
//                (send Q12)
// Every target gives these bits. The fast path keeps each sum in 32 bits, where mix_bound shows it cannot
// overflow; everywhere else the slow path splits each coefficient at 2^12 (c = hi 2^12 + lo) and keeps the
// two sums apart, each within 32 bits (a row is at most three carriers of 4096 and there are at most 18
// rows: 18 x 12288 x 4095 < 2^31), then puts them together exactly.

HV_INLINE int32_t send_coef(int32_t c, int32_t q) { return (c * q + 512) >> 10; }   // c < 2^18.1, q <= 2^12
// A voice's carriers: the operators its row adds (each at most 4096 in size: 4085 in chip mode, POW's
// largest negated; 4096 in high quality).
HV_INLINE uint32_t carriers(const Chip &c, uint32_t ch) {
	switch (c.kind[ch]) {
	case K_2OP: return (c.r_c0[ch] & 1) + 1;               // FM: one; additive: two
	case K_4OP: { static const uint8_t n[4] = {1, 2, 2, 3}; return n[(c.r_c0[ch] & 1) | (c.r_c0[ch + 3] & 1) << 1]; }
	case K_RHY: return 2;                                  // each row two drums' operators
	default: return 1;                                     // the bass drum: its carrier only
	}
}
// The fast path's bound for the voices of `mask` (the rhythm pair's second row is channel 8's): every
// coefficient, dry and combined send, below 2^15 (the A25's and RVV's 16-bit halves) and, for each of the
// six outputs, the sum of coefficient x carriers below 2^19, so |sum of s x coefficient| < 2^12 x 2^19.
bool mix_bound(const Chip &c, bool rev, bool cho, uint32_t mask) {
	uint32_t s[6] = {0, 0, 0, 0, 0, 0};
	for (uint32_t m = mask; m; m &= m - 1) {
		const uint32_t ch = lowest_bit(m), nc = carriers(c, ch);
		for (uint32_t k = 0; k < (c.kind[ch] == K_RHY ? 2u : 1u); ++k) {
			const uint32_t x = k ? 8 : ch;
			const int32_t l = c.coef_l[x], r = c.coef_r[x], q = rev ? c.rev_q[x] : 0, g = cho ? c.cho_q[x] : 0;
			const uint32_t v[6] = {uint32_t(l), uint32_t(r), uint32_t(send_coef(l, q)), uint32_t(send_coef(r, q)),
			                       uint32_t(send_coef(l, g)), uint32_t(send_coef(r, g))};
			for (uint32_t i = 0; i < 6; ++i) { if (v[i] > 32767) return false; s[i] += v[i] * nc; }
		}
	}
	for (uint32_t i = 0; i < 6; ++i) if (s[i] >= 524288) return false;
	return true;
}
// Whether the block takes the fast path: the bound once over every channel and both buses (kept until a
// coefficient changes), and only where that fails, over the voices sounding and the buses given.
bool mix_fast(Chip &c, const Output &o, uint32_t mask) {
	if (c.flags & MIX_SLOW) return false;
	if (!c.mix_bound) {
		uint32_t all = 0;
		for (uint32_t ch = 0; ch < CHANNELS; ++ch) if (c.kind[ch] != K_NONE) all |= 1u << ch;
		c.mix_bound = mix_bound(c, true, true, all) ? 1 : 2;
	}
	return c.mix_bound == 1 || mix_bound(c, o.rev, o.cho, mask);
}

// The slow path: a row into a sum split at 2^12 (lo, hi); `first` sets the sums instead of adding.
HV_INLINE void slow_sum(int32_t *lo, int32_t *hi, const int16_t *row, int32_t c, uint32_t n, bool first) {
	const int32_t h = c >> 12, l = c & 4095;
	if (first) for (uint32_t j = 0; j < n; ++j) { lo[j] = row[j] * l; hi[j] = row[j] * h; }
	else for (uint32_t j = 0; j < n; ++j) { lo[j] += row[j] * l; hi[j] += row[j] * h; }
}
// bus (in/out): the buses with sums this block (1 reverb, 2 chorus); a bus's first row sets its sums.
void mix_row_slow(Chip &c, const Output &o, uint32_t ch, const int16_t *row, uint32_t n, uint32_t &bus) {
	const int32_t cl = c.coef_l[ch], cr = c.coef_r[ch];
	slow_sum(c.acc_l, c.mix_hi[0], row, cl, n, false);
	slow_sum(c.acc_r, c.mix_hi[1], row, cr, n, false);
	for (uint32_t k = 0; k < 2; ++k) {
		const int32_t q = k ? (o.cho ? c.cho_q[ch] : 0) : (o.rev ? c.rev_q[ch] : 0);
		if (!q) continue;
		const bool first = !(bus >> k & 1);
		slow_sum(c.bus_acc[2 * k], c.mix_hi[2 + 2 * k], row, send_coef(cl, q), n, first);
		slow_sum(c.bus_acc[2 * k + 1], c.mix_hi[3 + 2 * k], row, send_coef(cr, q), n, first);
		bus |= 1u << k;
	}
}
HV_INLINE int32_t clip16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
// hi x 2^12 + lo, rounded from Q12: hi + round(lo / 2^12), exactly
void dry_out_slow(const Chip &c, int32_t *dry, bool add, uint32_t n) {
	const bool clip = !(c.flags & NO_CLIP);
	for (uint32_t j = 0; j < n; ++j) {
		int32_t l = c.mix_hi[0][j] + ((c.acc_l[j] + 2048) >> 12), r = c.mix_hi[1][j] + ((c.acc_r[j] + 2048) >> 12);
		if (clip) { l = clip16(l); r = clip16(r); }
		dry[2 * j] = add ? dry[2 * j] + l : l;
		dry[2 * j + 1] = add ? dry[2 * j + 1] + r : r;
	}
}
// hi x 2^12 + lo, rounded from Q14: with hi = 4 h + r (r = hi & 3), h + round((r 2^12 + lo) / 2^14), exactly
HV_INLINE int32_t slow_q14(int32_t hi, int32_t lo) { return (hi >> 2) + ((((hi & 3) << 12) + lo + 8192) >> 14); }
void bus_out_slow(const Chip &c, const Output &o, uint32_t off, uint32_t bus, uint32_t n) {
	for (uint32_t k = 0; k < 2; ++k) {
		if (!(bus >> k & 1)) continue;
		int32_t *b = (k ? o.cho : o.rev) + 2 * off;
		for (uint32_t j = 0; j < n; ++j) {
			b[2 * j] += slow_q14(c.mix_hi[2 + 2 * k][j], c.bus_acc[2 * k][j]);
			b[2 * j + 1] += slow_q14(c.mix_hi[3 + 2 * k][j], c.bus_acc[2 * k + 1][j]);
		}
	}
}

// The fast path, scalar (RVV's is in hvopl3_rvv.inc; the A25 mixes its own way, hvopl3_dsp.inc).
#if defined(HV_RVV)
#define HV_RVV_MIX 1
#include "hvopl3_rvv.inc"
#undef HV_RVV_MIX
#else
#if !defined(HV_DSP)
// One bus's two sums (k: 0 reverb, 1 chorus): set at its first row of the block, added to after.
HV_INLINE void bus_sum(Chip &c, uint32_t k, uint32_t &bus, const int16_t *row, int32_t ql, int32_t qr, uint32_t n) {
	int32_t *bl = c.bus_acc[2 * k], *br = c.bus_acc[2 * k + 1];
	if (!(bus >> k & 1)) {
		for (uint32_t j = 0; j < n; ++j) { bl[j] = row[j] * ql; br[j] = row[j] * qr; }
		bus |= 1u << k;
	} else {
		for (uint32_t j = 0; j < n; ++j) { bl[j] += row[j] * ql; br[j] += row[j] * qr; }
	}
}
void mix_row(Chip &c, const Output &o, uint32_t ch, const int16_t *row, uint32_t n, uint32_t &bus) {
	const int32_t cl = c.coef_l[ch], cr = c.coef_r[ch];
	int32_t *al = c.acc_l, *ar = c.acc_r;
	for (uint32_t j = 0; j < n; ++j) { al[j] += row[j] * cl; ar[j] += row[j] * cr; }
	if (o.rev && c.rev_q[ch]) bus_sum(c, 0, bus, row, send_coef(cl, c.rev_q[ch]), send_coef(cr, c.rev_q[ch]), n);
	if (o.cho && c.cho_q[ch]) bus_sum(c, 1, bus, row, send_coef(cl, c.cho_q[ch]), send_coef(cr, c.cho_q[ch]), n);
}
#endif
void bus_out(const Chip &c, const Output &o, uint32_t off, uint32_t bus, uint32_t n) {
	for (uint32_t k = 0; k < 2; ++k) {
		if (!(bus >> k & 1)) continue;
		int32_t *b = (k ? o.cho : o.rev) + 2 * off;
		const int32_t *bl = c.bus_acc[2 * k], *br = c.bus_acc[2 * k + 1];
		for (uint32_t j = 0; j < n; ++j) { b[2 * j] += (bl[j] + 8192) >> 14; b[2 * j + 1] += (br[j] + 8192) >> 14; }
	}
}
#if defined(HV_DSP)
#include "hvopl3_dsp.inc"
#else
void dry_out(const Chip &c, int32_t *dry, bool add, uint32_t n) {
	const bool clip = !(c.flags & NO_CLIP);
	for (uint32_t j = 0; j < n; ++j) {
		int32_t l = (c.acc_l[j] + 2048) >> 12, r = (c.acc_r[j] + 2048) >> 12;
		if (clip) { l = clip16(l); r = clip16(r); }
		dry[2 * j] = add ? dry[2 * j] + l : l;
		dry[2 * j + 1] = add ? dry[2 * j + 1] + r : r;
	}
}
#endif
#endif  // !HV_RVV

HV_INLINE void zero16(int16_t *p, uint32_t n) { memset(p, 0, n * sizeof *p); }

// A voice's samples from `done` on, into its row (and row8, the rhythm pair's second). Returns whether
// any of the n samples sounded; if not, nothing is written.
bool voice_fill(Chip &c, uint32_t ch, int16_t *row, int16_t *row8, uint32_t n, uint32_t done, bool sounded) {
	const uint8_t kind = c.kind[ch];
	uint32_t at = c.now + done;
	while (done < n) {
		if (int32_t(at - c.next_event[ch]) >= 0) {
			voice_event(c, ch, at);
			if (!(c.active >> ch & 1)) break;
		}
		uint32_t seg = n - done;
		const uint32_t d = c.next_event[ch] - at;
		if (d < seg) seg = d;
		bool s;
		if (kind == K_RHY) {
			if (c.flags & (HQ_PHASE | HQ_ENVELOPE)) k_rhythm_hq(c, at, row + done, row8 + done, seg);
			else k_rhythm(c, at, row + done, row8 + done, seg);
			s = true;
		} else {
			s = (c.flags & HQ_ANY) ? seg_hq(c, ch, at, row + done, seg)
			    : kind == K_4OP ? seg_4op(c, ch, row + done, seg) : seg_2op(c, ch, row + done, seg);
		}
		if (s) {
			if (!sounded) {
				zero16(row, done);
				if (kind == K_RHY) zero16(row8, done);
				sounded = true;
			}
		} else if (sounded) {
			zero16(row + done, seg);
		}
		done += seg;
		at += seg;
	}
	if (sounded && done < n) {
		zero16(row + done, n - done);
		if (kind == K_RHY) zero16(row8 + done, n - done);
	}
	return sounded;
}

// The block's first row: the chip's dry sums (and, on the slow path, their high parts) zeroed.
HV_INLINE void acc_begin(Chip &c, uint32_t n, bool &any, bool fast) {
	if (any) return;
	memset(c.acc_l, 0, n * sizeof(int32_t));
	memset(c.acc_r, 0, n * sizeof(int32_t));
	if (!fast) { memset(c.mix_hi[0], 0, n * sizeof(int32_t)); memset(c.mix_hi[1], 0, n * sizeof(int32_t)); }
	any = true;
}
// A row into the block's sums, by the path the block takes.
HV_INLINE void mix_in(Chip &c, const Output &o, uint32_t ch, const int16_t *row, uint32_t n, uint32_t &bus, bool fast) {
#ifdef HV_DSP
	(void)fast;
	mix_row_slow(c, o, ch, row, n, bus);   // the A25's fast path is its own (dsp_voices)
#else
	fast ? mix_row(c, o, ch, row, n, bus) : mix_row_slow(c, o, ch, row, n, bus);
#endif
}
// A whole voice, for n samples from c.now, mixed in.
void voice_render(Chip &c, const Output &o, uint32_t ch, uint32_t n, bool &any, uint32_t &bus, bool fast) {
	if (!voice_fill(c, ch, c.mono[0], c.mono[1], n, 0, false)) return;
	acc_begin(c, n, any, fast);
	mix_in(c, o, ch, c.mono[0], n, bus, fast);
	if (c.kind[ch] == K_RHY) mix_in(c, o, 8, c.mono[1], n, bus, fast);
}

#ifdef HV_DSP
#define HV_DSP_VOICES 1
#include "hvopl3_dsp.inc"
#undef HV_DSP_VOICES
#endif
#ifdef HV_RVV
#define HV_RVV_LANES 1
#include "hvopl3_rvv.inc"
#undef HV_RVV_LANES
#endif

void advance_time(Chip &c, uint32_t n) {
	uint32_t ti, f;
	pos_at(c, c.now + n, ti, f);
	// In rhythm mode the hi-hat's and cymbal's phases and the noise run whether or not a drum sounds:
	// a drum keyed later takes them as they are. rhy_at: how far the rhythm kernel has taken them.
	if (c.r_bd & 0x20) {
		const uint32_t end = c.now + n, gap = end - c.rhy_at;
		if (gap) {
			for (uint32_t i = 14; i < 18; ++i) c.ph[i] += c.inc[i] * gap;
			uint32_t t0, f0;
			pos_at(c, c.rhy_at, t0, f0);
			uint32_t lfsr = c.noise;
			for (uint32_t t = ti - t0; t; --t) {
				lfsr <<= 1;
				lfsr |= ((lfsr >> 23) ^ (lfsr >> 9) ^ (lfsr >> 8) ^ (lfsr >> 1)) & 1;
			}
			c.noise = lfsr;
		}
		c.rhy_at = end;
	}
	c.tick = ti;
	c.tfrac = f;
	c.now += n;
}

void init_common(Chip &c, uint32_t r_int, uint32_t r_frac, uint32_t rinv, uint32_t k, uint32_t flags) {
	memset(&c, 0, sizeof c);
	c.r_int = r_int; c.r_frac = r_frac; c.rinv_q16 = rinv; c.inc_k = k; c.flags = flags;
	c.fb_step = 1;
	c.noise = 1;
	c.tick = 0u - EG_LAG;
	for (uint32_t i = 0; i < OPERATORS; ++i) {
		c.env[i] = ENV_MAX;
		c.state[i] = RELEASE;
		c.mult2[i] = 1;
		refresh_op(c, i);
		c.att[i] = ENV_MAX << 2;
	}
	for (uint32_t ch = 0; ch < CHANNELS; ++ch) {
		c.pan[ch] = 64;
		c.fb_shift[ch] = 31;
	}
	update_kinds(c);
}

}  // namespace

// Ticks a sample (Q32), samples a tick (Q16), the phase scale (Q31): exact divisions of 14318180 / 288
// by the rate, worked out here so that no target needs a 64-bit division.
static constexpr uint64_t q32_of(uint32_t rate) { return (uint64_t(CHIP_HZ) << 32) / (uint64_t(CHIP_DIV) * rate); }
static constexpr uint32_t rinv_of(uint32_t rate) { return uint32_t(((uint64_t(CHIP_DIV) * rate) << 16) / CHIP_HZ); }
static constexpr uint32_t k_of(uint32_t rate) { return uint32_t((uint64_t(CHIP_HZ) << 31) / (uint64_t(CHIP_DIV) * rate)); }

uint32_t version() { return HVOPL3_VERSION_NUMBER; }
const char *version_string() { return HVOPL3_VERSION; }

void init(Chip &c, Rate rate, uint32_t flags) {
	static constexpr uint64_t Q48 = q32_of(48000), Q96 = q32_of(96000);
	static constexpr uint32_t I48 = rinv_of(48000), I96 = rinv_of(96000), K48 = k_of(48000), K96 = k_of(96000);
	const bool r96 = rate == RATE_96K;
	const uint64_t q = r96 ? Q96 : Q48;
	init_common(c, uint32_t(q >> 32), uint32_t(q), r96 ? I96 : I48, r96 ? K96 : K48, flags);
	c.fb_step = r96 ? 2 : 1;
}

void init_chip_rate(Chip &c, uint32_t flags) { init_common(c, 1, 0, 65536, 0x80000000u, flags); }

void write(Chip &c, uint32_t reg, uint8_t v) {
	HV_LANES_STALE(c);
	const uint32_t bank = (reg >> 8) & 1, low = reg & 0xFF;
	if (bank) {
		if (low == 0x04) {
			const uint8_t was = c.r_104;
			c.r_104 = v & 0x3F;
			if (was != c.r_104) {
				update_kinds(c);
				for (uint32_t i = 0; i < OPERATORS; ++i) refresh_op(c, i);
				for (uint32_t ch = 0; ch < CHANNELS; ++ch) apply_channel_key(c, ch);
			}
			return;
		}
		if (low == 0x05) {
			c.r_105 = v & 1;
			update_kinds(c);
			for (uint32_t i = 0; i < OPERATORS; ++i) refresh_op(c, i);
			return;
		}
	} else {
		if (low == 0x08) {
			c.r_08 = v;
			for (uint32_t i = 0; i < OPERATORS; ++i) { refresh_op(c, i); eg_schedule(c, i, c.tick); }
			for (uint32_t ch = 0; ch < CHANNELS; ++ch) voice_resched(c, ch, c.now, 0);
			return;
		}
		if (low == 0xBD) {
			const uint8_t was = c.r_bd;
			c.r_bd = v;
			c.lfo_next = c.tick;
			if ((was ^ v) & 0x20) {
				c.rhy_at = c.now;
				update_kinds(c);
			}
			apply_drum_keys(c);
			return;
		}
	}
	const uint32_t cls = low & 0xE0;
	if (cls == 0x20 || cls == 0x40 || cls == 0x60 || cls == 0x80 || cls == 0xE0) {
		const int s = SLOT_OP[low & 0x1F];
		if (s < 0) return;
		const uint32_t i = uint32_t(s) + 18 * bank;
		switch (cls) {
		case 0x20: {
			c.r_20[i] = v;
			static constexpr uint8_t MUL[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15};
			c.mult2[i] = MUL[v & 15] ? uint8_t(MUL[v & 15] * 2) : 1;
			const uint32_t vc = voice_of(c, i / 2);
			const OpList l = voice_ops(c, vc);
			uint32_t am = 0, vib = 0;
			for (uint32_t k = 0; k < l.n; ++k) {
				am |= c.r_20[l.o[k]] >> 7 & 1;
				vib |= c.r_20[l.o[k]] >> 6 & 1;
			}
			c.am_chans = (c.am_chans & ~(1u << vc)) | am << vc;
			c.vib_chans = (c.vib_chans & ~(1u << vc)) | vib << vc;
			if ((am || vib)) c.lfo_next = c.tick;
			break;
		}
		case 0x40: c.r_40[i] = v; break;
		case 0x60: c.r_60[i] = v; break;
		case 0x80: c.r_80[i] = v; break;
		case 0xE0: c.wave[i] = uint8_t((c.r_105 & 1) ? v & 7 : v & 3); return;
		}
		refresh_op(c, i);
		op_changed(c, i);
		return;
	}
	if (low >= 0xA0 && low <= 0xA8) {
		const uint32_t ch = low - 0xA0 + 9 * bank;
		c.fnblk[ch] = uint16_t((c.fnblk[ch] & 0x1F00) | v);
	} else if (low >= 0xB0 && low <= 0xB8) {
		const uint32_t ch = low - 0xB0 + 9 * bank;
		c.fnblk[ch] = uint16_t((c.fnblk[ch] & 0xFF) | (v & 0x1F) << 8);
		c.kon[ch] = (v >> 5) & 1;
	} else if (low >= 0xC0 && low <= 0xC8) {
		const uint32_t ch = low - 0xC0 + 9 * bank;
		c.r_c0[ch] = v;
		const uint32_t fb = (v >> 1) & 7;
		c.fb_shift[ch] = uint8_t(fb ? 9 - fb : 31);
		update_coef(c, ch);
		if (PAIR_FIRST[ch] >= 0) update_coef(c, uint32_t(PAIR_FIRST[ch]));
		return;
	} else {
		return;
	}
	// pitch or key: this channel, and its pair's second half if it leads one
	const uint32_t ch = (low & 15) + 9 * bank;
	const bool leads = PAIR_FIRST[ch] < 0 && PAIR_BIT[ch] >= 0 && pair_on(c, ch);
	const uint32_t last = leads ? 2 : 1;
	for (uint32_t x = 0; x < last; ++x) {
		const uint32_t cc = ch + 3 * x;
		for (uint32_t n = 0; n < 2; ++n) {
			refresh_op(c, 2 * cc + n);
			op_changed(c, 2 * cc + n);
		}
		if ((low & 0xF0) == 0xB0) apply_channel_key(c, cc);
	}
}

void set_pan(Chip &c, uint32_t ch, uint8_t pan) { c.pan[ch] = pan & 127; update_coef(c, ch); }
void set_sends(Chip &c, uint32_t ch, uint8_t r, uint8_t s) {   // 127 is exactly the whole voice: Q12
	c.rev_q[ch] = uint16_t(((r & 127u) * 4096 + 63) / 127);
	c.cho_q[ch] = uint16_t(((s & 127u) * 4096 + 63) / 127);
	HV_MIX_STALE(c);
}
void set_gain(Chip &c, uint32_t ch, int8_t level) {
	c.gain[ch] = int8_t(level < -32 ? -32 : level > 32 ? 32 : level);
	update_coef(c, ch);
}
void set_flags(Chip &c, uint32_t flags) {
	HV_LANES_STALE(c);
	if ((flags & HQ_ENVELOPE) && !(c.flags & HQ_ENVELOPE))
		for (uint32_t i = 0; i < OPERATORS; ++i) hq_start(c, i);
	c.flags = flags;
	for (uint32_t ch = 0; ch < CHANNELS; ++ch) update_coef(c, ch);
}

#ifdef HVOPL_TOUCH_FIRST
// A core whose prefetcher follows one stream (the A25): the caller's cold blocks, each alone, before
// a loop that walks several at once.
HV_INLINE void touch(const int32_t *p, uint32_t frames) {
	const volatile char *b = reinterpret_cast<const char *>(p);
	for (uint32_t i = 0; i < frames * 8; i += 32) (void)b[i];
}
#endif

uint32_t render(Chip &c, const Output &o, uint32_t frames) {
#ifdef HVOPL_TOUCH_FIRST
	if (c.active && frames >= 16) {
		touch(o.dry, frames);
		if (o.rev) touch(o.rev, frames);
		if (o.cho) touch(o.cho, frames);
	}
#endif
	uint32_t res = 0, off = 0;
	if (c.key_dirty[0] | c.key_dirty[1]) apply_keys(c);
	while (off < frames) {
		uint32_t n = frames - off;
		if (!c.active) {
			if (!o.add_dry) memset(o.dry + 2 * off, 0, n * 2 * sizeof(int32_t));
			advance_time(c, n);
			off += n;
			continue;
		}
		if (n > BLOCK) n = BLOCK;
		if ((c.am_chans | c.vib_chans) & c.active) {
			const uint32_t t = ticks_before(c, c.now);
			if (int32_t(t - c.lfo_next) >= 0) lfo_update(c, t);
			const uint32_t s = samples_before(c, c.now, c.lfo_next);
			const uint32_t s1 = s ? s : 1;
			if (s1 < n) n = s1;
		}
		bool any = false;
		uint32_t bus = 0;   // the buses with sums this block
		const bool fast = mix_fast(c, o, c.active);
#ifdef HV_RVV
		const uint32_t rest = lanes_pass(c, o, n, any, bus, fast);
#else
		const uint32_t rest = c.active;
#endif
		int32_t *dry = o.dry + 2 * off;
		uint32_t made = 0;   // 2: the dry output is made already (the A25's last pass)
#ifdef HV_DSP
		if (fast) {
			made = dsp_voices(c, o, rest, off, n, bus);
			any = made == 1;
			if (made) res = SOUNDED;
		} else
#endif
		for (uint32_t m = rest; m; m &= m - 1) voice_render(c, o, lowest_bit(m), n, any, bus, fast);
		if (any) {
			res = SOUNDED;
			fast ? dry_out(c, dry, o.add_dry, n) : dry_out_slow(c, dry, o.add_dry, n);
		} else if (!o.add_dry && made != 2) {
			memset(dry, 0, n * 2 * sizeof(int32_t));
		}
		if (bus) { res = SOUNDED; fast ? bus_out(c, o, off, bus, n) : bus_out_slow(c, o, off, bus, n); }
		advance_time(c, n);
		off += n;
	}
	return res;
}

uint32_t play(Chip *chips, uint32_t nchips, const uint32_t *records, size_t count, const Output &o, Rate rate, uint32_t flags) {
	uint32_t res = 0;
	uint32_t at[4] = {0, 0, 0, 0};
	const uint32_t scale = rate / 48000;
	for (size_t i = 0; i < count; ++i) {
		const uint32_t w = records[i], k = (w >> 4) & 3, addr = (w >> 6) & 0x1FF, d = (w >> 15) & 0xFF;
		if (k >= nchips) continue;
		Chip &c = chips[k];
		switch (w & 15) {
		case 1: write(c, addr, uint8_t(d)); break;
		case 2: set_pan(c, addr, uint8_t(d)); break;
		case 3: set_sends(c, addr, uint8_t(d), uint8_t((w >> 23) & 0xFF)); break;
		case 4: init(c, rate, flags); break;
		case 5: {
			const uint32_t n = ((w >> 6) & 0xFFFF) * scale;
			const Output p = {o.dry + 2 * at[k], o.rev ? o.rev + 2 * at[k] : nullptr, o.cho ? o.cho + 2 * at[k] : nullptr, true};
			res |= render(c, p, n);
			at[k] += n;
			break;
		}
		case 6: set_gain(c, addr, int8_t(d)); break;
		default: break;
		}
	}
	return res;
}

}  // namespace hvopl3

extern "C" {
uint32_t hvopl3_version(void) { return hvopl3::version(); }
const char *hvopl3_version_string(void) { return hvopl3::version_string(); }
size_t hvopl3_chip_size(void) { return sizeof(hvopl3::Chip); }
void hvopl3_init(void *c, uint32_t rate, uint32_t flags) { hvopl3::init(*static_cast<hvopl3::Chip *>(c), hvopl3::Rate(rate), flags); }
void hvopl3_write(void *c, uint32_t reg, uint8_t v) { hvopl3::write(*static_cast<hvopl3::Chip *>(c), reg, v); }
void hvopl3_set_pan(void *c, uint32_t ch, uint8_t p) { hvopl3::set_pan(*static_cast<hvopl3::Chip *>(c), ch, p); }
void hvopl3_set_sends(void *c, uint32_t ch, uint8_t r, uint8_t s) { hvopl3::set_sends(*static_cast<hvopl3::Chip *>(c), ch, r, s); }
void hvopl3_set_gain(void *c, uint32_t ch, int8_t l) { hvopl3::set_gain(*static_cast<hvopl3::Chip *>(c), ch, l); }
void hvopl3_set_flags(void *c, uint32_t f) { hvopl3::set_flags(*static_cast<hvopl3::Chip *>(c), f); }
uint32_t hvopl3_render(void *c, int32_t *d, int32_t *r, int32_t *s, int add, uint32_t n) {
	const hvopl3::Output o = {d, r, s, add != 0};
	return hvopl3::render(*static_cast<hvopl3::Chip *>(c), o, n);
}
}
