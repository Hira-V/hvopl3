// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// t_drv: the MIDI driver (drv/), against the chip's registers and extension state. Host only.
//
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <initializer_list>
#include "hvopl3.hpp"
#include "hvopl3_drv.hpp"
using namespace hvopl3;
using namespace hvopl3::drv;
static int fails = 0, checks = 0;
#define CHECK(x, ...) do { ++checks; if (!(x)) { ++fails; printf("FAIL %s:%d %s: ", __FILE__, __LINE__, #x); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Chip chips[MAX_CHIPS], chips2[MAX_CHIPS], chips3[MAX_CHIPS];
alignas(16) static uint8_t poolmem[1 << 18];
static Pool *pool;
static Driver d;
static int32_t D[2 * 8192], R[2 * 8192], C[2 * 8192];
static std::vector<uint8_t> wopl;

// ---- helpers ----
static uint32_t op_index(uint32_t ch, uint32_t n) { return 2 * ch + n; }   // the chip's operator array
static Patch sine(uint8_t tl = 0, uint8_t mark = 0) {   // a sustained sine; `mark` in the modulator's 0x60 to tell patches apart
	Patch p;
	memset(&p, 0, sizeof p);
	p.kind = KIND_2OP;
	p.op[0] = Operator{0x21, 0x3F, uint8_t(0xF0 | mark), 0x0F, 0};
	p.op[1] = Operator{0x21, tl, 0xF0, 0x0F, 0};
	return p;
}
static Config cfg1(uint32_t chips_n = 1) { Config c = default_config(); c.chips = chips_n; return c; }
static void setup(const Config &c) {
	init(d, *pool, chips, c);
	const Output o = {D, nullptr, nullptr, false};
	render(d, o, 64);
}
static uint32_t blk(uint32_t n = 64) { const Output o = {D, R, C, false}; memset(R, 0, n * 8); memset(C, 0, n * 8); return render(d, o, n); }
static void m3(uint8_t s, uint8_t a, uint8_t b = 0, uint32_t off = 0) { CHECK(send3(d, off, s, a, b), "queue"); }
static void cc(uint32_t ch, uint8_t c, uint8_t v) { m3(uint8_t(0xB0 | ch), c, v); }
static void on(uint32_t ch, uint8_t n, uint8_t v = 127, uint32_t at = 0) { m3(uint8_t(0x90 | ch), n, v, at); }
static void off(uint32_t ch, uint8_t n) { m3(uint8_t(0x80 | ch), n, 64); }
static void sx(std::initializer_list<uint8_t> m) { std::vector<uint8_t> v(m); CHECK(send(d, 0, v.data(), uint32_t(v.size())), "sysex"); }
// The chip channel a MIDI note sounds on (first found), or -1.
static int find_voice(uint32_t mc, uint32_t note, uint32_t from = 0) {
	for (uint32_t g = from; g < d.nslots; ++g) {
		const Slot &s = d.slot[g];
		if (s.state != 0 && s.midi == mc && s.note == note && (s.state == 1 || s.state == 2) && s.role != 3) return int(g);
	}
	return -1;
}
static bool keyed(uint32_t g) { return chips[g / 18].kon[g % 18]; }
static uint32_t carrier_tl(uint32_t g) { return chips[g / 18].r_40[op_index(g % 18, 1)] & 63; }
static uint32_t busy_total() { uint32_t n = 0; for (uint32_t k = 0; k < d.cfg.chips; ++k) n += d.busy[k]; return n; }
// The hirav attenuation by its formula, rounded once.
static uint32_t hirav(double v, double vol, double ex, double m) {
	const double db = 40 * std::log10(127 / v) + 40 * std::log10(127 / vol) + 40 * std::log10(127 / ex) + 40 * std::log10(127 / m);
	return uint32_t(std::floor(db / 0.75 + 0.5));
}
static double cents_of(uint16_t fnblk) {   // the chip's pitch, in cents above A4
	const double f = (fnblk & 1023) * (14318180.0 / 288) / std::pow(2.0, 20 - (fnblk >> 10));
	return 1200 * std::log2(f / 440);
}

// ---- step 2: one chip, 2-op voices, notes, program, volume, pan ----
static void t_notes() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine(0, 1));
	set_patch(*pool, false, 0, 0, 5, sine(10, 2));
	setup(cfg1());
	cc(0, 7, 127);
	on(0, 69);
	blk();
	const int g = find_voice(0, 69);
	CHECK(g >= 0, "a voice for A4");
	if (g < 0) return;
	CHECK(keyed(uint32_t(g)), "keyed");
	CHECK(chips[0].fnblk[g] == (4 << 10 | 0x244), "A4 is F-number 0x244, block 4: %x", chips[0].fnblk[g]);
	CHECK(carrier_tl(uint32_t(g)) == 0, "velocity, volume 127: no attenuation, tl %u", carrier_tl(uint32_t(g)));
	CHECK(chips[0].r_40[op_index(uint32_t(g), 0)] == 0x3F, "the modulator keeps its level");
	CHECK(chips[0].pan[g] == 64 && (chips[0].r_c0[g] & 0x30) == 0x30, "centred, soft pan");
	CHECK(blk() & SOUNDED, "it sounds");
	off(0, 69);
	blk();
	CHECK(!keyed(uint32_t(g)), "note off: key off");
	for (int i = 0; i < 50; ++i) blk(480);
	CHECK(busy_total() == 0 && d.free_mask[0] == 0x3FFFF, "silent: free again (%u busy)", busy_total());
	CHECK(!(blk() & SOUNDED), "and the chip is silent");
	// velocity 0 is a note off
	on(0, 60); blk(); CHECK(find_voice(0, 60) >= 0, "on");
	on(0, 60, 0); blk(); CHECK(find_voice(0, 60) < 0, "velocity 0 is off");
	// program change: for notes after it
	m3(0xC0, 5); on(0, 64); blk();
	const int g5 = find_voice(0, 64);
	CHECK(g5 >= 0 && (chips[0].r_60[op_index(uint32_t(g5), 0)] & 15) == 2 && carrier_tl(uint32_t(g5)) == 10, "program 5's patch");
	// velocity and volume: the hirav attenuation on the carrier
	cc(0, 7, 100); on(0, 65, 64); blk();
	const int gv = find_voice(0, 65);
	CHECK(gv >= 0 && carrier_tl(uint32_t(gv)) == 10 + hirav(64, 100, 127, 127), "velocity 64, volume 100: %u want %u", gv >= 0 ? carrier_tl(uint32_t(gv)) : 0, 10 + hirav(64, 100, 127, 127));
	cc(0, 11, 50); blk();
	CHECK(gv >= 0 && carrier_tl(uint32_t(gv)) == std::min(63u, 10 + hirav(64, 100, 50, 127)), "expression follows at once");
	cc(0, 7, 0); blk();
	CHECK(gv >= 0 && carrier_tl(uint32_t(gv)) == 63, "volume 0 is silent");
	cc(0, 7, 127); cc(0, 11, 127);
	// pan: soft through set_pan on every voice of the channel
	cc(0, 10, 20); blk();
	CHECK(chips[0].pan[g5] == 20 && chips[0].pan[gv] == 20, "pan 20 on both voices");
	cc(0, 91, 90); cc(0, 93, 30); blk();
	CHECK(chips[0].rev_q[g5] == (90u * 4096 + 63) / 127 && chips[0].cho_q[g5] == (30u * 4096 + 63) / 127, "sends");
	// hard pan: the C0 bits, the chip without its pan law
	Config hc = cfg1(); hc.hard_pan = true; setup(hc);
	const uint8_t pans[3] = {0, 64, 127}, bits[3] = {0x10, 0x30, 0x20};
	for (int i = 0; i < 3; ++i) {
		cc(0, 10, pans[i]); on(0, uint8_t(60 + i)); blk();
		const int h = find_voice(0, uint8_t(60 + i));
		CHECK(h >= 0 && (chips[0].r_c0[h] & 0x30) == bits[i], "hard pan %u: bits %x", pans[i], h >= 0 ? chips[0].r_c0[h] : 0);
	}
	CHECK(chips[0].flags & NO_PAN_LAW, "hard pan: no pan law");
	cc(0, 10, 127); blk();
	CHECK((chips[0].r_c0[find_voice(0, 60)] & 0x30) == 0x20, "a pan change moves a sounding voice");
}

// ---- step 3: banks and the WOPL loader ----
static uint32_t be16(const uint8_t *p) { return uint32_t(p[0]) << 8 | p[1]; }
static void t_wopl() {
	pool_clear(*pool);
	CHECK(load_wopl(*pool, wopl.data(), wopl.size()) == LOAD_OK, "DMXOPL3 loads");
	const uint8_t *b = wopl.data();
	const uint32_t ver = b[11] | b[12] << 8, mb = be16(b + 13), pb = be16(b + 15);
	CHECK(ver == 3 && mb == 11 && pb == 3, "version 3, 11 melodic banks, 3 kits: %u %u %u", ver, mb, pb);
	CHECK(pool->count == mb + pb, "every bank in the pool: %u", pool->count);
	CHECK(pool->deep == (b[17] & 3) && pool->volume_model == b[18] && pool->curve == CURVE_AUTO, "bank-wide flags, volume model auto");
	uint32_t bad = 0, n4 = 0, np = 0, n2 = 0, nb = 0;
	const uint8_t *e = b + 19 + 34 * (mb + pb);
	for (uint32_t i = 0; i < mb + pb; ++i) {
		const uint8_t lsb = b[19 + 34 * i + 32], msb = b[19 + 34 * i + 33];
		const bool perc = i >= mb;
		for (uint32_t k = 0; k < 128; ++k, e += 66) {
			const Patch *P = find_patch(*pool, perc, msb, lsb, uint8_t(k));
			if (!P) { ++bad; continue; }
			const uint8_t f = e[39];
			bool zero = !((e[40] | e[41]) & 15);
			for (int j = 42; j < 62; ++j) zero = zero && !e[j];
			const uint8_t kind = (f & 4) || zero ? KIND_BLANK : (f & 2) ? KIND_PSEUDO : (f & 1) ? KIND_4OP : KIND_2OP;
			bool ok = P->kind == kind && P->note_offset[0] == int16_t(be16(e + 32)) && P->note_offset[1] == int16_t(be16(e + 34)) &&
			          P->velocity_offset == int8_t(e[36]) && P->detune == int8_t(e[37]) && P->key == (e[38] & 127) &&
			          P->c0[0] == (e[40] & 15) && P->c0[1] == (e[41] & 15) && P->drum == ((f >> 3) & 7) && P->flags == ((f & 0x40) ? PATCH_FIXED : 0) && P->gain == 0;
			// chip order: modulator 1, carrier 1, modulator 2, carrier 2 from WOPL's carrier-first list
			const uint8_t *op[4] = {e + 47, e + 42, e + 57, e + 52};
			for (int o = 0; o < 4; ++o)
				ok = ok && P->op[o].r20 == op[o][0] && P->op[o].r40 == op[o][1] && P->op[o].r60 == op[o][2] && P->op[o].r80 == op[o][3] && P->op[o].rE0 == op[o][4];
			bad += !ok;
			n4 += kind == KIND_4OP; np += kind == KIND_PSEUDO; n2 += kind == KIND_2OP; nb += kind == KIND_BLANK;
		}
	}
	CHECK(bad == 0, "%u patches differ from the file", bad);
	CHECK(n4 > 0 && np > 0 && n2 > 0 && nb > 0, "every kind: %u 4-op, %u pseudo, %u 2-op, %u blank", n4, np, n2, nb);
	CHECK(find_bank(*pool, true, 0, 16) && find_bank(*pool, true, 0, 25) && find_bank(*pool, true, 0, 0), "kits 0, 16, 25");
	CHECK(find_bank(*pool, false, 8, 0) && !find_bank(*pool, false, 9, 0), "GS bank 8, no bank 9");
	// loading again changes nothing (the same banks); a full pool refuses, and changes nothing
	CHECK(load_wopl(*pool, wopl.data(), wopl.size()) == LOAD_OK && pool->count == mb + pb, "reload in place");
	static uint8_t small[1 << 16];
	Pool *sp = pool_init(small, pool_bytes(5));
	CHECK(sp && sp->capacity == 5, "a pool of 5");
	CHECK(load_wopl(*sp, wopl.data(), wopl.size()) == LOAD_FULL && sp->count == 0, "too many banks: refused whole");
	Patch q = sine();
	for (uint32_t i = 0; i < 5; ++i) CHECK(set_patch(*sp, false, uint8_t(i), 0, 0, q), "set_patch %u", i);
	CHECK(!set_patch(*sp, false, 9, 0, 0, q) && set_patch(*sp, false, 3, 0, 7, q), "full: a new bank fails, an old one takes patches");
	// truncated at every length up to the first patches, and at random lengths: refused, the pool untouched
	pool_clear(*sp);
	uint32_t wrong = 0;
	for (size_t n = 0; n < 19 + 34 * 14 + 200; ++n) wrong += load_wopl(*sp, wopl.data(), n) >= 0 || sp->count;
	srand(7);
	for (int i = 0; i < 200; ++i) { const size_t n = size_t(rand()) % wopl.size(); wrong += load_wopl(*sp, wopl.data(), n) >= 0 || sp->count; }
	CHECK(wrong == 0, "%u truncations loaded", wrong);
	std::vector<uint8_t> w = wopl;
	w[0] = 'X'; CHECK(load_wopl(*pool, w.data(), w.size()) == LOAD_FORMAT, "magic");
	w = wopl; w[11] = 4; CHECK(load_wopl(*pool, w.data(), w.size()) == LOAD_VERSION, "version 4");
	w[11] = 0; CHECK(load_wopl(*pool, w.data(), w.size()) == LOAD_VERSION, "version 0");
	w = wopl; w[19 + 33] = 200; CHECK(load_wopl(*pool, w.data(), w.size()) == LOAD_FORMAT, "an MSB past 127");
	w = wopl; w[14] = 200; CHECK(load_wopl(*pool, w.data(), w.size()) == LOAD_TRUNCATED, "more banks than the file holds");
	CHECK(load_wopl(*pool, nullptr, 100) < 0, "null");
	// a version 1 file: no metadata, melodic bank i at MSB i, kit i at LSB i, 62-byte entries
	std::vector<uint8_t> v1(b, b + 19);
	v1[11] = 1; v1[12] = 0; v1[13] = 0; v1[14] = 2; v1[15] = 0; v1[16] = 1; v1[18] = 1;
	for (uint32_t i = 0; i < 3; ++i)
		for (uint32_t k = 0; k < 128; ++k) { const uint8_t *x = b + 19 + 34 * 14 + 66 * (128 * (i == 2 ? 11 : i) + k); v1.insert(v1.end(), x, x + 62); }
	pool_clear(*sp);
	CHECK(load_wopl(*sp, v1.data(), v1.size()) == LOAD_OK && sp->count == 3, "version 1 loads");
	const Patch *a = find_patch(*sp, false, 1, 0, 0), *a0 = find_patch(*pool, false, 8, 0, 0);
	CHECK(a && a0 && !memcmp(a, a0, sizeof *a), "version 1: bank 1 is the file's second (MSB 8 in the v3 file)");
	CHECK(find_patch(*sp, true, 0, 0, 36) && sp->curve == CURVE_HIRAV, "version 1: its kit at LSB 0; volume model 1 (native) is hirav");
	// the volume models as the WOPL specification numbers them: 2 DMX .. 13 O'Connell; past 13 the default
	for (uint32_t vm : {2u, 3u, 9u, 13u, 14u}) {
		v1[18] = uint8_t(vm); pool_clear(*sp);
		CHECK(load_wopl(*sp, v1.data(), v1.size()) == LOAD_OK && sp->curve == (vm <= 13 ? vm + 2 : uint32_t(CURVE_AUTO)),
		      "volume model %u: curve %u", vm, sp->curve);
	}
}
// Malformed files: random damage to DMXOPL3, every load bounded (run under the sanitizers: tests/Makefile's t_drv_asan).
static void t_wopl_fuzz() {
	static uint8_t mem[1 << 17];
	Pool *sp = pool_init(mem, sizeof mem);
	std::vector<uint8_t> w;
	uint32_t seed = 12345, loaded = 0;
	const auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
	for (int i = 0; i < 3000; ++i) {
		w = wopl;
		const uint32_t n = 1 + rnd() % 8;
		for (uint32_t j = 0; j < n; ++j) {
			const uint32_t at = rnd() % (j < 2 ? 19 + 34 * 14 : uint32_t(w.size()));   // the header and metadata most
			w[at] = uint8_t(rnd());
		}
		size_t len = w.size();
		if (rnd() % 3 == 0) len = rnd() % (w.size() + 1);
		std::vector<uint8_t> exact(w.begin(), w.begin() + long(len));   // the buffer is exactly `len`: an overread is the sanitizer's
		pool_clear(*sp);
		loaded += load_wopl(*sp, exact.data(), exact.size()) == LOAD_OK;
	}
	for (int i = 0; i < 500; ++i) {   // noise behind a good magic
		std::vector<uint8_t> x(19 + rnd() % 4000);
		for (auto &c : x) c = uint8_t(rnd());
		memcpy(x.data(), wopl.data(), 13);
		pool_clear(*sp);
		loaded += load_wopl(*sp, x.data(), x.size()) == LOAD_OK;
	}
	CHECK(loaded > 0, "some damaged files still load (%u)", loaded);
}

// ---- step 4: 4-op and pseudo-4-op voices, percussion ----
static void t_kinds() {
	pool_clear(*pool);
	CHECK(load_wopl(*pool, wopl.data(), wopl.size()) == LOAD_OK, "load");
	// 4-op: each connection's carriers take the attenuation, the rest keep the bank's level
	for (uint32_t conn = 0; conn < 4; ++conn) {
		Patch p;
		memset(&p, 0, sizeof p);
		p.kind = KIND_4OP;
		for (int o = 0; o < 4; ++o) p.op[o] = Operator{0x21, uint8_t(5 + o), 0xF0, uint8_t(0x0F | o << 4), uint8_t(o)};
		p.c0[0] = uint8_t(conn & 1); p.c0[1] = uint8_t(conn >> 1 | 6);
		set_patch(*pool, false, 50, 0, 1, p);
		setup(cfg1());
		cc(0, 0, 50); m3(0xC0, 1); on(0, 60, 64); blk();
		const int g = find_voice(0, 60);
		CHECK(g >= 0 && d.slot[g].role == 2, "a 4-op voice");
		if (g < 0) continue;
		const uint32_t ch = uint32_t(g) % 18, pair = ch < 3 ? ch : ch - 6;
		CHECK(ch % 9 < 3 && (chips[0].r_104 >> pair & 1), "on a pair's first channel, in 4-op mode: ch %u, 104 %x", ch, chips[0].r_104);
		static const uint8_t HEARD[4] = {8, 9, 10, 13};
		const uint32_t att = hirav(64, 100, 127, 127);
		bool ok = true;
		for (uint32_t o = 0; o < 4; ++o) {
			const uint32_t i = op_index(ch + (o >= 2 ? 3 : 0), o & 1);
			const uint32_t want = (HEARD[conn] >> o & 1) ? std::min(63u, 5 + o + att) : 5 + o;
			ok = ok && (chips[0].r_40[i] & 63) == want && chips[0].wave[i] == o && (chips[0].r_80[i] >> 4) == o;
		}
		CHECK(ok, "connection %u: carriers attenuated, modulators as the bank", conn);
		CHECK((chips[0].r_c0[ch] & 1) == (conn & 1) && (chips[0].r_c0[ch + 3] & 15) == ((conn >> 1) | 6), "both C0s");
		CHECK(chips[0].kon[ch] && chips[0].active >> ch & 1, "keyed and sounding");
		off(0, 60); for (int i = 0; i < 40; ++i) blk(480);
		CHECK(busy_total() == 0, "both channels free after");
		// with the pair free, a 2-op voice may take it back
	}
	// pseudo-4-op: DMXOPL3's piano is two 2-op voices, the second detuned
	setup(cfg1());
	cc(0, 7, 127); m3(0xC0, 0); on(0, 60); blk();
	const int g = find_voice(0, 60);
	CHECK(g >= 0 && d.slot[g].role == 4, "pseudo-4-op: its first voice");
	if (g >= 0) {
		const int h = d.slot[g].partner;
		const Patch *P = find_patch(*pool, false, 0, 0, 0);
		CHECK(h >= 0 && h != NONE && d.slot[h].role == 5 && keyed(uint32_t(h)), "and its second, keyed");
		const double c1 = cents_of(chips[0].fnblk[g % 18]), c2 = cents_of(chips[0].fnblk[h % 18]);
		const double want1 = (60 + P->note_offset[0] - 69) * 100.0, want2 = (60 + P->note_offset[1] - 69) * 100.0 + ((((P->detune + 128) >> 1) - 64) / 32.0) * 100;
		CHECK(std::fabs(c1 - want1) < 1.7 && std::fabs(c2 - want2) < 1.7, "pitches %f %f want %f %f", c1, c2, want1, want2);
		CHECK((chips[0].r_c0[g % 18] & 15) == P->c0[0] && (chips[0].r_c0[h % 18] & 15) == P->c0[1], "each half its own C0");
		CHECK(chips[0].r_20[op_index(uint32_t(h) % 18, 0)] == P->op[2].r20 && chips[0].r_20[op_index(uint32_t(h) % 18, 1)] == P->op[3].r20, "the second voice's operators");
		off(0, 60); blk();
		CHECK(!keyed(uint32_t(g)) && !keyed(uint32_t(h)), "a note off releases both");
	}
	// percussion: channel 10's note 36 is kit 0's patch 36, at its key (and note offset)
	setup(cfg1());
	on(9, 36); blk();
	const int k = find_voice(9, 36);
	const Patch *dr = find_patch(*pool, true, 0, 0, 36);
	CHECK(k >= 0 && dr, "a drum");
	if (k >= 0 && dr) {
		const double c = cents_of(chips[0].fnblk[k % 18]), want = (dr->key + dr->note_offset[0] - 69) * 100.0;
		CHECK(std::fabs(c - want) < 1.7, "drum 36 at key %u + %d: %f cents, want %f", dr->key, dr->note_offset[0], c, want);
		CHECK(chips[0].r_60[op_index(uint32_t(k) % 18, 1)] == dr->op[1].r60, "kit 0's patch");
	}
	// kit 16 (program 16): its own snare; a key it lacks falls back to kit 0's
	m3(0xC9, 16); on(9, 38); on(9, 60); blk();
	const Patch *s16 = find_patch(*pool, true, 0, 16, 38);
	const int k38 = find_voice(9, 38), k60 = find_voice(9, 60);
	CHECK(k38 >= 0 && s16 && chips[0].r_20[op_index(uint32_t(k38) % 18, 1)] == s16->op[1].r20 && d.slot[k38].role == (s16->kind == KIND_4OP ? 2 : s16->kind == KIND_PSEUDO ? 4 : 1), "kit 16's snare");
	const Patch *b16 = find_patch(*pool, true, 0, 16, 60), *b0 = find_patch(*pool, true, 0, 0, 60);
	if (b16 && b16->kind == KIND_BLANK && b0 && b0->kind != KIND_BLANK)
		CHECK(k60 >= 0 && chips[0].r_20[op_index(uint32_t(k60) % 18, 1)] == b0->op[1].r20, "kit 16's blank key: kit 0's");
	// a kit that does not exist: kit 0
	m3(0xC9, 5); on(9, 42); blk();
	const int k42 = find_voice(9, 42);
	const Patch *h0 = find_patch(*pool, true, 0, 0, 42);
	CHECK(k42 >= 0 && h0 && chips[0].r_20[op_index(uint32_t(k42) % 18, 1)] == h0->op[1].r20, "no kit 5: kit 0");
	// GS variations: a blank program in bank 8 falls back to bank 0's; a real one is used
	const Bank *v8 = find_bank(*pool, false, 8, 0);
	int real = -1, blank = -1;
	for (int i = 0; i < 128 && v8; ++i) {
		if (v8->patch[i].kind == KIND_BLANK && blank < 0) blank = i;
		if (v8->patch[i].kind == KIND_2OP && real < 0) real = i;
	}
	CHECK(real >= 0 && blank >= 0, "bank 8 has both");
	if (real >= 0 && blank >= 0) {
		setup(cfg1());
		cc(1, 0, 8); m3(0xC1, uint8_t(blank)); on(1, 50); blk();
		int x = find_voice(1, 50);
		const Patch *p0 = find_patch(*pool, false, 0, 0, uint8_t(blank));
		CHECK(x >= 0 && chips[0].r_20[op_index(uint32_t(x) % 18, 1)] == p0->op[1].r20, "blank variation: bank 0's program");
		m3(0xC1, uint8_t(real)); on(1, 51); blk();
		x = find_voice(1, 51);
		CHECK(x >= 0 && chips[0].r_20[op_index(uint32_t(x) % 18, 1)] == v8->patch[real].op[1].r20 && chips[0].r_60[op_index(uint32_t(x) % 18, 1)] == v8->patch[real].op[1].r60, "a real variation is played");
	}
	// a missing bank's program that is blank in bank 0 too: silent, nothing allocated
	Patch blankp; memset(&blankp, 0, sizeof blankp);
	set_patch(*pool, false, 0, 0, 120, blankp);
	setup(cfg1());
	m3(0xC2, 120); on(2, 60); blk();
	CHECK(find_voice(2, 60) < 0 && busy_total() == 0, "a blank patch: no voice");
}

// ---- step 5: controllers, RPN, SysEx, percussion channels ----
static void gm_pool() { pool_clear(*pool); load_wopl(*pool, wopl.data(), wopl.size()); }
static void ticks(int n) { for (int i = 0; i < n; ++i) blk(64); }
static double voice_cents(int g) { return g < 0 ? 1e9 : cents_of(chips[g / 18].fnblk[g % 18]); }
static void t_pedals() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine());
	setup(cfg1());
	// sustain holds a released note until the pedal lifts
	on(0, 60); cc(0, 64, 127); blk(); off(0, 60); blk();
	int g = find_voice(0, 60);
	CHECK(g >= 0 && keyed(uint32_t(g)) && d.slot[g].state == 2, "sustained after note off");
	cc(0, 64, 0); blk();
	CHECK(g >= 0 && !keyed(uint32_t(g)), "pedal up: released");
	// sostenuto: holds only the notes down when it was pressed
	on(0, 62); blk(); cc(0, 66, 127); on(0, 64); blk();
	const int a = find_voice(0, 62), b = find_voice(0, 64);
	off(0, 62); off(0, 64); blk();
	CHECK(a >= 0 && keyed(uint32_t(a)), "sostenuto holds the note that was down");
	CHECK(b >= 0 && !keyed(uint32_t(b)), "not one played after");
	cc(0, 66, 0); blk();
	CHECK(a >= 0 && !keyed(uint32_t(a)), "sostenuto up: released");
	// a note held only by the sustain pedal (key up) is not taken by sostenuto
	cc(0, 64, 127); on(0, 63); blk(); off(0, 63); blk(); cc(0, 66, 127); blk(); cc(0, 64, 0); blk();
	const int q = find_voice(0, 63);
	CHECK(q < 0, "sostenuto does not hold a note whose key was up");
	cc(0, 66, 0); blk();
	// both pedals: the note stays until both let go
	on(0, 65); blk(); cc(0, 66, 127); cc(0, 64, 127); off(0, 65); blk();
	const int c = find_voice(0, 65);
	cc(0, 66, 0); blk(); CHECK(c >= 0 && keyed(uint32_t(c)), "sustain still holds it");
	cc(0, 64, 0); blk(); CHECK(c >= 0 && !keyed(uint32_t(c)), "both up: released");
	// all notes off respects the sustain pedal; all sound off does not
	on(0, 67); on(0, 69); blk(); cc(0, 64, 127); cc(0, 123, 0); blk();
	const int e = find_voice(0, 67), f = find_voice(0, 69);
	CHECK(e >= 0 && f >= 0 && keyed(uint32_t(e)) && keyed(uint32_t(f)), "all notes off under sustain: held");
	cc(0, 64, 0); blk();
	CHECK(!keyed(uint32_t(e)) && !keyed(uint32_t(f)), "and released with the pedal");
	for (uint8_t x : {124, 125, 126, 127}) {
		on(0, 70); blk(); const int h = find_voice(0, 70);
		cc(0, x, 0); blk();
		CHECK(h >= 0 && !keyed(uint32_t(h)), "CC %u is all notes off", x);
	}
	on(0, 71); cc(0, 64, 127); blk(); const int h = find_voice(0, 71);
	cc(0, 120, 0); blk();
	CHECK(h >= 0 && !keyed(uint32_t(h)) && (chips[0].r_80[op_index(uint32_t(h) % 18, 1)] & 15) == 15, "all sound off: cut with release rate 15, pedal or not");
	cc(0, 64, 0);
	// re-strike: a sounding note's voice played again, not a second voice
	ticks(40);
	on(0, 72); blk(); const int r1 = find_voice(0, 72);
	on(0, 72, 90); blk(1);
	CHECK(r1 >= 0 && !keyed(uint32_t(r1)), "re-strike: its key off first");
	ticks(1);
	CHECK(find_voice(0, 72) == r1 && keyed(uint32_t(r1)) && d.slot[r1].vel == 90, "then on again, the same voice");
	uint32_t n72 = 0; for (uint32_t x = 0; x < d.nslots; ++x) n72 += d.slot[x].note == 72 && d.slot[x].state == 1;
	CHECK(n72 == 1, "one voice for note 72: %u", n72);
	Config nr = cfg1(); nr.restrike = false; setup(nr);
	on(0, 72); blk(); on(0, 72); blk();
	n72 = 0; for (uint32_t x = 0; x < d.nslots; ++x) n72 += d.slot[x].note == 72 && d.slot[x].state == 1;
	CHECK(n72 == 2, "restrike off: two voices stack (%u)", n72);
	off(0, 72); blk();
	n72 = 0; for (uint32_t x = 0; x < d.nslots; ++x) n72 += d.slot[x].note == 72 && d.slot[x].state == 1;
	CHECK(n72 == 1, "a note off takes the oldest");
}
static void t_rpn() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine());
	setup(cfg1());
	on(0, 69); blk();
	const int g = find_voice(0, 69);
	CHECK(g >= 0 && std::fabs(voice_cents(g)) < 1.7, "A4");
	m3(0xE0, 127, 127); blk(); ticks(1);
	CHECK(std::fabs(voice_cents(g) - 200 * 8191.0 / 8192) < 1.7, "bend up, default range 2: %f", voice_cents(g));
	cc(0, 101, 0); cc(0, 100, 0); cc(0, 6, 12); cc(0, 38, 50); ticks(2);
	CHECK(std::fabs(voice_cents(g) - 1250 * 8191.0 / 8192) < 1.7, "RPN 0 = 12 semitones 50 cents: %f", voice_cents(g));
	cc(0, 96, 0); ticks(1);
	CHECK(d.ch[0].bend_semis == 12 && d.ch[0].bend_cents == 51, "increment: a cent");
	cc(0, 97, 0); cc(0, 97, 0); ticks(1);
	CHECK(d.ch[0].bend_cents == 49, "decrement");
	m3(0xE0, 0, 64); cc(0, 101, 127); cc(0, 100, 127); cc(0, 6, 3); ticks(2);
	CHECK(d.ch[0].bend_semis == 12 && std::fabs(voice_cents(g)) < 1.7, "RPN null: data entry ignored; bend centre");
	cc(0, 99, 1); cc(0, 98, 0); cc(0, 6, 5); ticks(1);
	CHECK(d.ch[0].bend_semis == 12 && d.ch[0].fine == 8192, "an NRPN is consumed");
	cc(0, 101, 0); cc(0, 100, 1); cc(0, 6, 96); cc(0, 38, 0); ticks(2);   // fine: 96 << 7 = 12288: +50 cents
	CHECK(std::fabs(voice_cents(g) - 50) < 1.7, "RPN 1 fine tuning +50 cents: %f", voice_cents(g));
	cc(0, 100, 2); cc(0, 6, 64 - 3); ticks(2);
	CHECK(std::fabs(voice_cents(g) - (50 - 300)) < 1.7, "RPN 2 coarse -3: %f", voice_cents(g));
	// Reset All Controllers: bend, modulation, expression, pedals, the RPN selection; not volume, pan, sends, RPN values
	cc(0, 7, 90); cc(0, 10, 30); cc(0, 91, 70); cc(0, 1, 100); cc(0, 11, 40); cc(0, 64, 127); m3(0xE0, 0, 100); ticks(1);
	cc(0, 121, 0); ticks(1);
	const Channel &c = d.ch[0];
	CHECK(c.modulation == 0 && c.expression == 127 && c.sustain == 0 && c.bend == 8192 && c.rpn_msb == 127 && c.rpn_lsb == 127, "reset controllers");
	CHECK(c.volume == 90 && c.pan == 30 && c.reverb == 70 && c.bend_semis == 12 && c.fine == 96 << 7 && c.coarse == -3, "kept");
	// master volume and tuning (universal real-time SysEx)
	cc(0, 7, 127); ticks(1);
	sx({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, 64, 0xF7}); ticks(1);
	CHECK(carrier_tl(uint32_t(g)) == hirav(127, 127, 127, 64), "master volume 64: %u", carrier_tl(uint32_t(g)));
	cc(0, 101, 0); cc(0, 100, 1); cc(0, 6, 64); cc(0, 38, 0); cc(0, 100, 2); cc(0, 6, 64); ticks(2);
	sx({0xF0, 0x7F, 0x7F, 0x04, 0x03, 0x00, 0x50, 0xF7}); ticks(2);   // 0x2800: +25 cents
	CHECK(std::fabs(voice_cents(g) - 25) < 1.7, "master fine +25: %f", voice_cents(g));
	sx({0xF0, 0x7F, 0x7F, 0x04, 0x04, 0x00, 66, 0xF7}); ticks(2);
	CHECK(std::fabs(voice_cents(g) - 225) < 1.7, "master coarse +2: %f", voice_cents(g));
	// the resets: every controller its default, notes off
	cc(0, 7, 20); m3(0xC0, 5); on(0, 50); blk();
	const int h = find_voice(0, 50);
	sx({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}); blk();
	CHECK(d.ch[0].volume == 100 && d.ch[0].program == 0 && d.master_volume == 127 && d.master_coarse == 0 && (h < 0 || !keyed(uint32_t(h))), "GM System On");
	const std::initializer_list<uint8_t> resets[] = {
		{0xF0, 0x7E, 0x7F, 0x09, 0x02, 0xF7}, {0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7},
		{0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7}, {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7}};
	for (const auto &r : resets) {
		cc(3, 7, 20); cc(3, 1, 50); blk();
		sx(r); blk();
		CHECK(d.ch[3].volume == 100 && d.ch[3].modulation == 0, "reset %x %x", r.begin()[1], r.begin()[4]);
	}
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x40, 0xF7});   // a bad checksum: ignored
	cc(3, 7, 20); blk(); sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x40, 0xF7}); blk();
	CHECK(d.ch[3].volume == 20, "a GS message with a bad checksum is ignored");
}
static bool drums_on(uint32_t ch) { return d.ch[ch].percussion; }
static void t_percussion() {
	gm_pool();
	setup(cfg1(2));
	// two drum channels at once, each its own kit: 10 (default) and 11 (GM2 bank MSB 120, kit 16)
	cc(10, 0, 120); m3(0xCA, 16); m3(0xC9, 0);
	on(9, 38); on(10, 38); blk();
	const Patch *k0 = find_patch(*pool, true, 0, 0, 38), *k16 = find_patch(*pool, true, 0, 16, 38);
	int a = find_voice(9, 38), b = find_voice(10, 38);
	CHECK(drums_on(10) && a >= 0 && b >= 0, "channels 10 and 11 both drums");
	if (a >= 0 && b >= 0) CHECK(chips[a / 18].r_20[op_index(uint32_t(a) % 18, 1)] == k0->op[1].r20 && chips[b / 18].r_20[op_index(uint32_t(b) % 18, 1)] == k16->op[1].r20, "kit 0 and kit 16");
	cc(10, 0, 121); m3(0xCA, 0); blk();
	CHECK(!drums_on(10), "GM2 MSB 121: melodic again");
	// 10 and 16 by GS: part 16 (block 0x1F) to rhythm, and off
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x02, uint8_t((128 - ((0x40 + 0x1F + 0x15 + 0x02) & 127)) & 127), 0xF7}); blk();
	CHECK(drums_on(15), "GS: part 16 rhythm (map 2)");
	on(15, 42); on(9, 42); blk();
	CHECK(find_voice(15, 42) >= 0 && find_voice(9, 42) >= 0 && d.slot[find_voice(15, 42)].patch == find_patch(*pool, true, 0, 0, 42), "16 and 10 both play drums");
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1F, 0x15, 0x00, uint8_t((128 - ((0x40 + 0x1F + 0x15) & 127)) & 127), 0xF7}); blk();
	CHECK(!drums_on(15), "GS off: melodic");
	// channel 10 made melodic by each route, and back
	cc(9, 0, 121); m3(0xC9, 0); blk(); CHECK(!drums_on(9), "GM2: channel 10 melodic");
	cc(9, 0, 120); m3(0xC9, 0); blk(); CHECK(drums_on(9), "GM2: channel 10 drums again");
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x00, uint8_t((128 - ((0x40 + 0x10 + 0x15) & 127)) & 127), 0xF7}); blk();
	CHECK(!drums_on(9), "GS: part 10 off");
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x10, 0x15, 0x01, uint8_t((128 - ((0x40 + 0x10 + 0x15 + 1) & 127)) & 127), 0xF7}); blk();
	CHECK(drums_on(9), "GS: part 10 map 1");
	// XG: after an XG reset, channel 10 is as if MSB 127 were selected; another MSB makes it melodic
	sx({0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7}); blk();
	CHECK(drums_on(9), "XG reset: channel 10 drums");
	cc(9, 0, 0); m3(0xC9, 0); blk(); CHECK(!drums_on(9), "XG: MSB 0 makes channel 10 melodic");
	cc(4, 0, 127); m3(0xC4, 0); blk(); CHECK(drums_on(4), "XG: MSB 127 on channel 5: drums");
	cc(4, 0, 0); m3(0xC4, 3); blk(); CHECK(!drums_on(4), "and MSB 0 back");
	cc(5, 0, 126); m3(0xC5, 0); blk(); CHECK(drums_on(5), "XG: MSB 126 (SFX kit): drums");
	// a GM file's MSB 0 on channel 10 (no XG reset) keeps its drums
	sx({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}); blk();
	CHECK(drums_on(9) && !drums_on(4) && !drums_on(5) && !drums_on(15), "GM reset: channel 10 only");
	cc(9, 0, 0); cc(9, 32, 0); m3(0xC9, 0); on(9, 36); blk();
	CHECK(drums_on(9) && find_voice(9, 36) >= 0 && d.slot[find_voice(9, 36)].patch == find_patch(*pool, true, 0, 0, 36), "GM: MSB 0 on channel 10 keeps the drums");
	cc(9, 0, 5); m3(0xC9, 0); blk();
	CHECK(drums_on(9), "any MSB, outside XG: still drums");
	// the host: on and off on any channel, kept across resets until it lets go; never by inference
	set_percussion(d, 2, 1); set_percussion(d, 9, 0); blk();
	CHECK(drums_on(2) && !drums_on(9), "host: 3 drums, 10 melodic");
	sx({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}); blk();
	cc(2, 121, 0); blk();
	CHECK(drums_on(2) && !drums_on(9), "kept across a GM reset and reset all controllers");
	cc(2, 0, 121); m3(0xC2, 0); blk();
	CHECK(drums_on(2), "the host's setting wins over MIDI's");
	set_percussion(d, 2, -1); set_percussion(d, 9, -1);
	sx({0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}); blk();
	CHECK(!drums_on(2) && drums_on(9), "host lets go: the default after a reset");
	cc(9, 121, 0); blk(); CHECK(drums_on(9), "reset all controllers keeps the flag");
	uint32_t inferred = 0;
	for (uint32_t i = 0; i < 16; ++i) { if (i == 9) continue; m3(uint8_t(0xC0 | i), 0); on(i, 36); blk(); inferred += drums_on(i); }
	CHECK(inferred == 0, "no channel becomes drums by playing drum notes");
	// on a drum channel, Program Change picks the kit for notes after it
	m3(0xC9, 25); on(9, 38); blk(); ticks(4);   // every channel busy: a steal, started a period or two later
	const int x = find_voice(9, 38);
	CHECK(x >= 0 && d.slot[x].patch == find_patch(*pool, true, 0, 25, 38) && find_patch(*pool, true, 0, 25, 38)->kind != KIND_BLANK, "kit 25 by program 25");
}

// ---- step 6: pitch, bend, vibrato, portamento ----
// Is fnblk the chip's best for `cents` (above A4)? Within 1 cent, or within half a step of the nearest
// F-number where the chip's steps are coarser; targets past the chip's range land on its last value.
static bool pitch_ok(uint16_t fb, double cents, double *err) {
	const double f = 440 * std::pow(2.0, cents / 1200), clk = 14318180.0 / 288;
	const double top = 1023 * clk / std::pow(2.0, 13);
	if (f >= top) { *err = 0; return fb == (7 << 10 | 1023); }
	*err = cents_of(fb) - cents;
	const uint32_t fn = fb & 1023;
	const double half = fn ? 1200 * std::log2((fn + 0.5) / fn) : 1e9;
	return std::fabs(*err) <= std::max(1.0, half) + 0.01;
}
static void t_pitch() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine());
	setup(cfg1());
	uint32_t bad = 0; double worst = 0;
	for (uint32_t n = 0; n < 128; ++n) {
		on(0, uint8_t(n)); blk();
		const int g = find_voice(0, uint8_t(n));
		double e = 0;
		if (g < 0 || !pitch_ok(chips[0].fnblk[g % 18], (double(n) - 69) * 100, &e)) { ++bad; printf("  note %u: %x err %f\n", n, g < 0 ? 0 : chips[0].fnblk[g % 18], e); }
		if (g >= 0 && (chips[0].fnblk[g % 18] & 1023) >= 866) worst = std::max(worst, std::fabs(e));
		off(0, uint8_t(n)); blk();
		ticks(30);
	}
	CHECK(bad == 0, "%u notes off their pitch", bad);
	CHECK(worst < 1.0, "within a cent where the steps allow: worst %f", worst);
	// bend extremes at three ranges, across the keyboard
	bad = 0;
	for (uint8_t range : {2, 12, 24})
		for (uint8_t n : {0, 21, 45, 69, 90, 108, 127})
			for (uint32_t b : {0u, 16383u}) {
				setup(cfg1());
				cc(0, 101, 0); cc(0, 100, 0); cc(0, 6, range); cc(0, 38, 0);
				m3(0xE0, uint8_t(b & 127), uint8_t(b >> 7));
				on(0, n); blk(); ticks(1);
				const int g = find_voice(0, n);
				const double want = (double(n) - 69) * 100 + range * 100.0 * (double(b) - 8192) / 8192;
				double e = 0;
				if (g < 0 || !pitch_ok(chips[0].fnblk[g % 18], want, &e)) { ++bad; printf("  bend %u range %u note %u: err %f\n", b, range, n, e); }
			}
	CHECK(bad == 0, "%u bends off", bad);
	// a bend is applied at the control period, not at once
	setup(cfg1());
	on(0, 60); blk(32);
	const int g = find_voice(0, 60);
	const uint16_t was = chips[0].fnblk[g % 18];
	m3(0xE0, 0, 96, 1); blk(16);
	CHECK(chips[0].fnblk[g % 18] == was, "not before the period");
	blk(16);
	CHECK(chips[0].fnblk[g % 18] == was, "not before the period");
	blk(1);   // the period at frame 128
	CHECK(chips[0].fnblk[g % 18] != was && std::fabs(voice_cents(g) - (-900 + 100)) < 1.7, "at the period: %f", voice_cents(g));
}
static void t_vibrato() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine());
	setup(cfg1());
	on(0, 69); blk();
	const int g = find_voice(0, 69);
	ticks(10);
	CHECK(std::fabs(voice_cents(g)) < 1.7, "no vibrato at CC 1 = 0");
	cc(0, 1, 127);
	double lo = 1e9, hi = -1e9;
	int ups = 0; double prev = 0; bool rising = false;
	std::vector<double> v;
	for (int i = 0; i < 750; ++i) { ticks(1); const double c = voice_cents(g); v.push_back(c); lo = std::min(lo, c); hi = std::max(hi, c); }
	for (size_t i = 1; i < v.size(); ++i) { const bool r = v[i] > prev + 0.5 || (v[i] >= prev - 0.5 && rising); if (r && !rising) ++ups; rising = r; prev = v[i]; }
	CHECK(hi > 46 && hi < 52 && lo < -46 && lo > -52, "CC 1 = 127: +-50 cents: %f %f", lo, hi);
	CHECK(ups >= 4 && ups <= 7, "about 5.5 Hz: %d rises in a second", ups);
	cc(0, 1, 64); ticks(200);
	lo = 1e9; hi = -1e9;
	for (int i = 0; i < 200; ++i) { ticks(1); lo = std::min(lo, voice_cents(g)); hi = std::max(hi, voice_cents(g)); }
	CHECK(hi > 22 && hi < 28, "CC 1 = 64: about +-25 cents: %f %f", lo, hi);
	cc(0, 1, 0); ticks(2);
	CHECK(std::fabs(voice_cents(g)) < 1.7, "CC 1 = 0: back on pitch");
	// channel pressure: no vibrato unless configured
	m3(0xD0, 127); ticks(50);
	CHECK(std::fabs(voice_cents(g)) < 1.7, "pressure off by default");
	Config pc = cfg1(); pc.pressure_vibrato = true; setup(pc);
	on(0, 69); m3(0xD0, 127); blk();
	const int h = find_voice(0, 69);
	hi = -1e9; for (int i = 0; i < 200; ++i) { ticks(1); hi = std::max(hi, voice_cents(h)); }
	CHECK(hi > 46, "pressure_vibrato: pressure adds depth: %f", hi);
}
static void t_portamento() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine());
	setup(cfg1());
	cc(0, 65, 127); cc(0, 5, 20);
	on(0, 60); blk(); off(0, 60); blk();
	on(0, 72); blk();
	const int g = find_voice(0, 72);
	CHECK(g >= 0 && std::fabs(voice_cents(g) - (60 - 69) * 100) < 30, "starts from the last note: %f", voice_cents(g));
	const uint32_t T = (20 * 20 * 3 + 4) / 8;
	double prev = voice_cents(g); bool mono = true;
	for (uint32_t i = 0; i < T + 2; ++i) { ticks(1); const double c = voice_cents(g); mono = mono && c >= prev - 0.01; prev = c; }
	CHECK(mono && std::fabs(prev - 300) < 1.7, "rises steadily to its own pitch in %u periods: %f", T, prev);
	ticks(T / 2);
	// half way at half the time: linear in pitch
	on(0, 48); blk(); ticks(T / 2);
	const int h = find_voice(0, 48);
	CHECK(h >= 0 && std::fabs(voice_cents(h) - ((72 + 48) / 2.0 - 69) * 100) < 15, "half way: %f", voice_cents(h));
	// portamento off: no glide; CC 84: the next note glides from the key given
	cc(0, 65, 0); on(0, 50); blk();
	CHECK(std::fabs(voice_cents(find_voice(0, 50)) - (50 - 69) * 100) < 1.7, "portamento off: at once");
	cc(0, 84, 62); on(0, 74); blk();
	const int x = find_voice(0, 74);
	CHECK(x >= 0 && std::fabs(voice_cents(x) - (62 - 69) * 100) < 30, "CC 84: from key 62: %f", voice_cents(x));
	ticks(T + 2);
	CHECK(std::fabs(voice_cents(x) - 500) < 1.7, "and arrives");
}

// ---- step 7: 1-16 chips, stealing, no leaks ----
// The driver's bookkeeping against itself and the chips: masks, counts, lists.
static int invariants(const char *where) {
	int bad = 0;
	uint32_t inheld = 0, inrel = 0, held = 0, rel = 0;
	for (uint32_t k = 0; k < d.cfg.chips; ++k) {
		uint32_t busy = 0;
		for (uint32_t c = 0; c < 18; ++c) {
			const Slot &s = d.slot[k * 18 + c];
			const bool fr = s.state == 0;
			busy += !fr;
			if (fr != bool(d.free_mask[k] >> c & 1)) { ++bad; printf("  %s: chip %u ch %u free %d mask %d\n", where, k, c, fr, int(d.free_mask[k] >> c & 1)); }
			if ((s.state == 1 || s.state == 2) && (s.role == 1 || s.role == 2 || s.role == 4)) ++held;
			if (s.state == 3 && s.role != 3) ++rel;   // a 4-op voice's second channel follows its first, on no list
			if (s.role == 2 && (s.partner != k * 18 + c + 3 || d.slot[s.partner].role != 3)) { ++bad; printf("  %s: 4-op pair broken at %u\n", where, k * 18 + c); }
		}
		if (busy != d.busy[k]) { ++bad; printf("  %s: chip %u busy %u counted %u\n", where, k, busy, d.busy[k]); }
		if (!d.cfg.stream && d.four[k] != chips[k].r_104) { ++bad; printf("  %s: chip %u 104 %x chip %x\n", where, k, d.four[k], chips[k].r_104); }
	}
	for (uint32_t g = d.held_head; g != NONE && inheld <= d.nslots; g = d.slot[g].gnext) { ++inheld; if (d.slot[g].state != 1 && d.slot[g].state != 2) ++bad; }
	for (uint32_t g = d.rel_head; g != NONE && inrel <= d.nslots; g = d.slot[g].gnext) { ++inrel; if (d.slot[g].state != 3 || d.slot[g].role == 3) ++bad; }
	if (inheld != held || inrel != rel) { ++bad; printf("  %s: lists %u/%u held, %u/%u released\n", where, inheld, held, inrel, rel); }
	uint32_t owned = 0;
	for (uint32_t m = 0; m < 16; ++m) for (uint32_t g = d.ch[m].head; g != NONE && owned <= d.nslots; g = d.slot[g].cnext) { ++owned; if (d.slot[g].midi != m) ++bad; }
	uint32_t want = 0; for (uint32_t g = 0; g < d.nslots; ++g) want += d.slot[g].state != 0 && d.slot[g].role != 3;
	if (owned != want) { ++bad; printf("  %s: %u owned in channel lists, %u busy\n", where, owned, want); }
	return bad;
}
static uint32_t rng = 1;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
// A random block of messages (rate: messages a block); drums, pedals, controllers, programs, resets.
static void random_block(uint32_t n, uint32_t frames) {
	for (uint32_t i = 0; i < n; ++i) {
		const uint32_t off = uint32_t(uint64_t(i) * frames / (n ? n : 1));
		const uint8_t ch = uint8_t(rnd() % 16), r = uint8_t(rnd() % 100);
		const uint8_t a = uint8_t(rnd() % 128), b = uint8_t(rnd() % 128);
		if (r < 40) send3(d, off, uint8_t(0x90 | ch), uint8_t(24 + a % 72), uint8_t(1 + b % 127));
		else if (r < 75) send3(d, off, uint8_t(0x80 | ch), uint8_t(24 + a % 72), 0);
		else if (r < 80) send3(d, off, uint8_t(0xC0 | ch), a);
		else if (r < 84) send3(d, off, uint8_t(0xB0 | ch), 64, b);
		else if (r < 86) send3(d, off, uint8_t(0xB0 | ch), 66, b);
		else if (r < 89) send3(d, off, uint8_t(0xE0 | ch), a, b);
		else if (r < 91) send3(d, off, uint8_t(0xB0 | ch), 1, b);
		else if (r < 93) { send3(d, off, uint8_t(0xB0 | ch), 65, b); send3(d, off, uint8_t(0xB0 | ch), 5, uint8_t(b % 30)); }
		else if (r < 95) send3(d, off, uint8_t(0xB0 | ch), uint8_t(r & 1 ? 7 : 11), b);
		else if (r < 96) send3(d, off, uint8_t(0xB0 | ch), 0, uint8_t(a < 20 ? 120 : a < 40 ? 121 : a < 50 ? 8 : 0));
		else if (r < 97) send3(d, off, uint8_t(0xB0 | ch), 10, b);
		else if (r < 98) send3(d, off, uint8_t(0xB0 | ch), uint8_t(120 + a % 8), 0);
		else if (r < 99) send3(d, off, uint8_t(0xB0 | ch), 84, uint8_t(24 + a % 72));
		else { const uint8_t m[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}; if (a < 8) send(d, off, m, 6); }
	}
}
static void t_chips() {
	gm_pool();
	// balance: 40 notes over 4 chips, each chip a similar share
	// (a build with a smaller HVOPL3_DRV_MAX_CHIPS clamps; the cases past it are left out)
	for (uint32_t n : {4u, 32u}) {
		if (n > MAX_CHIPS) continue;
		setup(cfg1(n));
		for (uint32_t i = 0; i < 10 * n; ++i) send3(d, 0, uint8_t(0x90 | (i % 8)), uint8_t(20 + (i * 7) % 90), 100);
		blk(); ticks(2);
		uint32_t lo = 99, hi = 0;
		for (uint32_t k = 0; k < n; ++k) { lo = std::min<uint32_t>(lo, d.busy[k]); hi = std::max<uint32_t>(hi, d.busy[k]); }
		CHECK(hi - lo <= 2 && busy_total() > 9 * n, "%u chips: busy channels per chip %u..%u", n, lo, hi);
	}
	// the chip count is clamped to the build's maximum
	setup(cfg1(MAX_CHIPS + 7));
	CHECK(d.cfg.chips == MAX_CHIPS && d.nslots == 18 * MAX_CHIPS, "clamped to %u: %u", unsigned(MAX_CHIPS), d.cfg.chips);
	setup(cfg1(0));
	CHECK(d.cfg.chips == 1, "at least one chip");
	CHECK(invariants("balance") == 0, "bookkeeping");
	// no leaks: a long random stream, then everything off; every channel of every chip free
	for (uint32_t n : {1u, 2u, 4u, 9u, 16u, 32u}) {
		if (n > MAX_CHIPS) continue;
		rng = 77 + n;
		setup(cfg1(n));
		int bad = 0;
#ifdef T_DRV_QUICK
		const int blocks = 300;
#else
		const int blocks = 1500;
#endif
		for (int i = 0; i < blocks; ++i) {
			const uint32_t frames = 1 + rnd() % 480;
			random_block(rnd() % (8 + 4 * n), frames);
			blk(frames);
			if (i % 50 == 0) bad += invariants("fuzz");
		}
		for (uint8_t ch = 0; ch < 16; ++ch) { cc(ch, 64, 0); cc(ch, 66, 0); cc(ch, 123, 0); }
		for (int i = 0; i < 1100; ++i) blk(480);   // 11 s: every release ends, or is cut at 10 s
		bad += invariants("after");
		uint32_t act = 0; for (uint32_t k = 0; k < n; ++k) act |= chips[k].active;
		CHECK(bad == 0 && busy_total() == 0 && act == 0 && d.held_head == NONE && d.rel_head == NONE, "%u chips: %u busy, active %x, %d broken", n, busy_total(), act, bad);
		uint32_t gl = 0; for (auto &c : d.ch) gl += c.gliders;
		CHECK(gl == 0, "no gliders left: %u", gl);
	}
}
// The steal policy, on one chip with every channel taken.
static void t_steal() {
	pool_clear(*pool);
	Patch p = sine();
	p.op[1].r80 = 0x0F;   // a quick release
	set_patch(*pool, false, 0, 0, 0, p);
	Patch slow = sine();
	slow.op[1].r80 = 0x03;   // a slow one
	set_patch(*pool, false, 0, 0, 1, slow);
	// free first: 18 notes take 18 free channels
	setup(cfg1());
	for (uint8_t i = 0; i < 18; ++i) on(0, uint8_t(40 + i), i == 7 ? 30 : 120);
	blk();
	CHECK(busy_total() == 18 && invariants("full") == 0, "18 voices");
	ticks(2);
	// the quietest held note is taken when nothing is free or released
	const int quiet = find_voice(0, 47);
	on(1, 80); blk(); ticks(2);
	CHECK(find_voice(1, 80) == quiet && find_voice(0, 47) < 0, "the quiet note was taken: %d vs %d", find_voice(1, 80), quiet);
	// a note held only by the pedal goes before one whose key is down (same level)
	cc(0, 64, 127); off(0, 50); blk(); ticks(2);
	const int pedal = find_voice(0, 50);
	on(1, 81); blk(); ticks(2);
	CHECK(pedal >= 0 && find_voice(1, 81) == pedal, "the pedal-held note was taken");
	cc(0, 64, 0); ticks(1);
	// a released note (still sounding: the slow patch) goes before any held one, the quietest of them
	setup(cfg1());
	m3(0xC0, 1);
	for (uint8_t i = 0; i < 18; ++i) on(0, uint8_t(40 + i), 120);
	blk(); ticks(2);
	off(0, 44); blk(); ticks(10); off(0, 45); blk(); ticks(1);
	const int r44 = [] { for (uint32_t g = 0; g < 18; ++g) if (d.slot[g].note == 44 && d.slot[g].state == 3) return int(g); return -1; }();
	CHECK(r44 >= 0 && chips[0].active >> r44 & 1, "44 released and still sounding");
	on(1, 82); blk(); ticks(4);
	CHECK(find_voice(1, 82) == r44, "the longer-released (quieter) one is taken: %d want %d", find_voice(1, 82), r44);
	// never a note started in the same control period
	setup(cfg1());
	for (uint8_t i = 0; i < 18; ++i) on(0, uint8_t(40 + i), i == 0 ? 10 : 120, 0);
	on(1, 90, 100, 1); blk(); ticks(2);
	CHECK(find_voice(1, 90) < 0 && find_voice(0, 40) >= 0, "a note of this period is not stolen: the new one is dropped");
	// a patch whose release never ends (release rate 0) is cut 10 s after its note off
	Patch forever = sine(); forever.op[1].r80 = 0x00;
	set_patch(*pool, false, 0, 0, 2, forever);
	setup(cfg1());
	m3(0xC0, 2); on(0, 60); blk(); off(0, 60); blk();
	for (int i = 0; i < 7400; ++i) blk(64);
	CHECK(busy_total() == 1 && (chips[0].active & 0x3FFFF), "still sounding at 9.9 s");
	for (int i = 0; i < 110; ++i) blk(64);
	CHECK(busy_total() == 0 && !chips[0].active, "cut and free after 10 s");
	// deterministic: the same input picks the same voices and makes the same samples
	std::vector<int32_t> a, b;
	for (int run = 0; run < 2; ++run) {
		gm_pool();
		setup(cfg1(2));
		rng = 4242;
		std::vector<int32_t> &out = run ? b : a;
		for (int i = 0; i < 400; ++i) {
			random_block(30, 256);
			blk(256);
			out.insert(out.end(), D, D + 512);
			for (uint32_t g = 0; g < 36; ++g) out.push_back(d.slot[g].note << 8 | d.slot[g].state);
		}
	}
	CHECK(a == b, "the same input, the same voices and samples");
}

// ---- levels: the curves, the remainder, the corrections ----
static void t_levels() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, sine(4));
	setup(cfg1());
	// hirav, exactly: every value of each input alone, and every sum (all four at once: 128^4)
	double ex[128];
	for (int x = 1; x < 128; ++x) ex[x] = 40 * std::log10(127.0 / x) / 0.75;
	uint32_t bad = 0;
	for (int x = 0; x < 128; ++x) {
		const uint32_t want = x ? uint32_t(std::floor(ex[x] + 0.5)) : 255;
		bad += attenuation(d, uint8_t(x), 127, 127, 127) != want || attenuation(d, 127, uint8_t(x), 127, 127) != want ||
		       attenuation(d, 127, 127, uint8_t(x), 127) != want || attenuation(d, 127, 127, 127, uint8_t(x)) != want;
	}
	CHECK(bad == 0, "hirav: %u single values off the formula", bad);
#ifndef T_DRV_QUICK
	for (int a = 1; a < 128; ++a)
		for (int b = 1; b < 128; ++b)
			for (int c = 1; c < 128; ++c) {
				const double ab = ex[a] + ex[b] + ex[c];
				for (int m = 1; m < 128; ++m) {
					const uint32_t want = uint32_t(std::min(255.0, std::floor(ab + ex[m] + 0.5)));
					bad += attenuation(d, uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(m)) != want;
				}
			}
	CHECK(bad == 0, "hirav: %u of 127^4 sums off the formula", bad);
#endif
	// through the chip: CC 7 at every value, on the carrier only, saturating at 63
	cc(0, 7, 127); on(0, 60); blk();
	const int g = find_voice(0, 60);
	bad = 0;
	for (int v = 0; v < 128; ++v) {
		cc(0, 7, uint8_t(v)); blk(1);
		bad += carrier_tl(uint32_t(g)) != std::min(63u, 4 + attenuation(d, 127, uint8_t(v), 127, 127)) || chips[0].r_40[op_index(uint32_t(g), 0)] != 0x3F;
	}
	CHECK(bad == 0, "CC 7 onto the carrier: %u off", bad);
	// linear: the four levels' product, linear in total-level steps, within one step of 63 (1 - product)
	Config lc = cfg1(); lc.curve = CURVE_LINEAR; setup(lc);
	bad = 0;
	for (int i = 0; i < 20000; ++i) {
		const int a = int(rnd() % 128), b = int(rnd() % 128), c = int(rnd() % 128), m = int(rnd() % 128);
		const double want = 63 * (1 - double(a) * b * c * m / (127.0 * 127 * 127 * 127));
		bad += std::fabs(double(attenuation(d, uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(m))) - want) > 1.0;
	}
	CHECK(bad == 0 && attenuation(d, 127, 127, 127, 127) == 0 && attenuation(d, 0, 127, 127, 127) == 63, "linear: %u off by more than a step", bad);
	// the caller's tables: 1/16 steps summed, rounded once; 0xFFFF silent
	static uint16_t tv[128], tw[128];
	for (int i = 0; i < 128; ++i) { tv[i] = uint16_t((127 - i) * 5); tw[i] = uint16_t((127 - i) * 3); }
	tw[0] = 0xFFFF;
	Config tc = cfg1(); tc.curve = CURVE_TABLE; setup(tc); set_curve_tables(d, tv, tw);
	CHECK(attenuation(d, 100, 90, 127, 127) == ((27 * 5 + 37 * 3 + 8) >> 4) && attenuation(d, 100, 0, 127, 127) == 255, "tables");
	// the remainder: what total level 63 cannot take goes to the gain (a documented switch, off by default)
	Config rc = cfg1(); rc.gain_remainder = true; setup(rc);
	set_patch(*pool, false, 0, 0, 0, sine(40));
	cc(0, 7, 127); on(0, 60, 20); blk();
	const int r = find_voice(0, 60);
	const uint32_t att = attenuation(d, 20, 127, 127, 127);
	CHECK(r >= 0 && carrier_tl(uint32_t(r)) == 63 && chips[0].gain[r] == -int(40 + att - 63), "remainder %u: tl %u gain %d", att, carrier_tl(uint32_t(r)), chips[0].gain[r]);
	setup(cfg1());
	cc(0, 7, 127); on(0, 60, 20); blk();
	CHECK(carrier_tl(uint32_t(find_voice(0, 60))) == 63 && chips[0].gain[find_voice(0, 60)] == 0, "off by default");
	// level corrections: each program's gain, each kit's keys (kit 0's for a kit without a table)
	gm_pool();
	static int8_t prog[128];
	static KitLevels kits[2];
	for (int i = 0; i < 128; ++i) { prog[i] = int8_t(i % 33 - 16); kits[0].levels[i] = int8_t(i % 7 - 3); kits[1].levels[i] = int8_t(i % 5 - 2); }
	kits[0].kit = 0; kits[1].kit = 16;
	set_levels(*pool, prog, kits, 2);
	setup(cfg1());
	bad = 0;
	for (int i = 0; i < 128; ++i) {
		m3(0xC0, uint8_t(i)); on(0, 60); blk();
		const int x = find_voice(0, 60);
		if (x >= 0) bad += chips[0].gain[x] != prog[i] || (d.slot[x].partner != NONE && chips[0].gain[d.slot[x].partner] != prog[i]);
		off(0, 60); cc(0, 120, 0); ticks(4);
	}
	CHECK(bad == 0, "%u programs' voices without their correction", bad);
	cc(1, 0, 8); m3(0xC1, 0); on(1, 61); blk();
	CHECK(chips[0].gain[find_voice(1, 61)] == prog[0], "every melodic bank's program");
	bad = 0;
	for (uint8_t kit : {0, 16, 25}) {
		m3(0xC9, kit);
		for (uint8_t key : {35, 36, 38, 42, 49}) {
			on(9, key); blk();
			const int x = find_voice(9, key);
			const int8_t want = kit == 16 ? kits[1].levels[key] : kits[0].levels[key];
			if (x >= 0) bad += chips[0].gain[x] != want; else ++bad;
			off(9, key); cc(9, 120, 0); ticks(4);
		}
	}
	CHECK(bad == 0, "%u drum keys without their kit's correction", bad);
	Config off_ = cfg1(); off_.level_corrections = false; setup(off_);
	m3(0xC0, 3); on(0, 60); blk();
	CHECK(chips[0].gain[find_voice(0, 60)] == 0, "the switch off: no gain");
	Config li = cfg1(); li.curve = CURVE_LINEAR; setup(li);
	m3(0xC0, 3); on(0, 60); blk();
	CHECK(chips[0].gain[find_voice(0, 60)] == 0, "another curve: no gain");
	set_levels(*pool, nullptr, nullptr, 0);
}

// ---- step 8: block sizes and the command stream ----
struct Timed { uint32_t at; uint8_t m[11]; uint8_t n; };
// A song-like random stream at fixed frames: the same whatever the blocks.
static std::vector<Timed> stream_of(uint32_t seed, uint32_t frames, uint32_t density) {
	std::vector<Timed> v;
	rng = seed;
	for (uint32_t t = 0; t < frames;) {
		t += rnd() % density;
		const uint8_t ch = uint8_t(rnd() % 16), r = uint8_t(rnd() % 100), a = uint8_t(rnd() % 128), b = uint8_t(rnd() % 128);
		Timed x = {t, {}, 3};
		if (r < 45) { x.m[0] = uint8_t(0x90 | ch); x.m[1] = uint8_t(30 + a % 60); x.m[2] = uint8_t(1 + b % 127); }
		else if (r < 80) { x.m[0] = uint8_t(0x80 | ch); x.m[1] = uint8_t(30 + a % 60); x.m[2] = 0; }
		else if (r < 84) { x.m[0] = uint8_t(0xC0 | ch); x.m[1] = a; x.n = 2; }
		else if (r < 88) { x.m[0] = uint8_t(0xE0 | ch); x.m[1] = a; x.m[2] = b; }
		else if (r < 91) { x.m[0] = uint8_t(0xB0 | ch); x.m[1] = 1; x.m[2] = b; }
		else if (r < 93) { x.m[0] = uint8_t(0xB0 | ch); x.m[1] = 64; x.m[2] = b; }
		else if (r < 95) { x.m[0] = uint8_t(0xB0 | ch); x.m[1] = 10; x.m[2] = b; }
		else if (r < 97) { x.m[0] = uint8_t(0xB0 | ch); x.m[1] = uint8_t(a & 1 ? 91 : 93); x.m[2] = b; }
		else if (r < 98) { x.m[0] = uint8_t(0xB0 | ch); x.m[1] = 65; x.m[2] = b; v.push_back(x); x.m[1] = 5; x.m[2] = uint8_t(b % 40); }
		else { x.m[0] = uint8_t(0xB0 | ch); x.m[1] = 7; x.m[2] = b; }
		if (x.at < frames) v.push_back(x);
	}
	return v;
}
// Rendered in blocks of `bs` (direct), or as records replayed by play() (stream, `cap` records a call).
static uint32_t splits;   // render_stream calls that stopped short
static void play_stream(const std::vector<Timed> &ev, uint32_t frames, uint32_t bs, const Config &c, bool stream, uint32_t cap,
                        std::vector<int32_t> out[3]) {
	Config cc_ = c; cc_.stream = stream;
	init(d, *pool, stream ? chips2 : chips, cc_);
	const uint32_t scale = c.rate == RATE_96K ? 2 : 1;
	for (int b = 0; b < 3; ++b) out[b].assign(size_t(2) * frames * scale, 0);
	static std::vector<uint32_t> rec;
	rec.resize(cap);
	size_t e = 0;
	for (uint32_t t = 0; t < frames * scale;) {
		const uint32_t n = std::min(bs, frames * scale - t);
		while (e < ev.size() && ev[e].at * scale < t + n) { CHECK(send(d, ev[e].at * scale - t, ev[e].m, ev[e].n), "queue"); ++e; }
		if (!stream) {
			const Output o = {&out[0][2 * t], &out[1][2 * t], &out[2][2 * t], false};
			render(d, o, n);
			t += n;
		} else {
			uint32_t done = 0;
			while (done < n) {
				uint32_t cnt = 0;
				const uint32_t m = render_stream(d, rec.data(), cap, cnt, n - done);
				CHECK(m > 0 || cnt > 0, "progress: frames or records");
				if (!m && !cnt) { t = frames * scale; break; }
				const Output o = {&out[0][2 * (t + done)], &out[1][2 * (t + done)], &out[2][2 * (t + done)], true};
				play(chips3, c.chips, rec.data(), cnt, o, c.rate, chip_flags(d));
				splits += m < n - done;
				done += m;
			}
			t += done;
		}
	}
}
static uint64_t energy(const std::vector<int32_t> &v) { uint64_t e = 0; for (int32_t x : v) e += uint64_t(int64_t(x) * x); return e; }
static void t_blocks() {
	gm_pool();
	const uint32_t F = 48000;
	const auto ev = stream_of(99, F, 400);
	std::vector<int32_t> ref[3], got[3];
	Config c = cfg1(2);
	play_stream(ev, F, 480, c, false, 0, ref);
	CHECK(energy(ref[0]) > 0 && energy(ref[1]) > 0, "the stream sounds");
	for (uint32_t bs : {1u, 7u, 64u}) {
		play_stream(ev, F, bs, c, false, 0, got);
		CHECK(got[0] == ref[0] && got[1] == ref[1] && got[2] == ref[2], "blocks of %u frames: the samples of blocks of 480", bs);
	}
	play_stream(ev, F, 4000, c, false, 0, got);
	CHECK(got[0] == ref[0] && got[1] == ref[1] && got[2] == ref[2], "blocks of 4000");
}
static void t_stream() {
	gm_pool();
	const uint32_t F = 36000;
	const auto ev = stream_of(1234, F, 200);
	std::vector<int32_t> ref[3], got[3];
	struct K { uint32_t chips; Rate rate; uint32_t flags; bool hard; uint32_t bs; };
	const K ks[] = {{1, RATE_48K, 0, false, 480}, {5, RATE_48K, 0, false, 256}, {2, RATE_96K, 0, false, 480}, {3, RATE_48K, HQ_PHASE | HQ_ENVELOPE, true, 333}, {16, RATE_48K, 0, false, 480}, {32, RATE_48K, 0, false, 480}};
	for (const K &k : ks) {
		if (k.chips > MAX_CHIPS) continue;
		Config c = cfg1(k.chips); c.rate = k.rate; c.chip_flags = k.flags; c.hard_pan = k.hard;
		play_stream(ev, F, k.bs, c, false, 0, ref);
		init(d, *pool, chips2, c);   // the capacity the stream needs
		Config sc = c; sc.stream = true; configure(d, chips2, sc);
		const uint32_t need = stream_reserve(d) + 64;
		play_stream(ev, F, k.bs, c, true, need * 4, got);
		CHECK(got[0] == ref[0] && got[1] == ref[1] && got[2] == ref[2], "%u chips at %u: the stream's samples are direct mode's", k.chips, uint32_t(k.rate));
		// a buffer just past the reserve: blocks split where the records would not fit, the same samples
		play_stream(ev, F, k.bs, c, true, need, got);
		CHECK(got[0] == ref[0] && got[1] == ref[1] && got[2] == ref[2], "%u chips, small buffer: the same samples", k.chips);
	}
	// long blocks with many messages into a buffer of the reserve: the records split the blocks
	{
		const auto dense = stream_of(77, F, 30);
		Config c = cfg1(1);
		play_stream(dense, F, 3000, c, false, 0, ref);
		init(d, *pool, chips2, c);
		Config sc = c; sc.stream = true; configure(d, chips2, sc);
		splits = 0;
		play_stream(dense, F, 3000, c, true, stream_reserve(d) + 8, got);
		CHECK(splits > 0, "blocks were split: %u", splits);
		CHECK(got[0] == ref[0] && got[1] == ref[1] && got[2] == ref[2], "split blocks: the same samples");
	}
	// a buffer below the reserve: no progress, nothing written
	Config sc = cfg1(); sc.stream = true;
	init(d, *pool, chips2, sc);
	static uint32_t tiny[16];
	uint32_t cnt = 99;
	CHECK(render_stream(d, tiny, 16, cnt, 480) == 0 && cnt == 0, "too small: refused");
	// render() in stream mode and render_stream() in direct mode do nothing
	const Output o = {D, nullptr, nullptr, false};
	CHECK(render(d, o, 64) == 0, "render in stream mode");
}

// ---- the C ABI: the same driver on caller-allocated memory ----
// The returning note: a Note On whose voice (channel, note, patch) still releases keys it again, its
// envelope going on from where it was; not a cut voice, not another patch, not with `returning` off.
static Patch slow(uint8_t kind) {   // a slow attack and a slow release
	Patch p;
	memset(&p, 0, sizeof p);
	p.kind = kind;
	for (int o = 0; o < 4; ++o) p.op[o] = Operator{0x21, uint8_t(o & 1 ? 0 : 0x3F), 0x40, 0x03, 0};
	p.c0[0] = 0; p.c0[1] = 0;
	return p;
}
static uint32_t carrier_env(uint32_t g) { return chips[g / 18].env[op_index(g % 18, 1)]; }
static void t_returning() {
	pool_clear(*pool);
	set_patch(*pool, false, 0, 0, 0, slow(KIND_2OP));
	set_patch(*pool, false, 0, 0, 1, slow(KIND_PSEUDO));
	set_patch(*pool, false, 0, 0, 2, slow(KIND_4OP));
	set_patch(*pool, false, 0, 0, 3, slow(KIND_2OP));
	Patch q = slow(KIND_PSEUDO);
	q.op[3].r40 = 20;                                          // the second half quieter: stolen first
	set_patch(*pool, false, 0, 0, 4, q);
	for (int mode = 0; mode < 2; ++mode) {
		Config c = cfg1(); c.returning = mode == 0;
		setup(c);
		on(0, 60); ticks(20); off(0, 60); ticks(4);
		int g = -1;
		for (uint32_t x = 0; x < d.nslots; ++x) if (d.slot[x].state == 3 && d.slot[x].note == 60) g = int(x);
		const uint32_t before = g >= 0 ? carrier_env(uint32_t(g)) : 0;
		CHECK(g >= 0 && before < 1000, "a released voice, still sounding (env %u)", before);
		if (g < 0) continue;
		on(0, 60); blk(1);
		const int h = find_voice(0, 60);
		if (mode == 0) {
			CHECK(h == g && keyed(uint32_t(h)) && busy_total() == 1, "returning: the same voice keyed again (%d, %d), no other", h, g);
			CHECK(carrier_env(uint32_t(g)) <= before && (chips[0].r_80[op_index(uint32_t(g) % 18, 1)] & 15) == 3,
			      "... its envelope from where it was (%u, was %u), no cut", carrier_env(uint32_t(g)), before);
		} else {
			CHECK(h >= 0 && h != g && busy_total() == 2, "returning off: another voice (%d, %d)", h, g);
		}
	}
	// a cut voice (All Sound Off) and another patch are not returned to
	setup(cfg1());
	on(0, 60); ticks(20); cc(0, 120, 0);
	on(0, 60); blk(1);
	CHECK(busy_total() == 2, "a cut voice: not returned (%u busy)", busy_total());
	setup(cfg1());
	on(0, 60); ticks(20); off(0, 60); ticks(2);
	m3(0xC0, 3); on(0, 60); blk(1);
	CHECK(busy_total() == 2, "another patch: not returned (%u busy)", busy_total());
	// pseudo-4-op: both halves, each on its channel again; 4-op: the pair, still in 4-op mode
	for (uint8_t prog : {1, 2}) {
		setup(cfg1());
		m3(0xC0, prog); on(0, 60); ticks(20);
		const int a = find_voice(0, 60);
		const uint16_t b = a >= 0 ? d.slot[a].partner : 0;
		off(0, 60); ticks(3);
		on(0, 60); blk(1);
		const int a2 = find_voice(0, 60);
		CHECK(a >= 0 && a2 == a && d.slot[a2].partner == b && busy_total() == 2 && keyed(uint32_t(a2)),
		      "%s: returned whole (%d -> %d, partner %u -> %u, %u busy)", prog == 1 ? "pseudo-4-op" : "4-op", a, a2, b,
		      a2 >= 0 ? d.slot[a2].partner : 0, busy_total());
		if (prog == 2 && a2 >= 0) CHECK(chips[0].r_104 & 1, "4-op: the pair still 4-op");
	}
	// a pseudo-4-op voice with one half taken meanwhile: not returned, a whole new voice
	setup(cfg1());
	m3(0xC0, 4); on(0, 60); ticks(20); off(0, 60); ticks(2);
	for (uint8_t k = 0; k < 17; ++k) on(1, uint8_t(40 + k));   // the 16 free channels, then one released half
	ticks(4);
	on(0, 60); ticks(4);
	const int r = find_voice(0, 60);
	CHECK(r >= 0 && d.slot[r].role == 4, "one half left: a new pseudo-4-op voice, both halves (role %d)", r >= 0 ? d.slot[r].role : -1);
}

// GS's per-drum NRPNs on a percussion channel (18h pitch, 1Ah level, 1Ch pan, 1Dh reverb, 1Eh chorus; the
// key the LSB): each on the key's voices, a sounding one too; none on a melodic channel; kept by Reset
// All Controllers (which nulls the NRPN selected), gone at a GS reset; map 2 apart from map 1.
static void nrpn(uint32_t ch, uint8_t msb, uint8_t key, uint8_t v) { cc(ch, 99, msb); cc(ch, 98, key); cc(ch, 6, v); }
static void t_gs_drums() {
	gm_pool();
	setup(cfg1(4));
	cc(9, 91, 127); cc(9, 93, 127);
	nrpn(9, 0x1D, 36, 0); on(9, 36); blk();
	int g = find_voice(9, 36);
	CHECK(g >= 0 && d.slot[g].rev_w == 0 && d.slot[g].cho_w == 127, "1Dh/36 = 0: the kick on no reverb, chorus the channel's");
	on(9, 42); blk(); g = find_voice(9, 42);
	CHECK(g >= 0 && d.slot[g].rev_w == 127 && d.slot[g].cho_w == 127, "a key no NRPN names: the channel's sends");
	nrpn(9, 0x1E, 42, 64); blk();
	CHECK(g >= 0 && d.slot[g].cho_w == 64 && d.slot[g].rev_w == 127, "1Eh/42 = 64 on the sounding hi-hat: chorus 64 of 127 at once");
	nrpn(9, 0x1C, 42, 20); blk();
	CHECK(g >= 0 && d.slot[g].pan_w == 20, "1Ch/42 = 20: the hi-hat's pan (%u)", g >= 0 ? d.slot[g].pan_w : 0);
	cc(9, 10, 74); blk();
	CHECK(g >= 0 && d.slot[g].pan_w == 30, "... about the channel's (74: 30, got %u)", g >= 0 ? d.slot[g].pan_w : 0);
	// level: the key's voice down by the curve's attenuation for that share of the expression
	on(9, 38, 100); blk();
	g = find_voice(9, 38);
	const uint32_t before = g >= 0 ? carrier_tl(uint32_t(g)) : 0;
	nrpn(9, 0x1A, 38, 64); blk();
	const uint32_t want = std::min(63u, before + attenuation(d, 100, 100, 64, 127) - attenuation(d, 100, 100, 127, 127));
	CHECK(g >= 0 && carrier_tl(uint32_t(g)) == want, "1Ah/38 = 64: the snare's level (TL %u, want %u)", g >= 0 ? carrier_tl(uint32_t(g)) : 0, want);
	// pitch: the next note of the key, 2 semitones up
	on(9, 45); blk(); g = find_voice(9, 45);
	const double c0 = voice_cents(g);
	off(9, 45); nrpn(9, 0x18, 45, 66); ticks(2); on(9, 45); ticks(4);
	const int g2 = find_voice(9, 45);
	CHECK(g2 >= 0 && std::fabs(voice_cents(g2) - c0 - 200) < 3, "18h/45 = 66: the next tom two semitones up (%.1f cents)", voice_cents(g2) - c0);
	// a melodic channel: nothing; Reset All Controllers: the NRPN select nulled, the settings kept
	nrpn(0, 0x1D, 50, 0); blk();
	CHECK(d.drums[0][50].reverb == 127, "a melodic channel's per-drum NRPN: nothing");
	cc(9, 121, 0); cc(9, 6, 5); blk();
	CHECK(d.drums[0][36].reverb == 0 && d.drums[0][42].chorus == 64 && d.drums[0][36].level == 127 && d.drums[0][45].pitch == 2,
	      "Reset All Controllers: the settings kept, a data entry after it goes nowhere");
	// map 2 (GS: part 11 a rhythm part on map 2) apart from map 1
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x1A, 0x15, 0x02, 0x0F, 0xF7}); blk();
	CHECK(d.ch[10].percussion && d.ch[10].drum_map == 1, "GS: part 11 rhythm, map 2");
	nrpn(10, 0x1D, 36, 90); blk();
	CHECK(d.drums[1][36].reverb == 90 && d.drums[0][36].reverb == 0, "map 2's kick its own, map 1's as set");
	sx({0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7}); blk();
	CHECK(d.drums[0][36].reverb == 127 && d.drums[1][36].reverb == 127 && d.drums[0][45].pitch == 0 && d.drums[0][42].pan == 0,
	      "GS reset: every per-drum setting as at power-on");
}

// The sound drivers' volume models: every model at single inputs exactly as measured (models_measured.inc),
// both operators of an additive voice (APOGEE's first its own rule, HMI_OLD's first never scaled); in FM
// the modulator keeps the bank's level under every model; a level of 0 anywhere is silent.
struct Measured { uint8_t curve, tl, vel, vol, op1, op2; };
static const Measured MEASURED[] = {
#include "models_measured.inc"
};
static void t_models() {
	pool_clear(*pool);
	uint32_t bad = 0, n = 0;
	for (const Measured &G : MEASURED) {
		Patch p = sine(G.tl);
		p.op[0].r40 = G.tl;                                     // the modulator heard too: additive
		p.c0[0] = 1;
		set_patch(*pool, false, 0, 0, 0, p);
		Config c = cfg1(); c.curve = G.curve; c.level_corrections = false;
		setup(c);
		cc(0, 7, G.vol); on(0, 60, G.vel); blk();
		const int g = find_voice(0, 60);
		const uint32_t ch = g < 0 ? 0 : uint32_t(g) % 18;
		const uint32_t t1 = chips[0].r_40[op_index(ch, 0)] & 63, t2 = chips[0].r_40[op_index(ch, 1)] & 63;
		++n;
		if (g < 0 || t1 != G.op1 || t2 != G.op2) {
			if (bad < 5) printf("  model %u, level %u, vel %u, vol %u: %u/%u, measured %u/%u\n", G.curve, G.tl, G.vel, G.vol, t1, t2, G.op1, G.op2);
			++bad;
		}
	}
	CHECK(bad == 0, "the models at single inputs: %u of %u points off the measurements", bad, n);
	// two or more inputs at once: the composition (velocity with the volume chain, CC 7 x CC 11 x master)
	struct Combined { uint8_t curve, tl, vel, vol, expr, master, op2; };
	static const Combined COMBINED[] = {
#include "models_combined.inc"
	};
	bad = n = 0;
	for (const Combined &G : COMBINED) {
		Patch p = sine(G.tl);
		p.c0[0] = 1; p.op[0].r40 = G.tl;
		set_patch(*pool, false, 0, 0, 0, p);
		Config c = cfg1(); c.curve = G.curve; c.level_corrections = false;
		setup(c);
		sx({0xF0, 0x7F, 0x7F, 0x04, 0x01, 0x00, G.master, 0xF7});
		cc(0, 7, G.vol); cc(0, 11, G.expr); on(0, 60, G.vel); blk();
		const int g = find_voice(0, 60);
		const uint32_t t2 = g < 0 ? 99 : chips[0].r_40[op_index(uint32_t(g) % 18, 1)] & 63;
		++n;
		if (t2 != G.op2) { if (bad < 5) printf("  model %u: level %u vel %u vol %u expr %u master %u: %u, measured %u\n", G.curve, G.tl, G.vel, G.vol, G.expr, G.master, t2, G.op2); ++bad; }
	}
	CHECK(bad == 0, "the models with inputs combined: %u of %u points off", bad, n);
	// FM: the modulator untouched; volume 0 silent
	for (uint8_t curve = CURVE_DMX; curve <= CURVE_NATIVE; ++curve) {
		set_patch(*pool, false, 0, 0, 0, sine(10));
		Config c = cfg1(); c.curve = curve; setup(c);
		cc(0, 7, 50); on(0, 60, 70); blk();
		int g = find_voice(0, 60);
		CHECK(g >= 0 && chips[0].r_40[op_index(uint32_t(g) % 18, 0)] == 0x3F, "model %u, FM: the modulator as the bank has it", curve);
		cc(0, 7, 0); blk();
		CHECK(g >= 0 && (chips[0].r_40[op_index(uint32_t(g) % 18, 1)] & 63) == 63 && attenuation(d, 70, 0, 127, 127) == 255,
		      "model %u: volume 0 silent", curve);
	}
}

// A released voice is freed once the chip says it can make no more sound (hvopl3::inaudible: 72 dB down),
// not at its envelope's end (96 dB), and cut then, so a release that never ends (rate 0) stops too.
static void t_inaudible_free() {
	pool_clear(*pool);
	Patch p = sine(0);
	p.op[1].r80 = 0x04;                                        // release rate 4: 96 dB in about 4.9 s
	set_patch(*pool, false, 0, 0, 0, p);
	Patch q = sine(0);
	q.op[1].r20 = 0x01; q.op[1].r60 = 0xFA; q.op[1].r80 = 0xF0;   // decays to 93 dB, then a release that never ends
	set_patch(*pool, false, 0, 0, 1, q);
	setup(cfg1());
	on(0, 60); ticks(10); off(0, 60);
	uint32_t t = 0;
	while (busy_total() && t < 8000) { blk(64); ++t; }
	CHECK(t > 2000 && t < 3300, "release rate 4: freed after %u periods (72 dB: about 2770; its end, 96 dB: about 3690)", t);
	bool zero = true;
	for (int b = 0; b < 100; ++b) { blk(480); for (uint32_t i = 0; i < 960; ++i) zero = zero && D[i] == 0; }
	CHECK(zero, "freed: silent from then on");
	setup(cfg1());
	m3(0xC0, 1); cc(0, 7, 1); on(0, 60, 1); ticks(200); off(0, 60);
	ticks(20);
	CHECK(busy_total() == 0 && chips[0].active == 0, "a release that never ends, far down: freed and cut (%u busy, active %x)", busy_total(), chips[0].active);
}

// update(): settings changed with the notes sounding on, the channel's state kept; the rest a full reset.
static void t_update() {
	pool_clear(*pool);
	Patch p = sine(4);
	p.gain = 3;
	set_patch(*pool, false, 0, 0, 0, p);
	setup(cfg1());
	cc(0, 10, 30); cc(0, 7, 64); on(0, 60); blk();
	const int g = find_voice(0, 60);
	CHECK(g >= 0 && keyed(uint32_t(g)) && chips[0].gain[g] == 3, "a note, its correction");
	const uint32_t hirav = carrier_tl(uint32_t(g));
	Config c = cfg1();
	update(d, chips, c);
	CHECK(d.pending == 0, "the same settings: nothing to do");
	c.curve = CURVE_LINEAR;
	update(d, chips, c); blk();
	CHECK(find_voice(0, 60) == g && keyed(uint32_t(g)) && d.ch[0].pan == 30, "a new curve: the note sounds on, the channel as it was");
	CHECK(carrier_tl(uint32_t(g)) == std::min(63u, 4 + attenuation(d, 127, 64, 127, 127)) && carrier_tl(uint32_t(g)) != hirav,
	      "... at the new curve's level (%u, hirav %u)", carrier_tl(uint32_t(g)), hirav);
	CHECK(chips[0].gain[g] == 0, "... and no correction off the hirav curve");
	c.curve = CURVE_HIRAV;
	update(d, chips, c); blk();
	CHECK(carrier_tl(uint32_t(g)) == hirav && chips[0].gain[g] == 3, "hirav again: its level and correction");
	c.level_corrections = false;
	update(d, chips, c); blk();
	CHECK(keyed(uint32_t(g)) && chips[0].gain[g] == 0, "corrections off: the gain goes, the note sounds on");
	CHECK(!(chips[0].r_bd & 0xC0), "deep tremolo and vibrato: the bank's (off)");
	c.deep_tremolo = 1; c.deep_vibrato = 1;
	update(d, chips, c); blk();
	CHECK(keyed(uint32_t(g)) && (chips[0].r_bd & 0xC0) == 0xC0, "deep tremolo and vibrato on, the note sounding on");
	c.chip_flags = HQ_PHASE | HQ_ENVELOPE | NO_CLIP;
	update(d, chips, c); blk();
	CHECK(keyed(uint32_t(g)) && chips[0].flags == (HQ_PHASE | HQ_ENVELOPE | NO_CLIP) && d.ch[0].volume == 64,
	      "the chips' hq flags, the note sounding on (flags %x)", chips[0].flags);
	c.chips = 2;
	update(d, chips, c);
	CHECK(d.pending & 1, "another chip count: a full reset");
	blk();
	CHECK(find_voice(0, 60) < 0 && d.ch[0].volume == 100, "... the notes and controllers gone");
	c.stream = true;
	update(d, chips, c);
	CHECK(d.pending & 1, "into stream mode: a full reset");
	std::vector<uint32_t> rec(stream_reserve(d) + 64);
	uint32_t n = 0;
	render_stream(d, rec.data(), uint32_t(rec.size()), n, 64);
	c.chip_flags = HQ_PHASE;
	update(d, chips, c);
	CHECK(d.pending & 1, "the chips' flags in stream mode: a full reset (no record carries them)");
	c.stream = false; c.chips = 1; c.chip_flags = 0;
	setup(c);
}

static void t_cabi() {
	CHECK(hvopl3_drv_version() == HVOPL3_DRV_VERSION_NUMBER && hvopl3_drv_size() == sizeof(Driver) && hvopl3_drv_config_size() == sizeof(Config), "sizes");
	std::vector<uint8_t> dm(hvopl3_drv_size() + 16), pm(hvopl3_drv_pool_bytes(16) + 16), cm(hvopl3_drv_config_size());
	std::vector<uint8_t> chm(hvopl3_chip_size() * 2 + 16);
	void *drvp = reinterpret_cast<void *>((uintptr_t(dm.data()) + 15) & ~uintptr_t(15));
	void *chp = reinterpret_cast<void *>((uintptr_t(chm.data()) + 15) & ~uintptr_t(15));
	void *pp = hvopl3_drv_pool_init(reinterpret_cast<void *>((uintptr_t(pm.data()) + 15) & ~uintptr_t(15)), hvopl3_drv_pool_bytes(16));
	CHECK(pp && hvopl3_drv_load_wopl(pp, wopl.data(), wopl.size()) == 0, "pool and bank");
	hvopl3_drv_default_config(cm.data());
	reinterpret_cast<Config *>(cm.data())->chips = 2;
	hvopl3_drv_init(drvp, pp, chp, cm.data());
	const uint8_t on_[3] = {0x90, 60, 100};
	CHECK(hvopl3_drv_send(drvp, 10, on_, 3) == 1, "send");
	CHECK(hvopl3_drv_render(drvp, D, nullptr, nullptr, 0, 480) == SOUNDED, "a note sounds through the C ABI");
	hvopl3_drv_set_percussion(drvp, 3, 1);
	CHECK(static_cast<Driver *>(drvp)->ch[3].percussion, "percussion");
	hvopl3_drv_panic(drvp); hvopl3_drv_reset(drvp);
	hvopl3_drv_render(drvp, D, nullptr, nullptr, 0, 480);
	reinterpret_cast<Config *>(cm.data())->deep_tremolo = 1;
	hvopl3_drv_update(drvp, chp, cm.data());
	hvopl3_drv_render(drvp, D, nullptr, nullptr, 0, 480);
	CHECK(static_cast<Chip *>(chp)->r_bd & 0x80, "update through the C ABI");
	reinterpret_cast<Config *>(cm.data())->stream = true;
	hvopl3_drv_configure(drvp, chp, cm.data());
	std::vector<uint32_t> rec(hvopl3::drv::stream_reserve(*static_cast<Driver *>(drvp)) + 64);
	uint32_t cnt = 0;
	CHECK(hvopl3_drv_render_stream(drvp, rec.data(), uint32_t(rec.size()), &cnt, 480) == 480 && cnt > 0, "the stream through the C ABI");
}

int main(int argc, char **argv) {
	pool = pool_init(poolmem, sizeof poolmem);
	CHECK(pool && pool->capacity >= 32, "pool");
	const char *bank = argc > 1 ? argv[1] : "../drv/banks/DMXOPL3-GS.wopl";
	if (FILE *f = fopen(bank, "rb")) { int c; while ((c = fgetc(f)) != EOF) wopl.push_back(uint8_t(c)); fclose(f); }
	CHECK(wopl.size() > 1000, "DMXOPL3 at %s", bank);
	t_notes();
	t_wopl();
	t_wopl_fuzz();
	t_kinds();
	t_pedals();
	t_rpn();
	t_percussion();
	t_pitch();
	t_vibrato();
	t_portamento();
	t_levels();
	t_chips();
	t_steal();
	t_blocks();
	t_stream();
	t_update();
	t_returning();
	t_inaudible_free();
	t_models();
	t_gs_drums();
	t_cabi();
	printf("t_drv: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
