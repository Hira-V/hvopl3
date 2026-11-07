// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3_drv.cpp -- the MIDI driver (docs/DRIVER.md): messages queued for a block, applied at their
// frames between control periods; voices on chip channels; banks in a caller's pool.
//
#include "hvopl3_drv.hpp"
#include <string.h>

// The chip's render, replaceable by a bench that counts what the chips cost apart from the driver.
#ifdef HVOPL3_DRV_CHIP_RENDER
uint32_t HVOPL3_DRV_CHIP_RENDER(hvopl3::Chip &, const hvopl3::Output &, uint32_t);
#else
#define HVOPL3_DRV_CHIP_RENDER ::hvopl3::render
#endif

namespace hvopl3 {
namespace drv {
namespace {

#include "hvopl3_drv_tables.inc"

// A slot's state: HELD is a note whose key is up, kept sounding by a pedal.
enum : uint8_t { S_FREE, S_ON, S_HELD, S_RELEASED, S_PENDING };
// Its role: R_4OP2 is a 4-op pair's second channel, which only follows its first.
enum : uint8_t { R_NONE, R_2OP, R_4OP, R_4OP2, R_PSEUDO1, R_PSEUDO2 };
// Flags: F_OFF the key is up (held by a pedal), F_SOST held by sostenuto, F_RESTRIKE a pending re-strike
// (starts at the next period, sound or not), F_CONVERT a pending start that changes its pair's mode.
enum : uint8_t { F_OFF = 1, F_SOST = 2, F_RESTRIKE = 4, F_CONVERT = 8, F_CUT = 16 };   // F_CUT: a release cut at its cap
enum : uint8_t { P_INIT = 1, P_RESET = 2, P_PANIC = 4, P_LEVELS = 8, P_DEEP = 16, P_FLAGS = 32 };   // P_LEVELS..P_FLAGS: update()'s
// SysEx, parsed when queued: the event's `a`.
enum : uint8_t { X_GM_ON, X_GM_OFF, X_GM2_ON, X_GS_RESET, X_XG_RESET, X_MASTER_VOLUME, X_MASTER_FINE, X_MASTER_COARSE, X_GS_RHYTHM };

// RELEASE_CAP: control periods a release may sound (10 s) before it is cut: some patches never end (release rate 0)
enum : uint32_t { SCAN = 32, WAIT_CUT = 3, WAIT_CONVERT = 16, RELEASE_CAP = 7500, OCTAVE = 49152, SEMI = 4096, VIB_INC = 31496426 };
constexpr uint8_t OP_OFF[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
constexpr int8_t PAIR[18] = {0, 1, 2, 0, 1, 2, -1, -1, -1, 3, 4, 5, 3, 4, 5, -1, -1, -1};
constexpr uint8_t LEAD[6] = {0, 1, 2, 9, 10, 11};
constexpr uint32_t NOPAIR = 0x1C0 | 0x1C0u << 9;   // channels 6-8, 15-17: in no 4-op pair
constexpr uint8_t HEARD[4] = {0x8, 0x9, 0xA, 0xD};  // the 4-op connections' carriers, a bit an operator
constexpr uint32_t ALL = (1u << CHANNELS) - 1;

inline uint32_t chreg(uint32_t ch, uint32_t base) { return (ch >= 9 ? 0x100u : 0u) + base + ch % 9; }
inline uint32_t opreg(uint32_t ch, uint32_t n, uint32_t base) { return (ch >= 9 ? 0x100u : 0u) + base + OP_OFF[ch % 9] + (n ? 3u : 0u); }
inline uint32_t lowest(uint32_t m) { uint32_t i = 0; while (!(m >> i & 1)) ++i; return i; }
// The channels of the 4-op pairs a chip's 0x104 has on.
inline uint32_t four_chans(uint32_t four) {
	uint32_t m = 0;
	for (uint32_t p = 0; p < 6; ++p) if (four >> p & 1) m |= 9u << LEAD[p];
	return m;
}
inline uint32_t clamp7(int32_t v) { return v < 0 ? 0u : v > 127 ? 127u : uint32_t(v); }

// ---- output: the chips driven, or the records -------------------------------------------------

inline uint32_t chip_bits(uint32_t k) { return (k & 3) << 4 | (k >> 2) << 29; }   // HVO2: 5..4, then 31..29
inline void emit(Driver &d, uint32_t w) { if (d.rec_n < d.rec_cap) d.rec[d.rec_n++] = w; }

// The chip made (or advanced) up to the block's position.
void flush(Driver &d, uint32_t k) {
	const uint32_t n = d.pos - d.done[k];
	if (!n) return;
	Chip &c = d.chips[k];
	if (d.cfg.stream) {
		advance(c, n);
		const uint32_t native = d.cfg.rate == RATE_96K ? 1u << 22 : 0;
		for (uint32_t left = n; left;) {
			const uint32_t m = left > 0xFFFF ? 0xFFFF : left;
			emit(d, 5 | chip_bits(k) | m << 6 | native);
			left -= m;
		}
	} else {
		const uint32_t o = 2 * d.done[k];
		const Output p = {d.out.dry + o, d.out.rev ? d.out.rev + o : nullptr, d.out.cho ? d.out.cho + o : nullptr, true};
		d.sounded |= HVOPL3_DRV_CHIP_RENDER(c, p, n);
	}
	d.done[k] = d.pos;
}
void wr(Driver &d, uint32_t k, uint32_t reg, uint8_t v) {
	flush(d, k);
	if (d.cfg.stream) emit(d, 1 | chip_bits(k) | reg << 6 | uint32_t(v) << 15);
	write(d.chips[k], reg, v);
}
void wpan(Driver &d, uint32_t k, uint32_t ch, uint8_t p) {
	flush(d, k);
	if (d.cfg.stream) emit(d, 2 | chip_bits(k) | ch << 6 | uint32_t(p) << 15);
	set_pan(d.chips[k], ch, p);
}
void wsends(Driver &d, uint32_t k, uint32_t ch, uint8_t r, uint8_t s) {
	flush(d, k);
	if (d.cfg.stream) emit(d, 3 | chip_bits(k) | ch << 6 | uint32_t(r) << 15 | uint32_t(s & 15) << 11 | uint32_t(s >> 4) << 23);
	set_sends(d.chips[k], ch, r, s);
}
void wgain(Driver &d, uint32_t k, uint32_t ch, int8_t g) {
	flush(d, k);
	if (d.cfg.stream) emit(d, 6 | chip_bits(k) | ch << 6 | uint32_t(uint8_t(g)) << 15);
	set_gain(d.chips[k], ch, g);
}

// ---- lists -----------------------------------------------------------------------------------

void clist_add(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	Channel &c = d.ch[s.midi];
	s.cnext = NONE; s.cprev = c.tail;
	if (c.tail != NONE) d.slot[c.tail].cnext = uint16_t(g); else c.head = uint16_t(g);
	c.tail = uint16_t(g);
}
void clist_del(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	Channel &c = d.ch[s.midi];
	if (s.cprev != NONE) d.slot[s.cprev].cnext = s.cnext; else c.head = s.cnext;
	if (s.cnext != NONE) d.slot[s.cnext].cprev = s.cprev; else c.tail = s.cprev;
	s.cprev = s.cnext = NONE;
}
// The held list (held = true) or the released list, oldest first.
void glist_add(Driver &d, uint32_t g, bool held) {
	uint16_t &head = held ? d.held_head : d.rel_head, &tail = held ? d.held_tail : d.rel_tail;
	Slot &s = d.slot[g];
	s.gnext = NONE; s.gprev = tail;
	if (tail != NONE) d.slot[tail].gnext = uint16_t(g); else head = uint16_t(g);
	tail = uint16_t(g);
}
void glist_del(Driver &d, uint32_t g, bool held) {
	uint16_t &head = held ? d.held_head : d.rel_head, &tail = held ? d.held_tail : d.rel_tail;
	Slot &s = d.slot[g];
	if (s.gprev != NONE) d.slot[s.gprev].gnext = s.gnext; else head = s.gnext;
	if (s.gnext != NONE) d.slot[s.gnext].gprev = s.gprev; else tail = s.gprev;
	s.gprev = s.gnext = NONE;
}
inline bool in_held(const Slot &s) { return (s.state == S_ON || s.state == S_HELD) && s.role != R_4OP2 && s.role != R_PSEUDO2; }

// Out of every list and mask: owned by nobody, its chip channel as it was.
void detach(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	const uint32_t k = g / CHANNELS, bit = 1u << (g % CHANNELS);
	if (s.state == S_FREE) { d.free_mask[k] &= ~bit; ++d.busy[k]; return; }
	if (s.glide) { --d.ch[s.midi].gliders; s.glide = 0; }
	if (s.role != R_4OP2) clist_del(d, g);
	if (in_held(s)) glist_del(d, g, true);
	else if (s.state == S_RELEASED) glist_del(d, g, false);
	d.rel_mask[k] &= ~bit;
	d.pend_mask[k] &= ~bit;
}
void make_free(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	if (s.state == S_FREE) return;
	const uint32_t k = g / CHANNELS, bit = 1u << (g % CHANNELS);
	const uint8_t role = s.role;
	const uint16_t partner = s.partner;
	detach(d, g);
	s.state = S_FREE; s.role = R_NONE; s.flags = 0; s.partner = NONE;
	d.free_mask[k] |= bit;
	--d.busy[k];
	if (role == R_4OP && partner != NONE) {
		Slot &t = d.slot[partner];
		t.state = S_FREE; t.role = R_NONE; t.flags = 0; t.partner = NONE;
		d.free_mask[k] |= 1u << (partner % CHANNELS);
		--d.busy[k];
	}
}

// ---- levels ------------------------------------------------------------------------------------

// THE SOUND DRIVERS' VOLUME MODELS (CURVE_DMX..CURVE_OCONNELL; docs/DRIVER.md section 8). Each takes the
// velocity v (after the patch's offset) and the channel volume c = CC 7 x CC 11 x master (a product index),
// and gives an operator its total level from the patch's own: family 0 scales toward 63 by a factor
// (63 - R((63 - tl) k / div)), family 1 adds an attenuation, family 2 scales with an offset
// (63 - ((63 - tl) K + B) / 64). How v and c combine: 0 one factor at the index v c / 127; 1 their
// attenuations as fractions of 63 multiplied; 2 their factors multiplied, over the factor at full volume
// (rounded up), so that each alone is exact; 3 attenuations added; 4 attenuations added, then scaled as 1. Comb 0 takes the velocity's own table
// when the volume is full (the models' velocity and volume tables differ by a step here and there).
// A 2-op voice's first operator, when it is heard: 0 as the second; 1 its own attenuations, added (a1v,
// a1c); 2 never scaled.
struct VolModel {
	uint8_t family, comb, op1, rnd;
	uint16_t div;
	uint16_t kv[128], kc[128];
	uint8_t a1v[128], a1c[128];
};
constexpr VolModel MODELS[14] = {   // CURVE_DMX .. CURVE_NATIVE, in order
#include "hvopl3_drv_models.inc"
};
inline bool is_model(uint8_t curve) { return curve >= CURVE_DMX && curve <= CURVE_NATIVE; }
inline uint32_t rdiv(uint32_t a, uint32_t b, uint8_t rnd) { return rnd == 0 ? a / b : rnd == 1 ? (a + b / 2) / b : (a + b - 1) / b; }
uint32_t model_rule(const VolModel &M, uint32_t tl, uint32_t k) {
	if (M.family == 1) { const uint32_t t = tl + k; return t > 63 ? 63 : t; }
	if (M.family == 2) {
		const int32_t t = 63 - int32_t(((63 - tl) * (k & 255) + (k >> 8)) / 64);
		return t < 0 ? 0 : t > 63 ? 63 : uint32_t(t);
	}
	const uint32_t r = rdiv((63 - tl) * k, M.div, M.rnd);
	return r >= 63 ? 0 : 63 - r;
}
// An operator's total level (0..63) from its own: the model's, for velocity v and channel volume c.
uint32_t model_tl(uint8_t curve, uint32_t tl, uint32_t v, uint32_t c, bool first) {
	const VolModel &M = MODELS[curve - CURVE_DMX];
	tl &= 63;
	if (first && M.op1 == 2) return tl;
	if (first && M.op1 == 1) { const uint32_t t = tl + M.a1v[v] + M.a1c[c]; return t > 63 ? 63 : t; }
	switch (M.comb) {
	case 0: return model_rule(M, tl, c == 127 ? M.kv[v] : M.kc[v * c / 127]);   // velocity alone: its own table
	case 1: case 4: {
		const uint32_t av = model_rule(M, 0, M.kv[v]), ac = model_rule(M, 0, M.kc[c]);
		const uint32_t a = M.comb == 4 ? av + ac : 63 - ((63 - av) * (63 - ac) + 31) / 63;
		return a >= 63 ? 63 : 63 - ((63 - tl) * (63 - a) + 62) / 63;
	}
	case 2: return model_rule(M, tl, rdiv(uint32_t(M.kv[v]) * M.kc[c], M.kc[127] ? M.kc[127] : 1, 2));   // over full volume's factor
	default: { const uint32_t t = tl + M.kv[v] + M.kc[c]; return t > 63 ? 63 : t; }
	}
}
inline uint32_t volume_chain(uint32_t vol, uint32_t expr, uint32_t master) { return (vol * expr / 127) * master / 127; }

uint8_t curve_of(const Driver &d) {
	if (d.cfg.curve != CURVE_AUTO) return d.cfg.curve;
	return d.pool && d.pool->curve != CURVE_AUTO ? d.pool->curve : uint8_t(CURVE_HIRAV);
}
// Velocity, volume, expression and master volume as one attenuation in 0.75 dB steps; 255 silent.
uint32_t curve_steps(const Driver &d, uint32_t vel, uint32_t vol, uint32_t expr, uint32_t master) {
	const uint8_t curve = curve_of(d);
	if (is_model(curve)) return vel && vol && expr && master ? model_tl(curve, 0, vel, volume_chain(vol, expr, master), false) : 255;
	switch (curve) {
	case CURVE_LINEAR: {
		// their product, 0..1 as 0..127^2, mapped linearly onto 63..0 total-level steps
		uint32_t p = vel * vol;
		p = (p * expr + 63) / 127;
		p = (p * master + 63) / 127;
		return (63 * (16129 - p) + 8064) / 16129;
	}
	case CURVE_TABLE: {
		const uint32_t a = d.vel_table[vel], b = d.vol_table[vol], c = d.vol_table[expr], m = d.vol_table[master];
		if (a == 0xFFFF || b == 0xFFFF || c == 0xFFFF || m == 0xFFFF) return 255;
		const uint32_t s = (a + b + c + m + 8) >> 4;
		return s > 255 ? 255 : s;
	}
	default: {
		if (!vel || !vol || !expr || !master) return 255;
		const uint64_t q = HIRAV_Q32[vel] + HIRAV_Q32[vol] + HIRAV_Q32[expr] + HIRAV_Q32[master] + (1ull << 31);
		const uint32_t s = uint32_t(q >> 32);
		return s > 255 ? 255 : s;
	}
	}
}

// GS's per-drum settings for a voice (none for a melodic one): its level as part of the expression, its
// pan about the channel's, its sends as a share of the channel's.
inline const Driver::Drum *drum_of(const Driver &d, const Slot &s) { return s.drum ? &d.drums[s.drum - 1][s.note & 127] : nullptr; }
inline uint32_t expr_of(const Driver &d, const Slot &s, const Channel &c) {
	const Driver::Drum *r = drum_of(d, s);
	return r ? (c.expression * r->level + 63) / 127 : c.expression;
}

// The voice's operators that its sound comes out of take the attenuation; the gain carries the
// correction (and, with gain_remainder, what total level 63 cannot). g: a voice's first slot.
void apply_level(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	const Channel &c = d.ch[s.midi];
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	const uint8_t curve = curve_of(d);
	if (is_model(curve)) {   // a sound driver's model: each heard operator its own level, as the model has it
		Slot *two = s.role == R_4OP ? &d.slot[s.partner] : nullptr;
		const uint32_t ex = expr_of(d, s, c);
		const uint32_t cv = volume_chain(c.volume, ex, d.master_volume);
		const bool any = c.volume && ex && d.master_volume;
		uint32_t least = 63;
		for (uint32_t n = 0; n < 2; ++n) {
			if (s.carriers >> n & 1) {
				const uint32_t t = any ? model_tl(curve, s.tl[n], s.vel, cv, n == 0 && !two) : 63;
				least = t < least ? t : least;
				wr(d, k, opreg(ch, n, 0x40), uint8_t((s.tl[n] & 0xC0) | t));
			}
			if (two && (two->carriers >> n & 1)) {
				const uint32_t t = any ? model_tl(curve, two->tl[n], s.vel, cv, false) : 63;
				least = t < least ? t : least;
				wr(d, k, opreg(ch + 3, n, 0x40), uint8_t((two->tl[n] & 0xC0) | t));
			}
		}
		s.level = uint8_t(least);
		if (s.gain_w != 0) { s.gain_w = 0; wgain(d, k, ch, 0); }
		return;
	}
	const uint32_t steps = curve_steps(d, s.vel, c.volume, expr_of(d, s, c), d.master_volume);
	Slot *two = s.role == R_4OP ? &d.slot[s.partner] : nullptr;
	uint32_t least = 63;   // the loudest carrier's own level
	for (uint32_t n = 0; n < 2; ++n) {
		if (s.carriers >> n & 1) least = (s.tl[n] & 63u) < least ? (s.tl[n] & 63u) : least;
		if (two && (two->carriers >> n & 1)) least = (two->tl[n] & 63u) < least ? (two->tl[n] & 63u) : least;
	}
	uint32_t excess = 0;
	if (d.cfg.gain_remainder && steps < 255 && least + steps > 63) excess = least + steps - 63;
	const uint32_t add = steps - excess;
	for (uint32_t n = 0; n < 2; ++n) {
		if (s.carriers >> n & 1) {
			const uint32_t t = (s.tl[n] & 63u) + add;
			wr(d, k, opreg(ch, n, 0x40), uint8_t((s.tl[n] & 0xC0) | (t > 63 ? 63 : t)));
		}
		if (two && (two->carriers >> n & 1)) {
			const uint32_t t = (two->tl[n] & 63u) + add;
			wr(d, k, opreg(ch + 3, n, 0x40), uint8_t((two->tl[n] & 0xC0) | (t > 63 ? 63 : t)));
		}
	}
	const uint32_t lv = least + steps;
	s.level = uint8_t(lv > 255 ? 255 : lv);
	int32_t gain = (d.cfg.level_corrections && curve_of(d) == CURVE_HIRAV ? s.corr : 0) - int32_t(excess);
	gain = gain < -32 ? -32 : gain > 32 ? 32 : gain;
	if (s.gain_w != int8_t(gain)) { s.gain_w = int8_t(gain); wgain(d, k, ch, int8_t(gain)); }
}

uint8_t out_bits(const Driver &d, uint32_t pan) {
	if (!d.cfg.hard_pan) return 0x30;
	return pan <= 42 ? 0x10 : pan >= 86 ? 0x20 : 0x30;   // left, both, right: thirds of the range
}
void apply_pan(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	const Channel &c = d.ch[s.midi];
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	const Driver::Drum *r = drum_of(d, s);
	const int32_t pp = r && r->pan ? int32_t(r->pan) + c.pan - 64 : c.pan;   // a drum's own pan, about the channel's
	const uint32_t pan = uint32_t(pp < 0 ? 0 : pp > 127 ? 127 : pp);
	const uint8_t ob = out_bits(d, pan);
	if ((s.c0 & 0x30) != ob) { s.c0 = uint8_t((s.c0 & 0x0F) | ob); wr(d, k, chreg(ch, 0xC0), s.c0); }
	if (s.role == R_4OP) {
		Slot &t = d.slot[s.partner];
		if ((t.c0 & 0x30) != ob) { t.c0 = uint8_t((t.c0 & 0x0F) | ob); wr(d, k, chreg(ch + 3, 0xC0), t.c0); }
	}
	const uint8_t p = d.cfg.hard_pan ? 64 : uint8_t(pan);
	if (s.pan_w != p) { s.pan_w = p; wpan(d, k, ch, p); }
}
void apply_sends(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	const Channel &c = d.ch[s.midi];
	const Driver::Drum *r = drum_of(d, s);
	const uint8_t rev = r ? uint8_t((c.reverb * r->reverb + 63) / 127) : c.reverb;
	const uint8_t cho = r ? uint8_t((c.chorus * r->chorus + 63) / 127) : c.chorus;
	if (s.rev_w != rev || s.cho_w != cho) {
		s.rev_w = rev; s.cho_w = cho;
		wsends(d, g / CHANNELS, g % CHANNELS, rev, cho);
	}
}

// ---- pitch ---------------------------------------------------------------------------------------

// log2(fnum x 2^block) in 1/4096 semitone -> block << 10 | fnum: the largest F-number that fits, rounded.
uint16_t fnblk_of(int32_t u) {
	if (u < 0) u = 0;
	const uint32_t oct = uint32_t(u) / OCTAVE, frac = uint32_t(u) % OCTAVE;
	if (oct >= 17) return 7 << 10 | 1023;
	const uint32_t i = frac >> 6, r = frac & 63;
	const uint32_t m = EXP2_Q30[i] + (((EXP2_Q30[i + 1] - EXP2_Q30[i]) * r + 32) >> 6);   // 2^frac, Q30
	uint32_t block = oct > 9 ? oct - 9 : 0;
	const uint32_t sh = 30 - (oct - block);
	uint32_t f = (m + (1u << (sh - 1))) >> sh;
	if (f > 1023) {
		if (block < 7) { ++block; f = (f + 1) >> 1; }
		else f = 1023;
	}
	return uint16_t(block << 10 | f);
}
void channel_offset(Driver &d, Channel &c) {
	const int32_t range = int32_t(c.bend_semis) * SEMI + (int32_t(c.bend_cents) * SEMI + 50) / 100;
	int32_t off = int32_t((int64_t(int32_t(c.bend) - 8192) * range) >> 13);
	off += (int32_t(c.fine) - 8192) >> 1;   // RPN 1: 8192 = 100 cents = 4096 units
	off += int32_t(c.coarse) * SEMI;
	off += ((int32_t(d.master_fine) - 8192) >> 1) + int32_t(d.master_coarse) * SEMI;
	c.offset = off;
}
uint32_t vib_depth(const Driver &d, const Channel &c) {
	uint32_t v = c.modulation + (d.cfg.pressure_vibrato ? c.pressure : 0);
	return v > 127 ? 127 : v;
}
int32_t vib_now(const Driver &d, const Channel &c) {
	const uint32_t depth = vib_depth(d, c);
	if (!depth) return 0;
	const int32_t units = int32_t((depth * 2048 + 63) / 127);   // +-50 cents at 127
	return (units * VIB_SIN[c.vib_phase >> 24]) >> 14;
}
// The voice's pitch to the chip, if it moved (and always with `force`).
void apply_pitch(Driver &d, uint32_t g, bool force) {
	Slot &s = d.slot[g];
	const Channel &c = d.ch[s.midi];
	const uint16_t fb = fnblk_of(s.base + c.offset + vib_now(d, c) + s.glide + PITCH_BASE);
	if (!force && fb == s.fnblk) return;
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	const bool key = s.state == S_ON || s.state == S_HELD;
	if (force || (fb & 0xFF) != (s.fnblk & 0xFF)) wr(d, k, chreg(ch, 0xA0), uint8_t(fb));
	if (force || (fb >> 8) != (s.fnblk >> 8)) wr(d, k, chreg(ch, 0xB0), uint8_t((key ? 0x20 : 0) | fb >> 8));
	s.fnblk = fb;
}

// ---- voices ------------------------------------------------------------------------------------

// Key off: the release as the patch has it, or `fast`: release rate 15 on every operator (a cut). The
// sustain level does not matter once released; a start rewrites every register.
void key_off(Driver &d, uint32_t g, bool fast) {
	Slot &s = d.slot[g];
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	if (fast) {
		const uint32_t chans = s.role == R_4OP ? 2 : 1;
		for (uint32_t x = 0; x < chans; ++x)
			for (uint32_t n = 0; n < 2; ++n) wr(d, k, opreg(ch + 3 * x, n, 0x80), 0x0F);
		s.rr = 15;
	}
	wr(d, k, chreg(ch, 0xB0), uint8_t(s.fnblk >> 8));
}

void set_four(Driver &d, uint32_t k, uint32_t four) {
	if (d.four[k] == four) return;
	d.four[k] = uint8_t(four);
	wr(d, k, 0x104, uint8_t(four));
}
inline bool four_mode(const Driver &d, uint32_t g) {
	const int p = PAIR[g % CHANNELS];
	return p >= 0 && (d.four[g / CHANNELS] >> p & 1);
}
inline bool primary(const Slot &s) { return s.role == R_2OP || s.role == R_4OP || s.role == R_PSEUDO1; }

// A sounding slot (on or held) to released: the key off (fast: a cut), into the released list. Pseudo-4-op
// halves go their own ways from here.
void release_one(Driver &d, uint32_t g, bool fast) {
	Slot &s = d.slot[g];
	const uint32_t k = g / CHANNELS;
	if (in_held(s)) glist_del(d, g, true);
	key_off(d, g, fast);
	s.state = S_RELEASED; s.flags = fast ? F_CUT : 0; s.released = d.tick;   // a cut is not returned to
	if (s.role == R_4OP) d.slot[s.partner].state = S_RELEASED;
	else { s.role = R_2OP; s.partner = NONE; }
	glist_add(d, g, false);
	d.rel_mask[k] |= 1u << (g % CHANNELS);
}
void release_voice(Driver &d, uint32_t g, bool fast) {
	const Slot &s = d.slot[g];
	const uint16_t p = (s.role == R_PSEUDO1 || s.role == R_PSEUDO2) ? s.partner : uint16_t(NONE);
	release_one(d, g, fast);
	if (p != NONE) release_one(d, p, fast);
}
// A pending voice that will not start: released as it stands (its key is off already).
void cancel_one(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	const uint32_t k = g / CHANNELS;
	d.pend_mask[k] &= ~(1u << (g % CHANNELS));
	s.state = S_RELEASED; s.flags = 0; s.released = d.tick;
	glist_add(d, g, false);
	d.rel_mask[k] |= 1u << (g % CHANNELS);
	if (s.role == R_4OP) {
		const uint32_t p = s.partner;
		Slot &t = d.slot[p];
		if (four_mode(d, g)) { t.state = S_RELEASED; return; }
		// the pair was to become 4-op: two 2-op channels again, each released
		s.role = R_2OP; s.partner = NONE;
		t.role = R_2OP; t.partner = NONE; t.state = S_RELEASED; t.flags = 0; t.released = d.tick; t.midi = s.midi;
		clist_add(d, p);
		glist_add(d, p, false);
		d.rel_mask[k] |= 1u << (p % CHANNELS);
	} else {
		s.role = R_2OP; s.partner = NONE;
	}
}
void cancel_voice(Driver &d, uint32_t g) {
	const Slot &s = d.slot[g];
	const uint16_t p = (s.role == R_PSEUDO1 || s.role == R_PSEUDO2) ? s.partner : uint16_t(NONE);
	cancel_one(d, g);
	if (p != NONE && d.slot[p].state == S_PENDING) cancel_one(d, p);
}

// Slot g for a new voice: whatever it held ends (cut, if it sounds). Afterwards it is owned by nobody;
// a 4-op voice's second channel stays with it. True if it was free (silent): it can start at once.
bool take(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	if (s.state == S_FREE) { detach(d, g); s.flags = 0; return true; }
	if (s.role == R_PSEUDO1 || s.role == R_PSEUDO2) {
		const uint32_t p = s.partner;
		Slot &t = d.slot[p];
		if (t.state == S_ON || t.state == S_HELD) release_one(d, p, true);
		else if (t.state == S_PENDING) { cancel_one(d, p); key_off(d, p, true); }
		t.partner = NONE;
		s.partner = NONE;
	}
	if (s.state != S_PENDING || (s.flags & F_RESTRIKE)) key_off(d, g, true);
	detach(d, g);
	s.flags = 0;
	return false;
}
// A 4-op voice's second channel back to free (its pair turning 2-op).
void free_partner(Driver &d, uint32_t g) {
	Slot &t = d.slot[g];
	const uint32_t k = g / CHANNELS;
	t.state = S_FREE; t.role = R_NONE; t.flags = 0; t.partner = NONE;
	d.free_mask[k] |= 1u << (g % CHANNELS);
	--d.busy[k];
}

// ---- allocation (docs/DRIVER.md, section 6) --------------------------------------------------------

// A free channel for a 2-op voice, on the chip with the fewest busy channels: a channel in no 4-op pair
// first, then one whose pair is broken already, then any; an all-free 4-op pair turned 2-op last.
int32_t free_2op(Driver &d) {
	int32_t bk = -1;
	uint32_t bm = 0;
	bool bconv = false;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) {
		const uint32_t fm = d.free_mask[k];
		if (!fm || (bk >= 0 && d.busy[k] >= d.busy[bk])) continue;
		uint32_t m = fm & ~four_chans(d.four[k]);
		bool conv = false;
		if (!m)
			for (uint32_t p = 0; p < 6; ++p)
				if ((d.four[k] >> p & 1) && ((fm >> LEAD[p]) & 9) == 9) { m = 1u << LEAD[p]; conv = true; break; }
		if (!m) continue;
		bk = int32_t(k); bm = m; bconv = conv;
	}
	if (bk < 0) return -1;
	uint32_t ch;
	if (bconv) {
		ch = lowest(bm);
		set_four(d, uint32_t(bk), d.four[bk] & ~(1u << PAIR[ch]));
	} else {
		uint32_t pref = bm & NOPAIR;
		if (!pref)
			for (uint32_t m = bm; m; m &= m - 1) {
				const uint32_t c = lowest(m), o = c % 9 < 3 ? c + 3 : c - 3;
				if (!(d.free_mask[bk] >> o & 1)) { pref = 1u << c; break; }
			}
		ch = lowest(pref ? pref : bm);
	}
	return int32_t(uint32_t(bk) * CHANNELS + ch);
}
// A free pair for a 4-op voice: one in 4-op mode first, then a 2-op pair turned; the fewest busy chip.
int32_t free_4op(Driver &d) {
	int32_t bk = -1, bp = -1;
	uint32_t bprio = 2;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) {
		const uint32_t fm = d.free_mask[k];
		for (uint32_t p = 0; p < 6; ++p) {
			if (((fm >> LEAD[p]) & 9) != 9) continue;
			const uint32_t prio = (d.four[k] >> p & 1) ? 0 : 1;
			if (bk < 0 || prio < bprio || (prio == bprio && d.busy[k] < d.busy[bk])) { bk = int32_t(k); bp = int32_t(p); bprio = prio; }
		}
	}
	if (bk < 0) return -1;
	set_four(d, uint32_t(bk), d.four[bk] | 1u << bp);
	return int32_t(uint32_t(bk) * CHANNELS + LEAD[bp]);
}
// How quiet a released voice is by now: its level, and what its release has taken since, guessed from
// the slowest carrier's release rate (rate 15 falls 96 dB in about 2.4 ms, each rate below takes twice as long).
uint32_t quietness(const Driver &d, const Slot &s) {
	uint32_t q = s.level;
	if (s.rr) {
		const uint32_t e = d.tick - s.released;
		const uint32_t fall = e > 4096 ? 255 : (e * 71) >> (15 - s.rr);
		q += fall;
	}
	return q > 255 ? 255 : q;
}
// The best voice to take from the released list (held = false) or the held list, scanning the oldest
// SCAN. want4: a 4-op voice; else a 2-op voice (conv: taking it changes its pair's mode). -1 if none.
int32_t scan(const Driver &d, bool held, bool want4, bool &conv) {
	int32_t best = -1;
	int32_t bkey = -1000;
	uint32_t n = 0;
	for (uint32_t g = held ? d.held_head : d.rel_head; g != NONE && n < SCAN; g = d.slot[g].gnext, ++n) {
		const Slot &s = d.slot[g];
		if (held && s.start == d.tick) continue;   // started this control period
		bool c;
		if (want4) { if (s.role != R_4OP) continue; c = false; }
		else { if (s.role != R_2OP && s.role != R_4OP && s.role != R_PSEUDO1) continue; c = s.role == R_4OP || four_mode(d, g); }
		int32_t key;
		if (held) {
			const uint32_t age = (d.tick - s.start) >> 6;
			key = int32_t(s.level) + ((s.flags & F_OFF) ? 24 : 0) + int32_t(age > 16 ? 16 : age) - (c ? 32 : 0);
		} else {
			key = int32_t(quietness(d, s)) - (c ? 64 : 0);
		}
		if (key > bkey) { bkey = key; best = int32_t(g); conv = c; }
	}
	return best;
}
// A 2-op-mode pair whose channels are both free or released: to become a 4-op voice.
int32_t pair_avail(const Driver &d) {
	int32_t bk = -1, bp = -1;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) {
		const uint32_t av = d.free_mask[k] | d.rel_mask[k];
		for (uint32_t p = 0; p < 6; ++p) {
			if ((d.four[k] >> p & 1) || ((av >> LEAD[p]) & 9) != 9) continue;
			if (bk < 0 || d.busy[k] < d.busy[bk]) { bk = int32_t(k); bp = int32_t(p); }
			break;
		}
	}
	return bk < 0 ? -1 : int32_t(uint32_t(bk) * CHANNELS + LEAD[bp]);
}
// A held 2-op voice in a 2-op-mode pair, whose pair can be taken whole (its other channel not pending).
int32_t held_pair(const Driver &d) {
	int32_t best = -1, bkey = -1000;
	uint32_t n = 0;
	for (uint32_t g = d.held_head; g != NONE && n < SCAN; g = d.slot[g].gnext, ++n) {
		const Slot &s = d.slot[g];
		if (s.start == d.tick || s.role == R_4OP || PAIR[g % CHANNELS] < 0 || four_mode(d, g)) continue;
		const uint32_t c = g % CHANNELS, o = g - c + (c % 9 < 3 ? c + 3 : c - 3);
		if (d.slot[o].state == S_PENDING || (d.slot[o].state != S_FREE && d.slot[o].start == d.tick && in_held(d.slot[o]))) continue;
		const uint32_t age = (d.tick - s.start) >> 6;
		const int32_t key = int32_t(s.level) + ((s.flags & F_OFF) ? 24 : 0) + int32_t(age > 16 ? 16 : age);
		if (key > bkey) { bkey = key; best = int32_t(g); }
	}
	if (best < 0) return -1;
	const uint32_t c = uint32_t(best) % CHANNELS;
	return int32_t(uint32_t(best) - c + LEAD[PAIR[c]]);
}

// A channel for a 2-op voice: free, else released and quietest, else held and least missed. ready: free,
// it can start now; conv: its pair changes mode first. -1: none.
int32_t get_2op(Driver &d, bool &ready, bool &conv) {
	conv = false;
	int32_t g = free_2op(d);
	if (g >= 0) { take(d, uint32_t(g)); ready = true; return g; }
	ready = false;
	g = scan(d, false, false, conv);
	if (g < 0) g = scan(d, true, false, conv);
	if (g < 0) return -1;
	const bool was4 = d.slot[g].role == R_4OP;
	const uint16_t p = d.slot[g].partner;
	take(d, uint32_t(g));
	if (was4) free_partner(d, p);
	return g;
}
// A pair for a 4-op voice, in the same order; a 2-op pair (free or released, then held) turned last.
int32_t get_4op(Driver &d, bool &ready, bool &conv) {
	conv = false;
	int32_t g = free_4op(d);
	if (g >= 0) { take(d, uint32_t(g)); take(d, uint32_t(g) + 3); ready = true; return g; }
	ready = false;
	bool c = false;
	g = scan(d, false, true, c);
	if (g < 0) g = scan(d, true, true, c);
	if (g >= 0) { take(d, uint32_t(g)); return g; }
	g = pair_avail(d);
	if (g < 0) g = held_pair(d);
	if (g < 0) return -1;
	take(d, uint32_t(g));
	take(d, uint32_t(g) + 3);
	conv = true;
	return g;
}

// ---- starting a voice --------------------------------------------------------------------------------

void assign(Driver &d, uint32_t g, uint32_t mc, uint32_t note, uint32_t vel, const Patch *P, uint32_t half, uint8_t role, int32_t key, uint8_t flags, bool owned) {
	Slot &s = d.slot[g];
	s.midi = uint8_t(mc); s.note = uint8_t(note); s.patch = P; s.half = uint8_t(half); s.role = role;
	s.state = S_PENDING; s.flags = flags; s.wait = 0; s.glide = 0; s.glide_step = 0;
	s.vel = uint8_t(clamp7(int32_t(vel) + P->velocity_offset)); if (!s.vel) s.vel = 1;
	s.corr = P->gain;
	const Channel &ch = d.ch[mc];
	s.drum = ch.percussion ? uint8_t(1 + ch.drum_map) : 0;
	s.base = (key + P->note_offset[half] + (s.drum ? d.drums[ch.drum_map][note & 127].pitch : 0)) * int32_t(SEMI);
	if (half) s.base += (((int32_t(P->detune) + 128) >> 1) - 64) * 128;   // 1/32 semitone
	if (!owned) clist_add(d, g);
	d.pend_mask[g / CHANNELS] |= 1u << (g % CHANNELS);
}
// One channel's two operators and C0 from the patch; returns the operators that are carriers (a bit each).
void load_channel(Driver &d, uint32_t g, const Operator *op, uint8_t c0, uint8_t carriers) {
	Slot &t = d.slot[g];
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	t.carriers = carriers;
	for (uint32_t n = 0; n < 2; ++n) {
		const Operator &o = op[n];
		t.tl[n] = o.r40;
		wr(d, k, opreg(ch, n, 0x20), o.r20);
		if (!(carriers >> n & 1)) wr(d, k, opreg(ch, n, 0x40), o.r40);
		wr(d, k, opreg(ch, n, 0x60), o.r60);
		wr(d, k, opreg(ch, n, 0x80), o.r80);
		wr(d, k, opreg(ch, n, 0xE0), o.rE0);
	}
	t.c0 = uint8_t((c0 & 0x0F) | out_bits(d, d.ch[d.slot[g].midi].pan));
	wr(d, k, chreg(ch, 0xC0), t.c0);
}
void start_voice(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	const Patch &P = *s.patch;
	d.pend_mask[k] &= ~(1u << ch);
	if (PAIR[ch] >= 0) {   // the pair's mode, if the voice needs it changed
		const uint32_t bit = 1u << PAIR[ch];
		if (s.role == R_4OP) set_four(d, k, d.four[k] | bit);
		else if (d.four[k] & bit) set_four(d, k, d.four[k] & ~bit);
	}
	s.state = S_ON; s.flags &= F_OFF; s.start = d.tick;
	uint32_t rr = 15;
	if (s.role == R_4OP) {
		const uint32_t heard = HEARD[(P.c0[0] & 1) | (P.c0[1] & 1) << 1];
		Slot &t = d.slot[s.partner];
		t.state = S_ON;
		load_channel(d, g, P.op, P.c0[0], uint8_t(heard & 3));
		load_channel(d, s.partner, P.op + 2, P.c0[1], uint8_t(heard >> 2));
		for (uint32_t j = 0; j < 4; ++j) if ((heard >> j & 1) && (P.op[j].r80 & 15u) < rr) rr = P.op[j].r80 & 15u;
	} else {
		const uint32_t h = s.half;
		const uint8_t carriers = (P.c0[h] & 1) ? 3 : 2;
		load_channel(d, g, P.op + 2 * h, P.c0[h], carriers);
		for (uint32_t n = 0; n < 2; ++n) if ((carriers >> n & 1) && (P.op[2 * h + n].r80 & 15u) < rr) rr = P.op[2 * h + n].r80 & 15u;
	}
	s.rr = uint8_t(rr);
	s.gain_w = -128; s.pan_w = 0xFF; s.rev_w = s.cho_w = 0xFF;
	apply_level(d, g);
	apply_pan(d, g);
	apply_sends(d, g);
	apply_pitch(d, g, true);
	if (s.role != R_PSEUDO2) glist_add(d, g, true);
}
// A pending voice may start: re-strikes at once; cuts once their channel is silent (or after a few periods).
bool ready(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	++s.wait;
	if (s.flags & F_RESTRIKE) return true;
	const uint32_t k = g / CHANNELS, ch = g % CHANNELS;
	flush(d, k);
	const uint32_t act = d.chips[k].active;
	bool quiet = !(act >> ch & 1);
	if (s.role == R_4OP && (s.flags & F_CONVERT)) quiet = quiet && !(act >> (ch + 3) & 1);
	return quiet || s.wait >= ((s.flags & F_CONVERT) ? WAIT_CONVERT : WAIT_CUT);
}

// ---- MIDI -----------------------------------------------------------------------------------------

const Patch *pick(const Driver &d, const Channel &c, uint32_t note) {
	if (!d.pool) return nullptr;
	const bool perc = c.percussion;
	const uint32_t i = perc ? note : c.program;
	const Bank *b = perc ? find_bank(*d.pool, true, 0, c.program) : find_bank(*d.pool, false, c.msb, c.lsb);
	const Patch *P = b ? &b->patch[i] : nullptr;
	if (!P || P->kind == KIND_BLANK) {
		b = find_bank(*d.pool, perc, 0, 0);
		P = b ? &b->patch[i] : nullptr;
	}
	return P && P->kind != KIND_BLANK ? P : nullptr;
}

// THE RETURNING NOTE (docs/DRIVER.md section 6): the channel's voice of this note and patch still
// releasing (not cut), to be keyed again without a cut. 2-op: a released 2-op voice of the patch's first
// half. 4-op: a released 4-op voice, its pair still in 4-op mode. Pseudo-4-op: both halves, each still
// releasing (g2 the second); one alone is not taken. The newest such voice. -1: none.
int32_t returning(const Driver &d, const Channel &c, uint32_t n, const Patch *P, uint8_t want, int32_t &g2) {
	int32_t h0 = -1, h1 = -1;
	for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
		const Slot &s = d.slot[x];
		if (s.state != S_RELEASED || s.note != n || s.patch != P || (s.flags & F_CUT)) continue;
		if (want == R_4OP) {
			if (s.role == R_4OP && four_mode(d, x) && d.slot[s.partner].state == S_RELEASED) h0 = int32_t(x);
		} else if (s.role == R_2OP && !four_mode(d, x)) {
			if (s.half == 0) h0 = int32_t(x);
			else if (want == R_PSEUDO1) h1 = int32_t(x);
		}
	}
	if (want == R_PSEUDO1 && (h0 < 0 || h1 < 0)) return -1;
	g2 = h1;
	return h0;
}
// A released voice back to sounding: out of the released list (it stays on its channel's list).
void reclaim(Driver &d, uint32_t g) {
	Slot &s = d.slot[g];
	if (s.glide) { --d.ch[s.midi].gliders; s.glide = 0; }
	glist_del(d, g, false);
	d.rel_mask[g / CHANNELS] &= ~(1u << (g % CHANNELS));
	d.slot[g].flags = 0;
}

void note_on(Driver &d, uint32_t mc, uint32_t n, uint32_t v) {
	Channel &c = d.ch[mc];
	const uint32_t from = c.porta_from != 0xFF ? c.porta_from : (c.porta_on && c.porta_time ? c.last_note : 0xFF);
	c.porta_from = 0xFF;
	c.last_note = uint8_t(n);
	const Patch *P = pick(d, c, n);
	if (!P) return;
	const int32_t key = (c.percussion || (P->flags & PATCH_FIXED)) ? P->key : int32_t(n);
	const uint8_t want = P->kind == KIND_4OP ? R_4OP : P->kind == KIND_PSEUDO ? R_PSEUDO1 : R_2OP;
	int32_t g = -1, g2 = -1;
	bool ready1 = false, ready2 = false, conv = false, conv2 = false, owned = false;
	uint8_t fl = 0;
	if (d.cfg.restrike)
		for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
			const Slot &s = d.slot[x];
			if (s.note != n || !primary(s) || (s.state != S_ON && s.state != S_HELD)) continue;
			if (s.role != want) { release_voice(d, x, false); break; }
			// the same voice again: its key off now, on at the next period
			const uint32_t p = s.partner;
			const uint32_t both[2] = {x, p};
			for (uint32_t y : both) {
				if (y == NONE || (y == p && want == R_2OP)) continue;
				Slot &t = d.slot[y];
				if (in_held(t)) glist_del(d, y, true);
				if (t.role != R_4OP2) key_off(d, y, false);
				if (t.glide) { --c.gliders; t.glide = 0; }
				t.state = S_PENDING;
			}
			g = int32_t(x); g2 = want == R_PSEUDO1 ? int32_t(p) : -1;
			owned = true; fl = F_RESTRIKE;
			break;
		}
	if (g < 0 && d.cfg.returning && (g = returning(d, c, n, P, want, g2)) >= 0) {
		reclaim(d, uint32_t(g));
		if (g2 >= 0) reclaim(d, uint32_t(g2));
		owned = ready1 = ready2 = true;
	}
	if (g < 0) {
		g = want == R_4OP ? get_4op(d, ready1, conv) : get_2op(d, ready1, conv);
		if (g < 0) return;   // nothing to take: the note is not played
		if (want == R_PSEUDO1) g2 = get_2op(d, ready2, conv2);
	}
	const uint8_t role = want == R_PSEUDO1 && g2 < 0 ? uint8_t(R_2OP) : want;
	assign(d, uint32_t(g), mc, n, v, P, 0, role, key, uint8_t(fl | (conv ? F_CONVERT : 0)), owned);
	Slot &s = d.slot[g];
	if (role == R_4OP) {
		Slot &t = d.slot[g + 3];
		s.partner = uint16_t(g + 3); t.partner = uint16_t(g); t.role = R_4OP2; t.state = S_PENDING; t.midi = uint8_t(mc);
	}
	if (role == R_PSEUDO1) {
		assign(d, uint32_t(g2), mc, n, v, P, 1, R_PSEUDO2, key, uint8_t(fl | (conv2 ? F_CONVERT : 0)), owned);
		s.partner = uint16_t(g2); d.slot[g2].partner = uint16_t(g);
	}
	// portamento: from the last note (or CC 84's), straight in pitch, over CC 5's time
	if (from != 0xFF && from != n) {
		const uint32_t t = c.porta_time, ticks = (t * t * 3 + 4) / 8;
		if (ticks) {
			const int32_t both[2] = {g, g2};
			for (int32_t y : both) {
				if (y < 0) continue;
				Slot &u = d.slot[y];
				u.glide = (int32_t(from) - int32_t(n)) * int32_t(SEMI);
				u.glide_step = u.glide / int32_t(ticks);
				if (!u.glide_step) u.glide_step = u.glide > 0 ? 1 : -1;
				++c.gliders;
			}
		}
	}
	if (fl) return;   // a re-strike starts at the next period
	if (role == R_PSEUDO1 ? ready1 && ready2 : ready1) {
		start_voice(d, uint32_t(g));
		if (role == R_PSEUDO1) start_voice(d, uint32_t(g2));
	}
}

void note_off(Driver &d, uint32_t mc, uint32_t n) {
	Channel &c = d.ch[mc];
	for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
		Slot &s = d.slot[x];
		if (s.note != n || !primary(s) || (s.flags & F_OFF)) continue;
		if (s.state == S_ON) {
			if (c.sustain || ((s.flags & F_SOST) && c.sostenuto)) {
				s.state = S_HELD; s.flags |= F_OFF;
				if (s.partner != NONE) { d.slot[s.partner].state = S_HELD; d.slot[s.partner].flags |= F_OFF; }
			} else {
				release_voice(d, x, false);
			}
			return;
		}
		if (s.state == S_PENDING) { cancel_voice(d, x); return; }
	}
}

void each_level(Driver &d, uint32_t mc) {
	for (uint32_t x = d.ch[mc].head; x != NONE; x = d.slot[x].cnext) if (d.slot[x].state != S_PENDING) apply_level(d, x);
}
void each_pan(Driver &d, uint32_t mc) {
	for (uint32_t x = d.ch[mc].head; x != NONE; x = d.slot[x].cnext) if (d.slot[x].state != S_PENDING) apply_pan(d, x);
}
void each_sends(Driver &d, uint32_t mc) {
	for (uint32_t x = d.ch[mc].head; x != NONE; x = d.slot[x].cnext) if (d.slot[x].state != S_PENDING) apply_sends(d, x);
}
// The pedals let go: notes whose key is up and that nothing holds now are released.
void pedals(Driver &d, uint32_t mc) {
	Channel &c = d.ch[mc];
	for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
		const Slot &s = d.slot[x];
		if (s.state == S_HELD && primary(s) && !c.sustain && !((s.flags & F_SOST) && c.sostenuto)) release_voice(d, x, false);
	}
}
void all_notes_off(Driver &d, uint32_t mc) {
	Channel &c = d.ch[mc];
	for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
		Slot &s = d.slot[x];
		if (!primary(s)) continue;
		if (s.state == S_PENDING) cancel_voice(d, x);
		else if (s.state == S_ON) note_off(d, mc, s.note);
	}
}
// Every voice of the channel cut: a fast release on everything sounding.
void sound_off(Driver &d, uint32_t mc) {
	Channel &c = d.ch[mc];
	for (uint32_t x = c.head; x != NONE;) {
		Slot &s = d.slot[x];
		const uint32_t next = s.cnext;
		if ((s.state == S_ON || s.state == S_HELD) && primary(s)) release_voice(d, x, true);
		else if (s.state == S_RELEASED) key_off(d, x, true);
		else if (s.state == S_PENDING && primary(s)) {
			const uint16_t p = s.role == R_PSEUDO1 ? s.partner : uint16_t(NONE);
			cancel_voice(d, x);
			key_off(d, x, true);
			if (p != NONE) key_off(d, p, true);
		}
		x = next;
	}
}

void percussion_default(const Driver &d, Channel &c, uint32_t i) {
	c.percussion = c.host_perc ? c.host_perc == 2 : i == 9;
	c.xg_kit = !c.host_perc && d.xg && i == 9;
}
// Reset All Controllers, as General MIDI 2's recommended practice: not program, bank, volume, pan,
// sends or the RPN values.
void reset_controllers(Driver &d, uint32_t mc) {
	Channel &c = d.ch[mc];
	c.modulation = 0; c.pressure = 0; c.expression = 127;
	c.sustain = 0; c.sostenuto = 0; c.soft = 0; c.porta_on = 0; c.porta_from = 0xFF;
	c.rpn_msb = c.rpn_lsb = 127; c.nrpn = 0; c.nrpn_msb = c.nrpn_lsb = 127;
	c.bend = 8192;
	for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) d.slot[x].flags &= ~F_SOST;
	pedals(d, mc);
	channel_offset(d, c);
	each_level(d, mc);
	d.dirty |= uint16_t(1u << mc);
}
void channel_defaults(Driver &d, uint32_t i) {
	Channel &c = d.ch[i];
	c.program = c.msb = c.lsb = c.msb_sel = c.lsb_sel = 0;
	c.volume = 100; c.expression = 127; c.pan = 64; c.reverb = 40; c.chorus = 0;
	c.modulation = 0; c.pressure = 0; c.porta_time = 0; c.porta_on = 0; c.porta_from = 0xFF; c.last_note = 0xFF;
	c.sustain = c.sostenuto = c.soft = 0;
	c.rpn_msb = c.rpn_lsb = 127; c.nrpn = 0; c.nrpn_msb = c.nrpn_lsb = 127; c.drum_map = 0;
	c.bend_semis = 2; c.bend_cents = 0; c.coarse = 0; c.bend = 8192; c.fine = 8192;
	c.vib_phase = 0;
	percussion_default(d, c, i);
	channel_offset(d, c);
}
uint8_t deep_bits(const Driver &d) {
	const uint8_t bank = d.pool ? d.pool->deep : 0;
	const bool t = d.cfg.deep_tremolo < 0 ? (bank & 1) : d.cfg.deep_tremolo > 0;
	const bool v = d.cfg.deep_vibrato < 0 ? (bank & 2) : d.cfg.deep_vibrato > 0;
	return uint8_t((t ? 0x80 : 0) | (v ? 0x40 : 0));
}
// Every note off (released as the patches have it), every controller its default.
void full_reset(Driver &d, bool xg) {
	for (uint32_t g = 0; g < d.nslots; ++g) {
		const Slot &s = d.slot[g];
		if (!primary(s)) continue;
		if (s.state == S_ON || s.state == S_HELD) release_voice(d, g, false);
		else if (s.state == S_PENDING) cancel_voice(d, g);
	}
	d.xg = xg;
	d.master_volume = 127; d.master_fine = 8192; d.master_coarse = 0;
	for (auto &map : d.drums)   // GS's per-drum settings: the kit's own (none here: the channel's)
		for (auto &r : map) { r.pitch = 0; r.level = 127; r.pan = 0; r.reverb = 127; r.chorus = 127; }
	for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) channel_defaults(d, i);
	for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) { each_level(d, i); each_pan(d, i); each_sends(d, i); }
	for (uint32_t k = 0; k < d.cfg.chips; ++k) wr(d, k, 0xBD, deep_bits(d));
	d.dirty = 0xFFFF;
}
void panic_all(Driver &d) {
	for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) sound_off(d, i);
}
void chips_init(Driver &d) {
	d.period = d.cfg.rate == RATE_96K ? 2 * PERIOD_48K : PERIOD_48K;
	d.nslots = d.cfg.chips * CHANNELS;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) {
		if (d.cfg.stream) emit(d, 4 | chip_bits(k));
		init(d.chips[k], d.cfg.rate, chip_flags(d));
		wr(d, k, 0x105, 1);
		wr(d, k, 0x104, 0);
		wr(d, k, 0x01, 0x20);
		wr(d, k, 0x08, 0);
		wr(d, k, 0xBD, deep_bits(d));
		d.free_mask[k] = ALL; d.rel_mask[k] = d.pend_mask[k] = 0; d.four[k] = 0; d.busy[k] = 0;
	}
	for (uint32_t g = 0; g < MAX_SLOTS; ++g) {
		Slot &s = d.slot[g];
		memset(&s, 0, sizeof s);
		s.cprev = s.cnext = s.gprev = s.gnext = s.partner = NONE;
	}
	d.held_head = d.held_tail = d.rel_head = d.rel_tail = NONE;
	d.tick = 0;
	d.next_tick = d.now + d.pos;
	for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) { d.ch[i].head = d.ch[i].tail = NONE; d.ch[i].gliders = 0; }
	d.xg = 0;
	full_reset(d, false);
}

void rpn_set(Driver &d, uint32_t mc, int which, uint32_t v) {   // which: 0 data MSB, 1 data LSB, 2 increment, 3 decrement
	Channel &c = d.ch[mc];
	if (c.nrpn || c.rpn_msb != 0) return;   // NRPNs and the null RPN consumed
	switch (c.rpn_lsb) {
	case 0: {
		if (which == 0) c.bend_semis = uint8_t(v);
		else if (which == 1) c.bend_cents = uint8_t(v > 99 ? 99 : v);
		else {
			int32_t t = int32_t(c.bend_semis) * 100 + c.bend_cents + (which == 2 ? 1 : -1);
			t = t < 0 ? 0 : t > 12799 ? 12799 : t;
			c.bend_semis = uint8_t(t / 100); c.bend_cents = uint8_t(t % 100);
		}
		break;
	}
	case 1: {
		if (which == 0) c.fine = uint16_t(v << 7 | (c.fine & 127));
		else if (which == 1) c.fine = uint16_t((c.fine & ~127u) | v);
		else { const int32_t t = int32_t(c.fine) + (which == 2 ? 1 : -1); c.fine = uint16_t(t < 0 ? 0 : t > 16383 ? 16383 : t); }
		break;
	}
	case 2: {
		if (which == 0) c.coarse = int8_t(int32_t(v) - 64);
		else if (which >= 2) { const int32_t t = c.coarse + (which == 2 ? 1 : -1); c.coarse = int8_t(t < -64 ? -64 : t > 63 ? 63 : t); }
		break;
	}
	default: return;
	}
	channel_offset(d, c);
	d.dirty |= uint16_t(1u << mc);
}
// GS's per-drum NRPNs (data entry MSB), on a percussion channel only, for its drum map's key nrpn_lsb:
// 18h pitch (semitones from 64, at the next note), 1Ah level, 1Ch pan (0 the channel's), 1Dh reverb
// and 1Eh chorus send (shares of the channel's). The key's sounding voices take level, pan and sends at
// once. Other NRPNs are consumed.
void nrpn_set(Driver &d, uint32_t mc, uint32_t v) {
	Channel &c = d.ch[mc];
	if (!c.percussion || c.nrpn_lsb > 127) return;
	Driver::Drum &r = d.drums[c.drum_map][c.nrpn_lsb];
	switch (c.nrpn_msb) {
	case 0x18: r.pitch = int8_t(int32_t(v) - 64); return;
	case 0x1A: r.level = uint8_t(v); break;
	case 0x1C: r.pan = uint8_t(v); break;
	case 0x1D: r.reverb = uint8_t(v); break;
	case 0x1E: r.chorus = uint8_t(v); break;
	default: return;
	}
	for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
		const Slot &s = d.slot[x];
		if (s.note != c.nrpn_lsb || !s.drum || s.state == S_PENDING) continue;
		apply_level(d, x); apply_pan(d, x); apply_sends(d, x);
	}
}
void depth_changed(Driver &d, uint32_t mc, uint32_t before) {
	Channel &c = d.ch[mc];
	if (!before && vib_depth(d, c)) c.vib_phase = 0;
	d.dirty |= uint16_t(1u << mc);
}
void control_change(Driver &d, uint32_t mc, uint32_t cc, uint32_t v) {
	Channel &c = d.ch[mc];
	switch (cc) {
	case 0: c.msb_sel = uint8_t(v); break;
	case 32: c.lsb_sel = uint8_t(v); break;
	case 1: { const uint32_t b = vib_depth(d, c); c.modulation = uint8_t(v); depth_changed(d, mc, b); break; }
	case 5: c.porta_time = uint8_t(v); break;
	case 6: if (c.nrpn) nrpn_set(d, mc, v); else rpn_set(d, mc, 0, v); break;
	case 38: rpn_set(d, mc, 1, v); break;
	case 96: rpn_set(d, mc, 2, v); break;
	case 97: rpn_set(d, mc, 3, v); break;
	case 98: c.nrpn_lsb = uint8_t(v); c.nrpn = 1; break;
	case 99: c.nrpn_msb = uint8_t(v); c.nrpn = 1; break;
	case 100: c.rpn_lsb = uint8_t(v); c.nrpn = 0; break;
	case 101: c.rpn_msb = uint8_t(v); c.nrpn = 0; break;
	case 7: c.volume = uint8_t(v); each_level(d, mc); break;
	case 11: c.expression = uint8_t(v); each_level(d, mc); break;
	case 10: c.pan = uint8_t(v); each_pan(d, mc); break;
	case 91: c.reverb = uint8_t(v); each_sends(d, mc); break;
	case 93: c.chorus = uint8_t(v); each_sends(d, mc); break;
	case 64: { const uint8_t on = v >= 64; if (c.sustain && !on) { c.sustain = 0; pedals(d, mc); } c.sustain = on; break; }
	case 65: c.porta_on = v >= 64; break;
	case 66: {
		const uint8_t on = v >= 64;
		if (on && !c.sostenuto)
			for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) if (d.slot[x].state == S_ON) d.slot[x].flags |= F_SOST;
		if (!on && c.sostenuto) {
			c.sostenuto = 0;
			pedals(d, mc);
			for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) d.slot[x].flags &= ~F_SOST;
		}
		c.sostenuto = on;
		break;
	}
	case 67: c.soft = v >= 64; break;   // kept, no effect
	case 84: c.porta_from = uint8_t(v); break;
	case 120: sound_off(d, mc); break;
	case 121: reset_controllers(d, mc); break;
	case 123: case 124: case 125: case 126: case 127: all_notes_off(d, mc); break;
	default: break;
	}
}
void program_change(Driver &d, uint32_t mc, uint32_t p) {
	Channel &c = d.ch[mc];
	c.program = uint8_t(p); c.msb = c.msb_sel; c.lsb = c.lsb_sel;
	if (c.host_perc) return;
	const uint32_t m = c.msb;
	if (m == 120) { c.percussion = 1; c.xg_kit = 0; }            // GM2 rhythm
	else if (m == 121) { c.percussion = 0; c.xg_kit = 0; }       // GM2 melody
	else if (m == 126 || m == 127) { c.percussion = 1; c.xg_kit = 1; }   // XG SFX and drum kits
	else if (c.percussion && (d.xg || c.xg_kit)) { c.percussion = 0; c.xg_kit = 0; }   // XG: another bank is melodic
}
void sysex(Driver &d, const Event &e) {
	switch (e.a) {
	case X_GM_ON: case X_GM_OFF: case X_GM2_ON: case X_GS_RESET: full_reset(d, false); break;
	case X_XG_RESET: full_reset(d, true); break;
	case X_MASTER_VOLUME:
		d.master_volume = e.b;
		for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) each_level(d, i);
		break;
	case X_MASTER_FINE: case X_MASTER_COARSE:
		if (e.a == X_MASTER_FINE) d.master_fine = uint16_t(e.b | e.c << 7);
		else d.master_coarse = int8_t(int32_t(e.c) - 64);
		for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) channel_offset(d, d.ch[i]);
		d.dirty = 0xFFFF;
		break;
	case X_GS_RHYTHM: {
		Channel &c = d.ch[e.b];
		if (!c.host_perc) { c.percussion = e.c != 0; c.xg_kit = 0; }   // map 1 and map 2 alike, but for
		c.drum_map = e.c == 2 ? 1 : 0;                                  // GS's per-drum settings, a map each
		break;
	}
	default: break;
	}
}
void apply(Driver &d, const Event &e) {
	if (e.status == 0xF0) { sysex(d, e); return; }
	const uint32_t mc = e.status & 15;
	switch (e.status & 0xF0) {
	case 0x80: note_off(d, mc, e.a); break;
	case 0x90: if (e.b) note_on(d, mc, e.a, e.b); else note_off(d, mc, e.a); break;
	case 0xB0: control_change(d, mc, e.a, e.b); break;
	case 0xC0: program_change(d, mc, e.a); break;
	case 0xD0: {
		Channel &c = d.ch[mc];
		const uint32_t b = vib_depth(d, c);
		c.pressure = e.a;
		if (d.cfg.pressure_vibrato) depth_changed(d, mc, b);
		break;
	}
	case 0xE0: {
		Channel &c = d.ch[mc];
		c.bend = uint16_t(e.a | e.b << 7);
		channel_offset(d, c);
		d.dirty |= uint16_t(1u << mc);
		break;
	}
	default: break;   // poly pressure: consumed
	}
}

// A SysEx the driver knows, as an event; false: consumed and ignored.
bool parse_sysex(const uint8_t *m, uint32_t n, Event &e) {
	if (n < 6 || m[n - 1] != 0xF7) return false;
	e.status = 0xF0;
	if (m[1] == 0x7E && n == 6 && m[3] == 0x09) {   // General MIDI System On / Off, GM2 On
		if (m[4] < 1 || m[4] > 3) return false;
		e.a = m[4] == 1 ? X_GM_ON : m[4] == 2 ? X_GM_OFF : X_GM2_ON;
		return true;
	}
	if (m[1] == 0x7F && n == 8 && m[3] == 0x04) {   // device control: master volume, fine and coarse tuning
		if (m[4] == 1) { e.a = X_MASTER_VOLUME; e.b = m[6] & 127; return true; }
		if (m[4] == 3) { e.a = X_MASTER_FINE; e.b = m[5] & 127; e.c = m[6] & 127; return true; }
		if (m[4] == 4) { e.a = X_MASTER_COARSE; e.c = m[6] & 127; return true; }
		return false;
	}
	if (m[1] == 0x41 && n == 11 && m[3] == 0x42 && m[4] == 0x12) {   // Roland GS, data set 1
		if (((m[5] + m[6] + m[7] + m[8] + m[9]) & 127) != 0) return false;
		if (m[5] == 0x40 && m[6] == 0x00 && m[7] == 0x7F && m[8] == 0x00) { e.a = X_GS_RESET; return true; }
		if (m[5] == 0x40 && (m[6] & 0xF0) == 0x10 && m[7] == 0x15) {   // use for rhythm part: block 0 is part 10
			const uint32_t x = m[6] & 15;
			e.a = X_GS_RHYTHM; e.b = uint8_t(x == 0 ? 9 : x <= 9 ? x - 1 : x); e.c = m[8] & 127;
			return true;
		}
		return false;
	}
	if (m[1] == 0x43 && n == 9 && (m[2] & 0xF0) == 0x10 && m[3] == 0x4C && m[4] == 0 && m[5] == 0 && m[6] == 0x7E && m[7] == 0) {
		e.a = X_XG_RESET;   // XG System On
		return true;
	}
	return false;
}

// ---- the control period -----------------------------------------------------------------------------

void control(Driver &d) {
	++d.tick;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) {
		if (d.rel_mask[k]) {   // released voices that can make no more sound (hvopl3::inaudible) or are silent: free
			flush(d, k);
			const Chip &chip = d.chips[k];
			for (uint32_t m = d.rel_mask[k]; m; m &= m - 1) {
				const uint32_t ch = lowest(m), g = k * CHANNELS + ch;
				if (d.slot[g].released == d.tick) continue;
				if (!(chip.active >> ch & 1)) make_free(d, g);
				else if (inaudible(chip, ch)) {
					// silent from here on, but its envelope may never end (release rate 0): cut, so the chip
					// stops making it -- release rate 15 only takes it further down -- then free
					if ((d.slot[g].rr != 15 || !(d.slot[g].flags & F_CUT))) key_off(d, g, true);
					make_free(d, g);
				}
			}
		}
		for (uint32_t m = d.pend_mask[k]; m; m &= m - 1) {
			const uint32_t g = k * CHANNELS + lowest(m);
			Slot &s = d.slot[g];
			if (s.state != S_PENDING || s.role == R_PSEUDO2) continue;
			if (!ready(d, g)) continue;
			if (s.role == R_PSEUDO1) {
				const uint32_t p = s.partner;
				if (!ready(d, p)) continue;
				start_voice(d, g);
				start_voice(d, p);
			} else {
				start_voice(d, g);
			}
		}
	}
	// releases past their cap, oldest first: cut
	for (uint32_t g = d.rel_head; g != NONE && d.tick - d.slot[g].released >= RELEASE_CAP; g = d.slot[g].gnext) {
		Slot &s = d.slot[g];
		if (!(s.flags & F_CUT)) { key_off(d, g, true); s.flags |= F_CUT; }
	}
	for (uint32_t mc = 0; mc < MIDI_CHANNELS; ++mc) {
		Channel &c = d.ch[mc];
		bool upd = d.dirty >> mc & 1;
		if (vib_depth(d, c)) { c.vib_phase += VIB_INC; upd = true; }
		if (c.gliders) {
			for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) {
				Slot &s = d.slot[x];
				if (!s.glide) continue;
				s.glide -= s.glide_step;
				if ((s.glide_step > 0 && s.glide <= 0) || (s.glide_step < 0 && s.glide >= 0)) { s.glide = 0; --c.gliders; }
			}
			upd = true;
		}
		if (!upd || c.head == NONE) continue;
		for (uint32_t x = c.head; x != NONE; x = d.slot[x].cnext) if (d.slot[x].state != S_PENDING) apply_pitch(d, x, false);
	}
	d.dirty = 0;
}

// ---- the block ------------------------------------------------------------------------------------

uint32_t block_reserve(const Driver &d, uint32_t frames) { return stream_reserve(d) + d.cfg.chips * (frames / 0xFFFF + 1); }

uint32_t run(Driver &d, uint32_t frames) {
	if (!frames) return 0;
	if (d.cfg.stream && d.rec_cap - d.rec_n < block_reserve(d, frames)) return 0;
	const uint32_t start = d.now;
	d.pos = 0;
	for (uint32_t k = 0; k < MAX_CHIPS; ++k) d.done[k] = 0;
	if (d.pending & P_INIT) { d.pending = 0; chips_init(d); }
	if (d.pending & P_PANIC) { d.qn = 0; panic_all(d); }
	if (d.pending & P_RESET) full_reset(d, false);
	if (d.pending & P_FLAGS)
		for (uint32_t k = 0; k < d.cfg.chips; ++k) { flush(d, k); set_flags(d.chips[k], chip_flags(d)); }
	if (d.pending & P_DEEP)
		for (uint32_t k = 0; k < d.cfg.chips; ++k) wr(d, k, 0xBD, deep_bits(d));
	if (d.pending & P_LEVELS)
		for (uint32_t i = 0; i < MIDI_CHANNELS; ++i) each_level(d, i);
	d.pending = 0;
	uint32_t i = 0, stop = frames;
	for (;;) {
		const uint32_t ev = i < d.qn ? (d.queue[i].offset < frames ? d.queue[i].offset : frames - 1) : frames;
		const uint32_t tk = d.next_tick - start;
		const bool tick = tk < frames && tk <= ev;
		if (!tick && i >= d.qn) break;
		const uint32_t at = tick ? tk : ev;
		if (d.cfg.stream && d.rec_cap - d.rec_n < block_reserve(d, frames)) { stop = at; break; }
		d.pos = at;
		if (tick) { control(d); d.next_tick += d.period; }
		else apply(d, d.queue[i++]);
	}
	d.pos = stop;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) flush(d, k);
	d.now += stop;
	uint32_t j = 0;
	for (; i < d.qn; ++i, ++j) {
		d.queue[j] = d.queue[i];
		d.queue[j].offset = d.queue[j].offset > stop ? d.queue[j].offset - stop : 0;
	}
	d.qn = j;
	return stop;
}

// ---- the pool --------------------------------------------------------------------------------------

uint32_t index_slots(uint32_t banks) { uint32_t n = 4; while (n < 2 * banks) n <<= 1; return n; }
inline uint32_t hdr_bytes() { return (sizeof(Pool) + 15) & ~15u; }
inline uint32_t bank_key(bool perc, uint32_t msb, uint32_t lsb) { return (perc ? 1u << 14 : 0) | (msb & 127) << 7 | (lsb & 127); }
inline uint32_t hash(const Pool &p, uint32_t key) { return ((key * 2654435761u) >> 12) & p.mask; }
int32_t bank_index(const Pool &p, bool perc, uint32_t msb, uint32_t lsb) {
	const uint32_t key = bank_key(perc, msb, lsb);
	for (uint32_t h = hash(p, key), n = 0; n <= p.mask; h = (h + 1) & p.mask, ++n) {
		const uint32_t v = p.index[h];
		if (!v) return -1;
		const Bank &b = p.banks[v - 1];
		if (bank_key(b.percussion, b.msb, b.lsb) == key) return int32_t(v - 1);
	}
	return -1;
}
int32_t bank_add(Pool &p, bool perc, uint32_t msb, uint32_t lsb) {
	int32_t i = bank_index(p, perc, msb, lsb);
	if (i >= 0) return i;
	if (p.count >= p.capacity) return -1;
	Bank &b = p.banks[p.count];
	memset(&b, 0, sizeof b);
	b.percussion = perc; b.msb = uint8_t(msb & 127); b.lsb = uint8_t(lsb & 127);
	uint32_t h = hash(p, bank_key(perc, msb, lsb));
	while (p.index[h]) h = (h + 1) & p.mask;
	p.index[h] = uint16_t(++p.count);
	return int32_t(p.count - 1);
}

}  // namespace

size_t pool_bytes(uint32_t banks) { return hdr_bytes() + size_t(banks) * sizeof(Bank) + index_slots(banks) * sizeof(uint16_t); }
Pool *pool_init(void *mem, size_t bytes) {
	if (!mem || bytes < pool_bytes(1)) return nullptr;
	uint32_t n = uint32_t((bytes - hdr_bytes()) / sizeof(Bank));
	if (n > 32767) n = 32767;
	while (n > 1 && pool_bytes(n) > bytes) --n;
	Pool *p = static_cast<Pool *>(mem);
	memset(p, 0, sizeof *p);
	p->banks = reinterpret_cast<Bank *>(static_cast<char *>(mem) + hdr_bytes());
	p->index = reinterpret_cast<uint16_t *>(p->banks + n);
	p->capacity = n;
	p->mask = index_slots(n) - 1;
	pool_clear(*p);
	return p;
}
void pool_clear(Pool &p) {
	memset(p.index, 0, (p.mask + 1) * sizeof(uint16_t));
	p.count = 0; p.deep = 0; p.volume_model = 0; p.curve = CURVE_AUTO;
}
bool set_patch(Pool &p, bool perc, uint8_t msb, uint8_t lsb, uint8_t program, const Patch &patch) {
	const int32_t i = bank_add(p, perc, msb, lsb);
	if (i < 0) return false;
	p.banks[i].patch[program & 127] = patch;
	return true;
}
const Bank *find_bank(const Pool &p, bool perc, uint8_t msb, uint8_t lsb) {
	const int32_t i = bank_index(p, perc, msb, lsb);
	return i < 0 ? nullptr : &p.banks[i];
}
const Patch *find_patch(const Pool &p, bool perc, uint8_t msb, uint8_t lsb, uint8_t program) {
	const Bank *b = find_bank(p, perc, msb, lsb);
	return b ? &b->patch[program & 127] : nullptr;
}

// WOPL (OPL3 Bank Editor's format, versions 1 to 3): every read inside `size`; nothing changes unless all
// of it loads.
int load_wopl(Pool &p, const void *data, size_t size) {
	const uint8_t *b = static_cast<const uint8_t *>(data);
	static const uint8_t MAGIC[11] = {'W', 'O', 'P', 'L', '3', '-', 'B', 'A', 'N', 'K', 0};
	if (!b) return LOAD_FORMAT;
	for (uint32_t i = 0; i < 11 && i < size; ++i) if (b[i] != MAGIC[i]) return LOAD_FORMAT;
	if (size < 19) return LOAD_TRUNCATED;
	const uint32_t ver = b[11] | b[12] << 8;
	if (ver < 1 || ver > 3) return LOAD_VERSION;
	const uint32_t mb = uint32_t(b[13]) << 8 | b[14], pb = uint32_t(b[15]) << 8 | b[16];
	const uint32_t nb = mb + pb;
	if (ver == 1 && (mb > 128 || pb > 128)) return LOAD_FORMAT;
	const uint32_t ins = ver >= 3 ? 66 : 62;
	const size_t meta = ver >= 2 ? size_t(34) * nb : 0;
	if (size < 19 + meta + size_t(ins) * 128 * nb) return LOAD_TRUNCATED;
	// each bank's (MSB, LSB): from its metadata; version 1 has none (melodic bank i is MSB i, kit i is LSB i)
	const auto ident = [&](uint32_t i, uint32_t &msb, uint32_t &lsb) {
		if (ver >= 2) { lsb = b[19 + 34 * i + 32]; msb = b[19 + 34 * i + 33]; }
		else if (i < mb) { msb = i; lsb = 0; }
		else { msb = 0; lsb = i - mb; }
	};
	uint32_t fresh = 0;
	for (uint32_t i = 0; i < nb; ++i) {
		uint32_t msb, lsb;
		ident(i, msb, lsb);
		if (msb > 127 || lsb > 127) return LOAD_FORMAT;
		const bool perc = i >= mb;
		if (bank_index(p, perc, msb, lsb) >= 0) continue;
		bool seen = false;
		for (uint32_t j = 0; j < i && !seen; ++j) {
			uint32_t m2, l2;
			ident(j, m2, l2);
			seen = (j >= mb) == perc && m2 == msb && l2 == lsb;
		}
		fresh += !seen;
	}
	if (p.count + fresh > p.capacity) return LOAD_FULL;
	const uint8_t *e = b + 19 + meta;
	for (uint32_t i = 0; i < nb; ++i) {
		uint32_t msb, lsb;
		ident(i, msb, lsb);
		const bool perc = i >= mb;
		Bank &bank = p.banks[bank_add(p, perc, msb, lsb)];
		for (uint32_t k = 0; k < 128; ++k, e += ins) {
			Patch &x = bank.patch[k];
			memset(&x, 0, sizeof x);
			x.note_offset[0] = int16_t(e[32] << 8 | e[33]);
			x.note_offset[1] = int16_t(e[34] << 8 | e[35]);
			x.velocity_offset = int8_t(e[36]);
			x.detune = int8_t(e[37]);
			x.key = e[38] & 127;
			const uint8_t f = e[39];
			x.c0[0] = e[40] & 0x0F;
			x.c0[1] = e[41] & 0x0F;
			// WOPL lists carrier 1, modulator 1, carrier 2, modulator 2; the chip's order is modulator first
			static const uint8_t FROM[4] = {1, 0, 3, 2};
			bool zero = !(x.c0[0] | x.c0[1]);
			for (uint32_t o = 0; o < 4; ++o) {
				const uint8_t *r = e + 42 + 5 * FROM[o];
				x.op[o] = Operator{r[0], r[1], r[2], r[3], r[4]};
				zero = zero && !(r[0] | r[1] | r[2] | r[3] | r[4]);
			}
			x.kind = (f & 4) || zero ? KIND_BLANK : (f & 2) ? KIND_PSEUDO : (f & 1) ? KIND_4OP : KIND_2OP;
			x.drum = (f >> 3) & 7;
			x.flags = (f & 0x40) ? PATCH_FIXED : 0;
		}
	}
	p.deep = b[17] & 3;
	p.volume_model = b[18];
	// the volume model, as the WOPL specification numbers it: 2 DMX .. 13 O'Connell -> the driver's models;
	// 1 (the chip's own logarithmic scale) -> hirav; 0 -> the driver's default, hirav (the specification
	// calls 0 generic; banks that name no model, DMXOPL3 among them, store 0); past 13 -> the default
	p.curve = b[18] >= 2 && b[18] <= 13 ? uint8_t(b[18] + 2) : b[18] == 1 ? uint8_t(CURVE_HIRAV) : uint8_t(CURVE_AUTO);
	return LOAD_OK;
}

void set_levels(Pool &p, const int8_t *programs, const KitLevels *kits, uint32_t nkits) {
	const KitLevels *kit0 = nullptr;
	for (uint32_t j = 0; kits && j < nkits; ++j) if (kits[j].kit == 0) kit0 = &kits[j];
	for (uint32_t i = 0; i < p.count; ++i) {
		Bank &b = p.banks[i];
		const int8_t *t = nullptr;
		if (!b.percussion) t = programs;
		else {
			t = kit0 ? kit0->levels : nullptr;
			for (uint32_t j = 0; kits && j < nkits; ++j) if (kits[j].kit == b.lsb) t = kits[j].levels;
		}
		for (uint32_t k = 0; k < 128; ++k) {
			const int32_t v = t ? t[k] : 0;
			b.patch[k].gain = int8_t(v < -16 ? -16 : v > 16 ? 16 : v);
		}
	}
}

// ---- the driver ---------------------------------------------------------------------------------------

Config default_config() {
	Config c;
	memset(&c, 0, sizeof c);
	c.chips = 1; c.rate = RATE_48K; c.curve = CURVE_AUTO;
	c.level_corrections = true; c.restrike = true; c.returning = true;
	c.deep_tremolo = -1; c.deep_vibrato = -1;
	return c;
}
void init(Driver &d, Pool &p, Chip *chips, const Config &c) {
	memset(&d, 0, sizeof d);
	d.pool = &p;
	for (auto &x : d.ch) x.host_perc = 0;
	configure(d, chips, c);
}
void configure(Driver &d, Chip *chips, const Config &c) {
	d.chips = chips;
	d.cfg = c;
	if (d.cfg.chips < 1) d.cfg.chips = 1;
	if (d.cfg.chips > MAX_CHIPS) d.cfg.chips = MAX_CHIPS;
	if (d.cfg.rate != RATE_96K) d.cfg.rate = RATE_48K;
	d.nslots = d.cfg.chips * CHANNELS;
	d.pending = P_INIT;
}
void update(Driver &d, Chip *chips, const Config &c) {
	Config n = c;
	if (n.chips < 1) n.chips = 1;
	if (n.chips > MAX_CHIPS) n.chips = MAX_CHIPS;
	if (n.rate != RATE_96K) n.rate = RATE_48K;
	const Config &o = d.cfg;
	constexpr uint32_t LIVE = HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION | NO_CLIP;   // set_flags, in direct mode
	const uint32_t flags = n.chip_flags ^ o.chip_flags;
	if (chips != d.chips || n.chips != o.chips || n.rate != o.rate || n.stream != o.stream || n.hard_pan != o.hard_pan ||
	    (flags & ~LIVE) || (flags && n.stream)) {
		configure(d, chips, c);
		return;
	}
	uint8_t p = 0;
	if (n.curve != o.curve || n.level_corrections != o.level_corrections || n.gain_remainder != o.gain_remainder) p |= P_LEVELS;
	if (n.deep_tremolo != o.deep_tremolo || n.deep_vibrato != o.deep_vibrato) p |= P_DEEP;
	if (flags) p |= P_FLAGS;
	d.cfg = n;
	d.pending |= p;
}
void set_curve_tables(Driver &d, const uint16_t *vel, const uint16_t *vol) {
	for (uint32_t i = 0; i < 128; ++i) { d.vel_table[i] = vel ? vel[i] : 0; d.vol_table[i] = vol ? vol[i] : 0; }
}
void set_percussion(Driver &d, uint32_t channel, int on) {
	if (channel >= MIDI_CHANNELS) return;
	Channel &c = d.ch[channel];
	c.host_perc = uint8_t(on < 0 ? 0 : on ? 2 : 1);
	if (on >= 0) { c.percussion = on > 0; c.xg_kit = 0; }
}
bool send(Driver &d, uint32_t offset, const uint8_t *m, uint32_t len) {
	if (!m || !len) return true;
	Event e = {offset, m[0], 0, 0, 0};
	if (m[0] == 0xF0) {
		if (!parse_sysex(m, len, e)) return true;
	} else if (m[0] < 0x80 || m[0] > 0xEF) {
		return true;
	} else {
		const uint32_t n = (m[0] & 0xE0) == 0xC0 ? 1 : 2;
		if (len < 1 + n) return true;
		e.a = m[1] & 127;
		e.b = n > 1 ? m[2] & 127 : 0;
	}
	if (d.qn >= QUEUE) return false;
	if (d.qn && e.offset < d.queue[d.qn - 1].offset) e.offset = d.queue[d.qn - 1].offset;
	d.queue[d.qn++] = e;
	return true;
}
bool send3(Driver &d, uint32_t offset, uint8_t status, uint8_t a, uint8_t b) {
	const uint8_t m[3] = {status, a, b};
	return send(d, offset, m, 3);
}
uint32_t render(Driver &d, const Output &o, uint32_t frames) {
	if (d.cfg.stream || !frames) return 0;
	d.out = o;
	d.sounded = 0;
	if (!o.add_dry) memset(o.dry, 0, size_t(frames) * 2 * sizeof(int32_t));
	run(d, frames);
	return d.sounded;
}
uint32_t render_stream(Driver &d, uint32_t *records, uint32_t capacity, uint32_t &count, uint32_t frames) {
	count = 0;
	if (!d.cfg.stream) return 0;
	d.rec = records; d.rec_cap = capacity; d.rec_n = 0;
	const uint32_t n = run(d, frames);
	count = d.rec_n;
	d.rec = nullptr; d.rec_cap = d.rec_n = 0;
	return n;
}
uint32_t attenuation(const Driver &d, uint8_t vel, uint8_t vol, uint8_t expr, uint8_t master) { return curve_steps(d, vel & 127, vol & 127, expr & 127, master & 127); }
uint32_t chip_flags(const Driver &d) { return d.cfg.chip_flags | (d.cfg.hard_pan ? uint32_t(NO_PAN_LAW) : 0u); }
uint32_t stream_reserve(const Driver &d) { return d.cfg.chips * CHANNELS * 20 + d.cfg.chips * 8 + 64; }
void reset(Driver &d) { d.pending |= P_RESET; }
void panic(Driver &d) { d.pending |= P_PANIC; }
uint32_t version() { return HVOPL3_DRV_VERSION_NUMBER; }

}  // namespace drv
}  // namespace hvopl3

extern "C" {
using namespace hvopl3;
uint32_t hvopl3_drv_version(void) { return drv::version(); }
size_t hvopl3_drv_size(void) { return sizeof(drv::Driver); }
size_t hvopl3_drv_config_size(void) { return sizeof(drv::Config); }
void hvopl3_drv_default_config(void *c) { *static_cast<drv::Config *>(c) = drv::default_config(); }
size_t hvopl3_drv_pool_bytes(uint32_t banks) { return drv::pool_bytes(banks); }
void *hvopl3_drv_pool_init(void *mem, size_t bytes) { return drv::pool_init(mem, bytes); }
int hvopl3_drv_load_wopl(void *p, const void *data, size_t size) { return drv::load_wopl(*static_cast<drv::Pool *>(p), data, size); }
void hvopl3_drv_set_levels(void *p, const int8_t *programs, const void *kits, uint32_t nkits) {
	drv::set_levels(*static_cast<drv::Pool *>(p), programs, static_cast<const drv::KitLevels *>(kits), nkits);
}
void hvopl3_drv_init(void *d, void *p, void *chips, const void *c) {
	drv::init(*static_cast<drv::Driver *>(d), *static_cast<drv::Pool *>(p), static_cast<Chip *>(chips), *static_cast<const drv::Config *>(c));
}
void hvopl3_drv_configure(void *d, void *chips, const void *c) {
	drv::configure(*static_cast<drv::Driver *>(d), static_cast<Chip *>(chips), *static_cast<const drv::Config *>(c));
}
void hvopl3_drv_update(void *d, void *chips, const void *c) {
	drv::update(*static_cast<drv::Driver *>(d), static_cast<Chip *>(chips), *static_cast<const drv::Config *>(c));
}
void hvopl3_drv_set_percussion(void *d, uint32_t ch, int on) { drv::set_percussion(*static_cast<drv::Driver *>(d), ch, on); }
int hvopl3_drv_send(void *d, uint32_t offset, const uint8_t *m, uint32_t len) { return drv::send(*static_cast<drv::Driver *>(d), offset, m, len); }
uint32_t hvopl3_drv_render(void *d, int32_t *dry, int32_t *rev, int32_t *cho, int add, uint32_t frames) {
	const Output o = {dry, rev, cho, add != 0};
	return drv::render(*static_cast<drv::Driver *>(d), o, frames);
}
uint32_t hvopl3_drv_render_stream(void *d, uint32_t *records, uint32_t capacity, uint32_t *count, uint32_t frames) {
	uint32_t n = 0;
	const uint32_t f = drv::render_stream(*static_cast<drv::Driver *>(d), records, capacity, n, frames);
	if (count) *count = n;
	return f;
}
void hvopl3_drv_reset(void *d) { drv::reset(*static_cast<drv::Driver *>(d)); }
void hvopl3_drv_panic(void *d) { drv::panic(*static_cast<drv::Driver *>(d)); }
}
