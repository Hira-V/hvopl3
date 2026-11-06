// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3's kernels in RVV: the same integers as the scalar ones, in 16-bit lanes across the samples of
// a segment (32-bit for the phases). Built for RVV 1.0 (X280), RVV 0.7.1 (C920) and Zve32x (Tensix
// T2): no 64-bit elements, and table indices as wide as the table's elements.
//

#if !defined(HV_RVV_MIX) && !defined(HV_RVV_LANES)
// The phases of the next vl samples, from p: p + inc, p + 2 inc, ...
HV_INLINE vuint32m2_t v_phases(uint32_t p, uint32_t inc, size_t vl) {
	return __riscv_vadd_vx_u32m2(__riscv_vmul_vx_u32m2(__riscv_vadd_vx_u32m2(__riscv_vid_v_u32m2(vl), 1, vl), inc, vl), p, vl);
}

// wave index (mod 1024, 16-bit) -> the operator's output: gather, add, gather, shift, invert.
HV_INLINE vint16m1_t v_op(const uint16_t *wave, vuint16m1_t idx, uint32_t att, size_t vl) {
	const vuint16m1_t off = __riscv_vsll_vx_u16m1(__riscv_vand_vx_u16m1(idx, 1023, vl), 1, vl);
	const vuint16m1_t e = __riscv_vadd_vx_u16m1(__riscv_vluxei16_v_u16m1(wave, off, vl), uint16_t(att), vl);
	const vuint16m1_t frac = __riscv_vsll_vx_u16m1(__riscv_vand_vx_u16m1(e, 255, vl), 1, vl);
	// a 16-bit shift counts mod 16; any shift from 12 up is 0 anyway
	const vuint16m1_t whole = __riscv_vminu_vx_u16m1(__riscv_vand_vx_u16m1(__riscv_vsrl_vx_u16m1(e, 8, vl), 0x7F, vl), 15, vl);
	const vuint16m1_t v = __riscv_vsrl_vv_u16m1(__riscv_vluxei16_v_u16m1(POW.v, frac, vl), whole, vl);
	const vint16m1_t s = __riscv_vsra_vx_i16m1(__riscv_vreinterpret_v_u16m1_i16m1(e), 15, vl);
	return __riscv_vxor_vv_i16m1(__riscv_vreinterpret_v_u16m1_i16m1(v), s, vl);
}

HV_INLINE vuint16m1_t v_index(vuint32m2_t p, size_t vl) { return __riscv_vnsrl_wx_u16m1(p, 22, vl); }

void k_one(const Chip &c, uint32_t i, uint32_t &ph, int16_t *out, uint32_t n) {
	const uint16_t *w = WAVES.w[c.wave[i]];
	const uint32_t inc = c.inc[i], att = c.att[i];
	uint32_t p = ph;
	for (size_t vl; n; n -= vl, out += vl, p += uint32_t(vl) * inc) {
		vl = __riscv_vsetvl_e16m1(n);
		__riscv_vse16_v_i16m1(out, v_op(w, v_index(v_phases(p, inc, vl), vl), att, vl), vl);
	}
	ph = p;
}

// A carrier over a row of modulation.
void k_car(const Chip &c, uint32_t i, uint32_t &ph, const int16_t *mod, int16_t *out, uint32_t n) {
	const uint16_t *w = WAVES.w[c.wave[i]];
	const uint32_t inc = c.inc[i], att = c.att[i];
	uint32_t p = ph;
	for (size_t vl; n; n -= vl, out += vl, mod += vl, p += uint32_t(vl) * inc) {
		vl = __riscv_vsetvl_e16m1(n);
		const vuint16m1_t m = __riscv_vreinterpret_v_i16m1_u16m1(__riscv_vle16_v_i16m1(mod, vl));
		__riscv_vse16_v_i16m1(out, v_op(w, __riscv_vadd_vv_u16m1(v_index(v_phases(p, inc, vl), vl), m, vl), att, vl), vl);
	}
	ph = p;
}

void k_fm(const Chip &c, uint32_t a, uint32_t b, uint32_t &pha, uint32_t &phb, int16_t *out, uint32_t n) {
	const uint16_t *wa = WAVES.w[c.wave[a]], *wb = WAVES.w[c.wave[b]];
	const uint32_t ia = c.inc[a], ib = c.inc[b], aa = c.att[a], ab = c.att[b];
	uint32_t p = pha, q = phb;
	for (size_t vl; n; n -= vl, out += vl, p += uint32_t(vl) * ia, q += uint32_t(vl) * ib) {
		vl = __riscv_vsetvl_e16m1(n);
		const vint16m1_t m = v_op(wa, v_index(v_phases(p, ia, vl), vl), aa, vl);
		const vuint16m1_t idx = __riscv_vadd_vv_u16m1(v_index(v_phases(q, ib, vl), vl), __riscv_vreinterpret_v_i16m1_u16m1(m), vl);
		__riscv_vse16_v_i16m1(out, v_op(wb, idx, ab, vl), vl);
	}
	pha = p;
	phb = q;
}

// Feedback makes the modulator a chain in time: it stays scalar, its carrier does not.
void k_fm_fb(const Chip &c, uint32_t a, uint32_t b, uint32_t &pha, uint32_t &phb, int32_t &f0, int32_t &f1, uint32_t shift, int16_t *out, uint32_t n) {
	int16_t mod[BLOCK];
	k_fb(c, a, pha, f0, f1, shift, mod, n);
	k_car(c, b, phb, mod, out, n);
}

void k_fm_fb96(Chip &c, uint32_t a, uint32_t b, uint32_t ch, uint32_t shift, int16_t *out, uint32_t n) {
	int16_t mod[BLOCK];
	k_fb96(c, a, ch, shift, mod, n);
	k_car(c, b, c.ph[b], mod, out, n);
}

#elif defined(HV_RVV_MIX)

// The mix's fast path (hvopl3.cpp has the definition): a row into 32-bit sums, s x coefficient widened
// (vwmacc: 16 x 16 -> 32), every coefficient below 2^15 where the block takes this path. One loop a row:
// the samples loaded once, each sum read, added to and written (or set, where its bus begins: BF).
template <bool FIRST>
HV_INLINE vint32m2_t v_acc(const int32_t *p, size_t vl) { return FIRST ? __riscv_vmv_v_x_i32m2(0, vl) : __riscv_vle32_v_i32m2(p, vl); }
// F0, F1: the reverb's, the chorus's sums begin with this row
template <uint32_t BUS, bool F0, bool F1>
void v_row(Chip &c, const int16_t *row, uint32_t n, const int16_t *w) {
	int32_t *al = c.acc_l, *ar = c.acc_r, *b0 = c.bus_acc[0], *b1 = c.bus_acc[1], *b2 = c.bus_acc[2], *b3 = c.bus_acc[3];
	const int16_t w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3], w4 = w[4], w5 = w[5];
	for (size_t vl, j = 0; j < n; j += vl) {
		vl = __riscv_vsetvl_e16m1(n - j);
		const vint16m1_t s = __riscv_vle16_v_i16m1(row + j, vl);
		__riscv_vse32_v_i32m2(al + j, __riscv_vwmacc_vx_i32m2(__riscv_vle32_v_i32m2(al + j, vl), w0, s, vl), vl);
		__riscv_vse32_v_i32m2(ar + j, __riscv_vwmacc_vx_i32m2(__riscv_vle32_v_i32m2(ar + j, vl), w1, s, vl), vl);
		if (BUS & 1) {
			__riscv_vse32_v_i32m2(b0 + j, __riscv_vwmacc_vx_i32m2(v_acc<F0>(b0 + j, vl), w2, s, vl), vl);
			__riscv_vse32_v_i32m2(b1 + j, __riscv_vwmacc_vx_i32m2(v_acc<F0>(b1 + j, vl), w3, s, vl), vl);
		}
		if (BUS & 2) {
			__riscv_vse32_v_i32m2(b2 + j, __riscv_vwmacc_vx_i32m2(v_acc<F1>(b2 + j, vl), w4, s, vl), vl);
			__riscv_vse32_v_i32m2(b3 + j, __riscv_vwmacc_vx_i32m2(v_acc<F1>(b3 + j, vl), w5, s, vl), vl);
		}
	}
}
// bus (in/out): the buses with sums this block; a bus's first row sets its sums.
void mix_row(Chip &c, const Output &o, uint32_t ch, const int16_t *row, uint32_t n, uint32_t &bus) {
	const int32_t cl = c.coef_l[ch], cr = c.coef_r[ch];
	const int32_t q = o.rev ? c.rev_q[ch] : 0, g = o.cho ? c.cho_q[ch] : 0;
	const int16_t w[6] = {int16_t(cl), int16_t(cr), int16_t(send_coef(cl, q)), int16_t(send_coef(cr, q)), int16_t(send_coef(cl, g)), int16_t(send_coef(cr, g))};
	const uint32_t want = (q ? 1u : 0u) | (g ? 2u : 0u), bf = want & ~bus;
	bus |= want;
	switch (want << 2 | bf) {
	case 0: v_row<0, false, false>(c, row, n, w); break;
	case 4: v_row<1, false, false>(c, row, n, w); break;
	case 5: v_row<1, true, false>(c, row, n, w); break;
	case 8: v_row<2, false, false>(c, row, n, w); break;
	case 10: v_row<2, false, true>(c, row, n, w); break;
	case 12: v_row<3, false, false>(c, row, n, w); break;
	case 13: v_row<3, true, false>(c, row, n, w); break;
	case 14: v_row<3, false, true>(c, row, n, w); break;
	default: v_row<3, true, true>(c, row, n, w); break;
	}
}
// The sums rounded: dry (Q12), clipped unless NO_CLIP; buses (Q14), added to the caller's.
void dry_out(const Chip &c, int32_t *dry, bool add, uint32_t n) {
	const bool clip = !(c.flags & NO_CLIP);
	const int32_t *al = c.acc_l, *ar = c.acc_r;
	for (size_t vl; n; n -= vl, al += vl, ar += vl, dry += 2 * vl) {
		vl = __riscv_vsetvl_e32m2(n);
		vint32m2_t l = __riscv_vsra_vx_i32m2(__riscv_vadd_vx_i32m2(__riscv_vle32_v_i32m2(al, vl), 2048, vl), 12, vl);
		vint32m2_t r = __riscv_vsra_vx_i32m2(__riscv_vadd_vx_i32m2(__riscv_vle32_v_i32m2(ar, vl), 2048, vl), 12, vl);
		if (clip) {
			l = __riscv_vmax_vx_i32m2(__riscv_vmin_vx_i32m2(l, 32767, vl), -32768, vl);
			r = __riscv_vmax_vx_i32m2(__riscv_vmin_vx_i32m2(r, 32767, vl), -32768, vl);
		}
		if (add) {
			l = __riscv_vadd_vv_i32m2(__riscv_vlse32_v_i32m2(dry, 8, vl), l, vl);
			r = __riscv_vadd_vv_i32m2(__riscv_vlse32_v_i32m2(dry + 1, 8, vl), r, vl);
		}
		__riscv_vsse32_v_i32m2(dry, 8, l, vl);
		__riscv_vsse32_v_i32m2(dry + 1, 8, r, vl);
	}
}
void bus_out(const Chip &c, const Output &o, uint32_t off, uint32_t bus, uint32_t n) {
	for (uint32_t k = 0; k < 2; ++k) {
		if (!(bus >> k & 1)) continue;
		for (uint32_t side = 0; side < 2; ++side) {
			int32_t *b = (k ? o.cho : o.rev) + 2 * off + side;
			const int32_t *s = c.bus_acc[2 * k + side];
			for (size_t vl, m = n; m; m -= vl, s += vl, b += 2 * vl) {
				vl = __riscv_vsetvl_e32m2(m);
				const vint32m2_t v = __riscv_vsra_vx_i32m2(__riscv_vadd_vx_i32m2(__riscv_vle32_v_i32m2(s, vl), 8192, vl), 14, vl);
				__riscv_vsse32_v_i32m2(b, 8, __riscv_vadd_vv_i32m2(__riscv_vlse32_v_i32m2(b, 8, vl), v, vl), vl);
			}
		}
	}
}
#else   // HV_RVV_LANES

// Lanes across voices: FM voices with feedback, one sample of all of them at a time, so the
// feedback chain stays in each lane. Each voice's segment ends the lockstep chunk.

HV_INLINE bool lane_voice(const Chip &c, uint32_t ch) {
	const uint32_t a = 2 * ch;
	return c.kind[ch] == K_2OP && !(c.r_c0[ch] & 1) && c.fb_shift[ch] < 16 && c.att[a] < QUIET && c.att[a + 1] < QUIET;
}

// wave offset (bytes) per lane, attenuation per lane.
HV_INLINE vint16m1_t v_op_lane(vuint16m1_t idx, vuint16m1_t wo, vuint16m1_t att, size_t vl) {
	const vuint16m1_t off = __riscv_vadd_vv_u16m1(__riscv_vsll_vx_u16m1(__riscv_vand_vx_u16m1(idx, 1023, vl), 1, vl), wo, vl);
	const vuint16m1_t e = __riscv_vadd_vv_u16m1(__riscv_vluxei16_v_u16m1(&WAVES.w[0][0], off, vl), att, vl);
	const vuint16m1_t frac = __riscv_vsll_vx_u16m1(__riscv_vand_vx_u16m1(e, 255, vl), 1, vl);
	const vuint16m1_t whole = __riscv_vminu_vx_u16m1(__riscv_vand_vx_u16m1(__riscv_vsrl_vx_u16m1(e, 8, vl), 0x7F, vl), 15, vl);
	const vuint16m1_t v = __riscv_vsrl_vv_u16m1(__riscv_vluxei16_v_u16m1(POW.v, frac, vl), whole, vl);
	const vint16m1_t s = __riscv_vsra_vx_i16m1(__riscv_vreinterpret_v_u16m1_i16m1(e), 15, vl);
	return __riscv_vxor_vv_i16m1(__riscv_vreinterpret_v_u16m1_i16m1(v), s, vl);
}

// `count` voices (at most one vector's worth), `n` samples, starting at row offset `done`.
void k_lanes(Chip &c, const uint8_t *chs, const uint8_t *rowi, uint32_t count, uint32_t done, uint32_t n) {
	uint32_t *pa = c.l_pa, *pb = c.l_pb, *ia = c.l_ia, *ib = c.l_ib;
	uint16_t *aa = c.l_aa, *ab = c.l_ab, *wa = c.l_wa, *wb = c.l_wb, *sh = c.l_sh;
	int16_t *y1 = c.l_y1, *y2 = c.l_y2, *y3 = c.l_y3, *y4 = c.l_y4;
	for (uint32_t k = 0; k < count; ++k) {
		const uint32_t ch = chs[k], a = 2 * ch, b = a + 1;
		pa[k] = c.ph[a]; pb[k] = c.ph[b]; ia[k] = c.inc[a]; ib[k] = c.inc[b];
		aa[k] = c.att[a]; ab[k] = c.att[b];
		wa[k] = uint16_t(c.wave[a] * 2048u); wb[k] = uint16_t(c.wave[b] * 2048u);
		sh[k] = c.fb_shift[ch];
		y1[k] = c.fb0[ch]; y2[k] = c.fb1[ch]; y3[k] = c.fb2[ch]; y4[k] = c.fb3[ch];
	}
	const size_t vl = __riscv_vsetvl_e16m1(count);
	vuint32m2_t Pa = __riscv_vle32_v_u32m2(pa, vl), Pb = __riscv_vle32_v_u32m2(pb, vl);
	const vuint32m2_t Ia = __riscv_vle32_v_u32m2(ia, vl), Ib = __riscv_vle32_v_u32m2(ib, vl);
	const vuint16m1_t Aa = __riscv_vle16_v_u16m1(aa, vl), Ab = __riscv_vle16_v_u16m1(ab, vl);
	const vuint16m1_t Wa = __riscv_vle16_v_u16m1(wa, vl), Wb = __riscv_vle16_v_u16m1(wb, vl), Sh = __riscv_vle16_v_u16m1(sh, vl);
	vint16m1_t Y1 = __riscv_vle16_v_i16m1(y1, vl), Y2 = __riscv_vle16_v_i16m1(y2, vl);
	vint16m1_t Y3 = __riscv_vle16_v_i16m1(y3, vl), Y4 = __riscv_vle16_v_i16m1(y4, vl);
	const bool r96 = c.fb_step == 2;
	// the rows of these lanes are consecutive (lanes_pass), so a sample is one strided store
	int16_t *dst = &c.rows[rowi[0]][done];
	for (uint32_t j = 0; j < n; ++j, ++dst) {
		Pa = __riscv_vadd_vv_u32m2(Pa, Ia, vl);
		Pb = __riscv_vadd_vv_u32m2(Pb, Ib, vl);
		const vint16m1_t sum = r96 ? __riscv_vadd_vv_i16m1(Y2, Y4, vl) : __riscv_vadd_vv_i16m1(Y1, Y2, vl);
		const vuint16m1_t mod = __riscv_vreinterpret_v_i16m1_u16m1(__riscv_vsra_vv_i16m1(sum, Sh, vl));
		const vint16m1_t m = v_op_lane(__riscv_vadd_vv_u16m1(__riscv_vnsrl_wx_u16m1(Pa, 22, vl), mod, vl), Wa, Aa, vl);
		if (r96) { Y4 = Y3; Y3 = Y2; }
		Y2 = Y1;
		Y1 = m;
		const vuint16m1_t idx = __riscv_vadd_vv_u16m1(__riscv_vnsrl_wx_u16m1(Pb, 22, vl), __riscv_vreinterpret_v_i16m1_u16m1(m), vl);
		__riscv_vsse16_v_i16m1(dst, sizeof c.rows[0], v_op_lane(idx, Wb, Ab, vl), vl);
	}
	__riscv_vse32_v_u32m2(pa, Pa, vl); __riscv_vse32_v_u32m2(pb, Pb, vl);
	__riscv_vse16_v_i16m1(y1, Y1, vl); __riscv_vse16_v_i16m1(y2, Y2, vl);
	__riscv_vse16_v_i16m1(y3, Y3, vl); __riscv_vse16_v_i16m1(y4, Y4, vl);
	for (uint32_t k = 0; k < count; ++k) {
		const uint32_t ch = chs[k], a = 2 * ch;
		c.ph[a] = pa[k]; c.ph[a + 1] = pb[k];
		c.fb0[ch] = y1[k]; c.fb1[ch] = y2[k]; c.fb2[ch] = y3[k]; c.fb3[ch] = y4[k];
	}
}

#ifdef HV_RVV_HQ
// High quality in lanes across voices: seg_hq's arithmetic, op for op in its order, one lane a voice.
// Float lanes round as the scalar code does: no fused multiply-add, the truncating conversions rtz, the
// polynomials in the same order, every select a merge of what the scalar code computes on that side.
// An operator at a time over a sub-block of samples: only op 1's feedback is a chain in time, the
// others take the previous operator's outputs of the same sample from h_v.
typedef vfloat32m1_t VF;
typedef vuint32m1_t VU;
typedef vint32m1_t VI;
typedef vbool32_t VB;
// h_bits: op 1 has feedback; op j is modulated (2 << (j - 1)); op j is heard (16 << j); the lane writes its row
enum : uint32_t { HQ_SUB = 16, HB_FB = 1, HB_MOD = 2, HB_HEARD = 16, HB_WRITE = 256 };

// The constants, in registers: GCC otherwise reloads each from memory at every use in the loops.
struct HqK { float e5, e4, e3, e2, e1, s5, s4, s3, s2, s1, half, quarter, one, zero, mone, two, four, r256, r1024, r4096, r2p24, k1024, k8192; };
#define HV_REG(x) asm("" : "+f"(x))
HV_INLINE HqK hq_consts() {
	HqK k;
	{ float x = 0.00189511f; HV_REG(x); k.e5 = x; } { float x = 0.00894621f; HV_REG(x); k.e4 = x; } { float x = 0.05586328f; HV_REG(x); k.e3 = x; } { float x = 0.24014077f; HV_REG(x); k.e2 = x; } { float x = 0.69315462f; HV_REG(x); k.e1 = x; } { float x = 0.00015148f; HV_REG(x); k.s5 = x; }
	{ float x = -0.00467377f; HV_REG(x); k.s4 = x; } { float x = 0.07968968f; HV_REG(x); k.s3 = x; } { float x = -0.64596371f; HV_REG(x); k.s2 = x; } { float x = 1.57079632f; HV_REG(x); k.s1 = x; } { float x = 0.5f; HV_REG(x); k.half = x; } { float x = 0.25f; HV_REG(x); k.quarter = x; }
	{ float x = 1.0f; HV_REG(x); k.one = x; } { float x = 0.0f; HV_REG(x); k.zero = x; } { float x = -1.0f; HV_REG(x); k.mone = x; } { float x = 2.0f; HV_REG(x); k.two = x; } { float x = 4.0f; HV_REG(x); k.four = x; } { float x = 1.0f / 256.0f; HV_REG(x); k.r256 = x; }
	{ float x = 1.0f / 1024.0f; HV_REG(x); k.r1024 = x; } { float x = 1.0f / 4096.0f; HV_REG(x); k.r4096 = x; } { float x = 1.0f / 16777216.0f; HV_REG(x); k.r2p24 = x; } { float x = 1024.0f; HV_REG(x); k.k1024 = x; } { float x = 8192.0f; HV_REG(x); k.k8192 = x; }
	return k;
}

// C's float -> int (toward zero). RVV 1.0 has it (rtz); RVV 0.7.1 converts by frm only, so: convert x >= 0,
// convert back (exact: n < 2^24, or x was already an integer), and step n down by one where it went past x.
// Right for any frm, so no CSR is touched; the same integer as rtz for every x in range. v_trunc_pos: x >= 0
// (or -0); v_trunc: any sign, by |x| and the sign put back.
HV_INLINE VI v_trunc_pos(VF x, size_t vl) {
#ifndef HV_NO_RTZ
	return __riscv_vfcvt_rtz_x_f_v_i32m1(x, vl);
#else
	const VI n = __riscv_vfcvt_x_f_v_i32m1(x, vl);
	return __riscv_vsub_vx_i32m1_mu(__riscv_vmflt_vv_f32m1_b32(x, __riscv_vfcvt_f_x_v_f32m1(n, vl), vl), n, n, 1, vl);
#endif
}
HV_INLINE VI v_trunc(VF x, size_t vl) {
#ifndef HV_NO_RTZ
	return __riscv_vfcvt_rtz_x_f_v_i32m1(x, vl);
#else
	const VI n = v_trunc_pos(__riscv_vfsgnjx_vv_f32m1(x, x, vl), vl);
	return __riscv_vrsub_vx_i32m1_mu(__riscv_vmflt_vf_f32m1_b32(x, 0.0f, vl), n, n, 0, vl);
#endif
}

// 16-bit table entries gathered into 32-bit lanes (byte offsets), and lanes' low 16 bits scattered (masked).
// RVV 0.7.1 has no fractional LMUL; it loads and stores halfwords at SEW 32 instead (th.vlxhu, th.vsxh).
HV_INLINE VU v_gather_u16(const uint16_t *b, VU off, size_t vl) {
#ifdef __riscv_xtheadvector
	return __riscv_th_vlxhu_v_u32m1(reinterpret_cast<const uint32_t *>(b), off, vl);
#else
	return __riscv_vzext_vf2_u32m1(__riscv_vluxei32_v_u16mf2(b, off, vl), vl);
#endif
}
HV_INLINE void v_scatter_i16(VB m, int16_t *b, VU off, VI v, size_t vl) {
#ifdef __riscv_xtheadvector
	__riscv_th_vsxh_v_i32m1_m(m, reinterpret_cast<int32_t *>(b), off, v, vl);
#else
	__riscv_vsuxei32_v_i16mf2_m(m, b, off, __riscv_vncvt_x_x_w_i16mf2(v, vl), vl);
#endif
}

HV_INLINE VF v_exp2_frac(const HqK &k, VF x, size_t vl) {
	VF t = __riscv_vfmul_vf_f32m1(x, k.e5, vl);
	t = __riscv_vfmul_vv_f32m1(x, __riscv_vfadd_vf_f32m1(t, k.e4, vl), vl);
	t = __riscv_vfmul_vv_f32m1(x, __riscv_vfadd_vf_f32m1(t, k.e3, vl), vl);
	t = __riscv_vfmul_vv_f32m1(x, __riscv_vfadd_vf_f32m1(t, k.e2, vl), vl);
	t = __riscv_vfmul_vv_f32m1(x, __riscv_vfadd_vf_f32m1(t, k.e1, vl), vl);
	return __riscv_vfadd_vf_f32m1(t, k.one, vl);
}
HV_INLINE VF v_atten_gain(const HqK &k, VF a, size_t vl) {
	const VI w = v_trunc_pos(a, vl);   // a >= 0: an attenuation
	const VF f = __riscv_vfsub_vv_f32m1(a, __riscv_vfcvt_f_x_v_f32m1(w, vl), vl);
	const VI sh = __riscv_vsra_vx_i32m1(w, 8, vl);
	const VF x = __riscv_vfadd_vv_f32m1(__riscv_vfcvt_f_x_v_f32m1(__riscv_vand_vx_i32m1(w, 255, vl), vl), f, vl);
	const VF m = v_exp2_frac(k, __riscv_vfrsub_vf_f32m1(__riscv_vfmul_vf_f32m1(x, k.r256, vl), k.one, vl), vl);
	const VF p = __riscv_vreinterpret_v_i32m1_f32m1(__riscv_vsll_vx_i32m1(__riscv_vrsub_vx_i32m1(sh, 127 + 11, vl), 23, vl));
	return __riscv_vfmerge_vfm_f32m1(__riscv_vfmul_vv_f32m1(m, p, vl), k.zero, __riscv_vmsgt_vx_i32m1_b32(sh, 24, vl), vl);
}
HV_INLINE VF v_sin_cycle(const HqK &k, VF x, size_t vl) {
	const VB lo = __riscv_vmflt_vf_f32m1_b32(x, k.half, vl);
	const VF h = __riscv_vmerge_vvm_f32m1(__riscv_vfsub_vf_f32m1(x, k.half, vl), x, lo, vl);
	const VF q = __riscv_vmerge_vvm_f32m1(__riscv_vfrsub_vf_f32m1(h, k.half, vl), h, __riscv_vmflt_vf_f32m1_b32(h, k.quarter, vl), vl);
	const VF t = __riscv_vfmul_vf_f32m1(q, k.four, vl), t2 = __riscv_vfmul_vv_f32m1(t, t, vl);
	VF u = __riscv_vfmul_vf_f32m1(t2, k.s5, vl);
	u = __riscv_vfmul_vv_f32m1(t2, __riscv_vfadd_vf_f32m1(u, k.s4, vl), vl);
	u = __riscv_vfmul_vv_f32m1(t2, __riscv_vfadd_vf_f32m1(u, k.s3, vl), vl);
	u = __riscv_vfmul_vv_f32m1(t2, __riscv_vfadd_vf_f32m1(u, k.s2, vl), vl);
	const VF v = __riscv_vfmul_vv_f32m1(t, __riscv_vfadd_vf_f32m1(u, k.s1, vl), vl);
	return __riscv_vmerge_vvm_f32m1(__riscv_vfneg_v_f32m1(v, vl), v, lo, vl);
}
HV_INLINE VF v_frac_cycle(const HqK &k, VF x, size_t vl) {
#ifdef HV_NO_RTZ
	// n rounded to nearest (frm's default, which every float operation here already assumes) instead of
	// truncated: where they differ, x - n is (x - trunc(x)) - 1 for x > 0, exact as the fraction is then 1/2 or
	// more, and gains its 1 back below; for x < 0 it is the one rounding of the same value the + 1 below gives.
	const VF f = __riscv_vfsub_vv_f32m1(x, __riscv_vfcvt_f_x_v_f32m1(__riscv_vfcvt_x_f_v_i32m1(x, vl), vl), vl);
#else
	const VF f = __riscv_vfsub_vv_f32m1(x, __riscv_vfcvt_f_x_v_f32m1(v_trunc(x, vl), vl), vl);
#endif
	return __riscv_vmerge_vvm_f32m1(f, __riscv_vfadd_vf_f32m1(f, k.one, vl), __riscv_vmflt_vf_f32m1_b32(f, k.zero, vl), vl);
}
// s < 0 ? -s : s, as the scalar code has it (-0 stays -0)
HV_INLINE VF v_abs_lt(const HqK &k, VF s, size_t vl) {
	return __riscv_vmerge_vvm_f32m1(s, __riscv_vfneg_v_f32m1(s, vl), __riscv_vmflt_vf_f32m1_b32(s, k.zero, vl), vl);
}
// wave_at with each lane's waveform; `shapes`: the waveforms any lane has, a bit each. Waveforms 0-5
// are one sine: of x, its half or twice it; negated where it is negative (2, 5); 0 where it is (1, 3-5).
// The masks, of each lane's waveform: m_half 3 (the sine of x's half), m_twice 4-5 (of twice x), m_zlo 1, 4, 5
// (0 from x = 1/2 on), m_abs 2, 5, m_sq 6, m_saw 7.
HV_INLINE VF v_wave_at(const HqK &k, VB m_half, VB m_twice, VB m_zlo, VB m_abs, VB m_sq, VB m_saw, VF x, uint32_t shapes, size_t vl) {
	const VB lo = __riscv_vmflt_vf_f32m1_b32(x, k.half, vl);
	VF r = __riscv_vfmv_v_f_f32m1(k.zero, vl);
	if (shapes & 0x3F) {
		VF arg = x;
		VB z = __riscv_vmandn_mm_b32(m_zlo, lo, vl);
		if (shapes & 0x08) {
			const VF h = __riscv_vmerge_vvm_f32m1(__riscv_vfsub_vf_f32m1(x, k.half, vl), x, lo, vl);
			arg = __riscv_vmerge_vvm_f32m1(arg, h, m_half, vl);
			z = __riscv_vmor_mm_b32(z, __riscv_vmand_mm_b32(m_half, __riscv_vmfge_vf_f32m1_b32(h, k.quarter, vl), vl), vl);
		}
		if (shapes & 0x30) arg = __riscv_vmerge_vvm_f32m1(arg, __riscv_vfmul_vf_f32m1(x, k.two, vl), m_twice, vl);
		r = v_sin_cycle(k, arg, vl);
		if (shapes & 0x24) r = __riscv_vfneg_v_f32m1_mu(__riscv_vmand_mm_b32(m_abs, __riscv_vmflt_vf_f32m1_b32(r, k.zero, vl), vl), r, r, vl);
		if (shapes & 0x3A) r = __riscv_vfmerge_vfm_f32m1(r, k.zero, z, vl);
	}
	if (shapes & 0x40) r = __riscv_vmerge_vvm_f32m1(r, __riscv_vfmerge_vfm_f32m1(__riscv_vfmv_v_f_f32m1(k.mone, vl), k.one, lo, vl), m_sq, vl);
	if (shapes & 0x80) {
		const VF d = __riscv_vmerge_vvm_f32m1(__riscv_vfrsub_vf_f32m1(x, k.one, vl), x, lo, vl);
		const VF g = __riscv_vfmul_vf_f32m1(v_atten_gain(k, __riscv_vfmul_vf_f32m1(d, k.k8192, vl), vl), k.r4096, vl);
		r = __riscv_vmerge_vvm_f32m1(r, __riscv_vmerge_vvm_f32m1(__riscv_vfneg_v_f32m1(g, vl), g, lo, vl), m_saw, vl);
	}
	return r;
}

// One operator (slot j) of lanes [b, b + vl) over m samples of a sub-block. Reads h_v[(j - 1) & 1] (the
// previous operator's outputs), writes h_v[j & 1], adds to h_sum (op 1 starts it); LAST rounds the sums
// into the rows at `done` instead. J0: op 1, with feedback; PHASE: HQ_PHASE.
template <bool J0, bool LAST, bool PHASE>
HV_INLINE void hq_op(Chip &c, const HqK &k, uint32_t j, uint32_t b, size_t vl, uint32_t m, uint32_t shapes, uint32_t done) {
	const bool hq_mod = c.flags & HQ_MODULATION, r96 = c.fb_step == 2;
	VU P = __riscv_vle32_v_u32m1(&c.h_ph[j][b], vl);
	const VU I = __riscv_vle32_v_u32m1(&c.h_inc[j][b], vl), WS = __riscv_vle32_v_u32m1(&c.h_ws[j][b], vl);
	VF D = __riscv_vle32_v_f32m1(&c.h_d[j][b], vl);
	const VF FROM = __riscv_vle32_v_f32m1(&c.h_from[j][b], vl), SLOPE = __riscv_vle32_v_f32m1(&c.h_slope[j][b], vl);
	const VU BITS = __riscv_vle32_v_u32m1(&c.h_bits[b], vl);
	const VB heard = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(BITS, HB_HEARD << j, vl), 0, vl);
	const VB modded = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(BITS, J0 ? HB_FB : HB_MOD << (j - 1), vl), 0, vl);
	const VB write = __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(BITS, HB_WRITE, vl), 0, vl);
	const VF ZERO = __riscv_vfmv_v_f_f32m1(k.zero, vl);
	VF F0, F1, F2, F3, FBK;
	VU FBS;
	if (J0) {
		F0 = __riscv_vle32_v_f32m1(&c.h_f[0][b], vl); F1 = __riscv_vle32_v_f32m1(&c.h_f[1][b], vl);
		F2 = __riscv_vle32_v_f32m1(&c.h_f[2][b], vl); F3 = __riscv_vle32_v_f32m1(&c.h_f[3][b], vl);
		FBK = __riscv_vle32_v_f32m1(&c.h_fbk[b], vl);
		FBS = __riscv_vle32_v_u32m1(&c.h_fbs[b], vl);
	}
	const VU ROW = __riscv_vle32_v_u32m1(&c.h_row[b], vl);
	const VB m_half = __riscv_vmseq_vx_u32m1_b32(WS, 3, vl), m_twice = __riscv_vmsgtu_vx_u32m1_b32(WS, 3, vl);
	const VB m_zlo = __riscv_vmor_mm_b32(__riscv_vmseq_vx_u32m1_b32(WS, 1, vl), m_twice, vl);
	const VB m_abs = __riscv_vmor_mm_b32(__riscv_vmseq_vx_u32m1_b32(WS, 2, vl), __riscv_vmseq_vx_u32m1_b32(WS, 5, vl), vl);
	const VB m_sq = __riscv_vmseq_vx_u32m1_b32(WS, 6, vl), m_saw = __riscv_vmseq_vx_u32m1_b32(WS, 7, vl);
	const float *vin = &c.h_v[(j - 1) & 1][0][b];
	float *vout = &c.h_v[j & 1][0][b], *sum = &c.h_sum[0][b];
	int16_t *out = &c.rows[0][done];
	for (uint32_t s = 0; s < m; ++s, vin += CHANNELS, vout += CHANNELS, sum += CHANNELS, ++out) {
		P = __riscv_vadd_vv_u32m1(P, I, vl);
		VF M;   // the modulation, in cycles
		if (J0) {
			VF a = F0, bb = F1;
			if (r96) { a = F1; bb = F3; }
			if (hq_mod) M = __riscv_vfmul_vv_f32m1(__riscv_vfadd_vv_f32m1(a, bb, vl), FBK, vl);
			else {
				const VI t = __riscv_vadd_vv_i32m1(v_trunc(a, vl), v_trunc(bb, vl), vl);
				M = __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_x_v_f32m1(__riscv_vsra_vv_i32m1(t, FBS, vl), vl), k.r1024, vl);
			}
		} else {
			const VF v = __riscv_vle32_v_f32m1(vin, vl);
			M = __riscv_vfmul_vf_f32m1(hq_mod ? v : __riscv_vfcvt_f_x_v_f32m1(v_trunc(v, vl), vl), k.r1024, vl);
		}
		M = __riscv_vmerge_vvm_f32m1(ZERO, M, modded, vl);
		VF w;
		if (PHASE) {
			const VF ph = __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_xu_v_f32m1(__riscv_vsrl_vx_u32m1(P, 8, vl), vl), k.r2p24, vl);
			w = v_wave_at(k, m_half, m_twice, m_zlo, m_abs, m_sq, m_saw, v_frac_cycle(k, __riscv_vfadd_vv_f32m1(ph, M, vl), vl), shapes, vl);
		} else {
			const VF ph = __riscv_vfmul_vf_f32m1(__riscv_vfcvt_f_xu_v_f32m1(__riscv_vsrl_vx_u32m1(P, 22, vl), vl), k.r1024, vl);
			const VF fr = __riscv_vfmul_vf_f32m1(v_frac_cycle(k, __riscv_vfadd_vv_f32m1(ph, M, vl), vl), k.k1024, vl);
			const VU idx = __riscv_vand_vx_u32m1(__riscv_vreinterpret_v_i32m1_u32m1(v_trunc(fr, vl)), 1023, vl);
			const VU off = __riscv_vadd_vv_u32m1(__riscv_vsll_vx_u32m1(WS, 11, vl), __riscv_vsll_vx_u32m1(idx, 1, vl), vl);
			const VU e = v_gather_u16(&WAVES.w[0][0], off, vl);
			const VF a = __riscv_vfmul_vf_f32m1(v_atten_gain(k, __riscv_vfcvt_f_xu_v_f32m1(__riscv_vand_vx_u32m1(e, 0x7FFF, vl), vl), vl), k.r4096, vl);
			w = __riscv_vmerge_vvm_f32m1(a, __riscv_vfneg_v_f32m1(a, vl), __riscv_vmsne_vx_u32m1_b32(__riscv_vand_vx_u32m1(e, 0x8000, vl), 0, vl), vl);
		}
		const VF v = __riscv_vfmul_vv_f32m1(w, __riscv_vfadd_vv_f32m1(FROM, __riscv_vfmul_vv_f32m1(SLOPE, D, vl), vl), vl);
		D = __riscv_vfadd_vf_f32m1(D, k.one, vl);
		const VF s0 = J0 ? ZERO : __riscv_vle32_v_f32m1(sum, vl);
		const VF t = __riscv_vmerge_vvm_f32m1(s0, __riscv_vfadd_vv_f32m1(s0, v, vl), heard, vl);
		if (LAST) {
			const VF r = __riscv_vmerge_vvm_f32m1(__riscv_vfadd_vf_f32m1(t, k.half, vl), __riscv_vfsub_vf_f32m1(t, k.half, vl), __riscv_vmflt_vf_f32m1_b32(t, k.zero, vl), vl);
			v_scatter_i16(write, out, ROW, v_trunc(r, vl), vl);
		} else {
			__riscv_vse32_v_f32m1(vout, v, vl);
			__riscv_vse32_v_f32m1(sum, t, vl);
		}
		if (J0) { F3 = F2; F2 = F1; F1 = F0; F0 = v; }
	}
	__riscv_vse32_v_u32m1(&c.h_ph[j][b], P, vl);
	__riscv_vse32_v_f32m1(&c.h_d[j][b], D, vl);
	if (J0) {
		__riscv_vse32_v_f32m1(&c.h_f[0][b], F0, vl); __riscv_vse32_v_f32m1(&c.h_f[1][b], F1, vl);
		__riscv_vse32_v_f32m1(&c.h_f[2][b], F2, vl); __riscv_vse32_v_f32m1(&c.h_f[3][b], F3, vl);
	}
}

// A chunk of n samples from row offset `done`, lanes [b, b + vl): every operator, a sub-block at a time.
template <bool PHASE, uint32_t NOPS>
__attribute__((noinline)) void hq_chunk(Chip &c, uint32_t b, size_t vl, uint32_t n, uint32_t shapes, uint32_t done) {
	const HqK k = hq_consts();
	for (uint32_t s = 0; s < n; s += HQ_SUB) {
		const uint32_t m = n - s < HQ_SUB ? n - s : HQ_SUB;
		hq_op<true, false, PHASE>(c, k, 0, b, vl, m, shapes & 255, done + s);
		if (NOPS == 2) hq_op<false, true, PHASE>(c, k, 1, b, vl, m, shapes >> 8 & 255, done + s);
		else {
			hq_op<false, false, PHASE>(c, k, 1, b, vl, m, shapes >> 8 & 255, done + s);
			hq_op<false, false, PHASE>(c, k, 2, b, vl, m, shapes >> 16 & 255, done + s);
			hq_op<false, true, PHASE>(c, k, 3, b, vl, m, shapes >> 24, done + s);
		}
	}
}

// A voice into its lane from sample `at`: seg_hq's set-up (its gain lines drawn as there). Returns
// whether it is heard; a quiet one stays in its lane (its phases run there) but writes nothing.
bool hq_lane(Chip &c, uint32_t ch, uint32_t at, uint32_t lane, uint32_t row, uint32_t &shapes) {
	const OpList l = voice_ops(c, ch);
	const Route route = voice_route(c, ch, l.n);
	bool quiet = true;
	for (uint32_t j = 0; j < l.n; ++j)
		if ((route.heard >> j & 1) && c.att[l.o[j]] < QUIET) quiet = false;
	uint32_t bits = uint32_t(route.heard) * HB_HEARD, sh = 0;
	for (uint32_t j = 0; j < 4; ++j) {
		if (j < l.n) {
			const uint32_t i = l.o[j];
			if (c.flags & HQ_ENVELOPE) {
				const GainLine g = gain_line(c, i, at, false);
				c.h_from[j][lane] = g.from; c.h_slope[j][lane] = g.slope; c.h_d[j][lane] = g.d;
			} else {
				c.h_from[j][lane] = atten_gain(float(c.att[i])); c.h_slope[j][lane] = 0.0f; c.h_d[j][lane] = 0.0f;
			}
			c.h_ph[j][lane] = c.ph[i]; c.h_inc[j][lane] = c.inc[i]; c.h_ws[j][lane] = c.wave[i];
			sh |= (1u << c.wave[i]) << (8 * j);
			if (j && route.mod[j] >= 0) bits |= HB_MOD << (j - 1);
		} else {   // a 2-op voice among 4-op ones: its ops 3 and 4 make nothing that is used
			c.h_ph[j][lane] = 0; c.h_inc[j][lane] = 0; c.h_ws[j][lane] = 0;
			c.h_from[j][lane] = 0.0f; c.h_slope[j][lane] = 0.0f; c.h_d[j][lane] = 0.0f;
		}
	}
	const uint32_t fbs = c.fb_shift[ch];
	if (fbs < 16) bits |= HB_FB;
	c.h_fbk[lane] = fbs < 16 ? 1.0f / float(1u << fbs) / 1024.0f : 0.0f;
	c.h_fbs[lane] = fbs < 16 ? fbs : 0;
	if (quiet) c.hq_fb0[ch] = c.hq_fb1[ch] = c.hq_fb2[ch] = c.hq_fb3[ch] = 0.0f;
	else bits |= HB_WRITE;
	c.h_f[0][lane] = c.hq_fb0[ch]; c.h_f[1][lane] = c.hq_fb1[ch]; c.h_f[2][lane] = c.hq_fb2[ch]; c.h_f[3][lane] = c.hq_fb3[ch];
	c.h_bits[lane] = bits;
	c.h_row[lane] = row * sizeof c.rows[0];
	c.h_shp[lane] = sh;
	c.h_slot[ch] = uint8_t(lane); c.h_ch[lane] = uint8_t(ch);
	c.h_ok |= 1u << ch;
	shapes |= sh;
	return !quiet;
}

// Its phases and feedback back to the voice (a quiet voice's feedback is 0, as seg_hq leaves it).
HV_INLINE void hq_unlane(Chip &c, uint32_t ch, uint32_t lane) {
	const OpList l = voice_ops(c, ch);
	for (uint32_t j = 0; j < l.n; ++j) c.ph[l.o[j]] = c.h_ph[j][lane];
	if (!(c.h_bits[lane] & HB_WRITE)) return;
	c.hq_fb0[ch] = c.h_f[0][lane]; c.hq_fb1[ch] = c.h_f[1][lane]; c.hq_fb2[ch] = c.h_f[2][lane]; c.hq_fb3[ch] = c.h_f[3][lane];
}

// voice_fill for every voice but the rhythm pair at once, a lane each, in lockstep chunks that end at
// any voice's event; a voice's lane is set up again only at its own events (where seg_hq's segments
// start). Mixes them; returns the active voices left for the others.
__attribute__((noinline)) uint32_t hq_lanes_pass(Chip &c, const Output &o, uint32_t n, bool &any, uint32_t &bus, bool fast) {
	uint8_t chs[CHANNELS];
	uint8_t st[CHANNELS];       // 0: gone (inactive), 1: quiet, 2: heard, 3: not in its lane yet
	uint8_t sounded[CHANNELS];
	uint8_t qfrom[CHANNELS];    // a quiet voice: quiet from this sample of the row
	uint32_t count = 0, mask = 0, four = 0, shapes = 0;
	for (uint32_t m = c.active; m; m &= m - 1) {
		const uint32_t ch = lowest_bit(m);
		if (c.kind[ch] == K_RHY || c.kind[ch] == K_NONE) continue;
		chs[count] = uint8_t(ch); sounded[count] = 0; st[count] = 3; qfrom[count] = 0;
		if (c.kind[ch] == K_4OP) four = 1;
		mask |= 1u << ch;
		++count;
	}
	if (!count) return c.active;
	const size_t vmax = __riscv_vsetvlmax_e32m1();
	uint32_t done = 0, at = c.now, live = count;
	for (uint32_t x = 0; x < count; ++x) {
		const uint32_t ch = chs[x];
		if (int32_t(at - c.next_event[ch]) >= 0) continue;   // its event first, below
		if ((c.h_ok >> ch & 1) && c.h_slot[ch] == x && c.h_ch[x] == ch) {   // as the last block left it
			st[x] = (c.h_bits[x] & HB_WRITE) ? 2 : 1;
			shapes |= c.h_shp[x];
		} else {
			st[x] = hq_lane(c, ch, at, x, x, shapes) ? 2 : 1;
		}
		sounded[x] = st[x] == 2;
	}
	while (done < n) {
		uint32_t seg = n - done;
		for (uint32_t x = 0; x < count; ++x) {
			if (!st[x]) continue;
			const uint32_t ch = chs[x];
			int32_t d = int32_t(c.next_event[ch] - at);
			if (d <= 0) {
				if (st[x] != 3) hq_unlane(c, ch, x);
				const uint32_t was = st[x];
				voice_event(c, ch, at);
				if (!(c.active >> ch & 1)) {
					if (sounded[x]) zero16(c.rows[x] + (was == 1 ? qfrom[x] : done), n - (was == 1 ? qfrom[x] : done));
					st[x] = 0; c.h_bits[x] = 0; c.h_ok &= ~(1u << ch);
					--live;
					continue;
				}
				st[x] = hq_lane(c, ch, at, x, x, shapes) ? 2 : 1;
				if (st[x] == 2 && was != 2) {   // heard from here: the samples before are 0
					if (sounded[x]) zero16(c.rows[x] + qfrom[x], done - qfrom[x]);
					else zero16(c.rows[x], done);
					sounded[x] = 1;
				} else if (st[x] == 1 && was != 1) {
					qfrom[x] = uint8_t(done);
				}
				d = int32_t(c.next_event[ch] - at);
			}
			if (uint32_t(d) < seg) seg = uint32_t(d);
		}
		if (!live) break;
		for (uint32_t b = 0; b < count; b += uint32_t(vmax)) {
			const size_t vl = __riscv_vsetvl_e32m1(count - b);
			if (c.flags & HQ_PHASE) {
				if (four) hq_chunk<true, 4>(c, b, vl, seg, shapes, done); else hq_chunk<true, 2>(c, b, vl, seg, shapes, done);
			} else {
				if (four) hq_chunk<false, 4>(c, b, vl, seg, shapes, done); else hq_chunk<false, 2>(c, b, vl, seg, shapes, done);
			}
		}
		done += seg;
		at += seg;
	}
	for (uint32_t x = 0; x < count; ++x) {
		if (!st[x]) continue;
		hq_unlane(c, chs[x], x);
		if (st[x] == 1 && sounded[x]) zero16(c.rows[x] + qfrom[x], n - qfrom[x]);
	}
	for (uint32_t x = 0; x < count; ++x) {
		if (!sounded[x]) continue;
		acc_begin(c, n, any, fast);
		mix_in(c, o, chs[x], c.rows[x], n, bus, fast);
	}
	return c.active & ~mask;
}
#endif  // HV_RVV_HQ

// Makes and mixes the voices the lanes can take; returns the active voices left for the others.
uint32_t lanes_pass(Chip &c, const Output &o, uint32_t n, bool &any, uint32_t &bus, bool fast) {
	if (c.flags & HQ_ANY) {
#ifdef HV_RVV_HQ
		return hq_lanes_pass(c, o, n, any, bus, fast);
#else
		return c.active;
#endif
	}
	uint8_t chs[CHANNELS], rowi[CHANNELS];
	uint32_t count = 0, done_mask = 0;
	for (uint32_t m = c.active; m; m &= m - 1) {
		const uint32_t ch = lowest_bit(m);
		if (lane_voice(c, ch)) { chs[count] = uint8_t(ch); rowi[count] = uint8_t(count); ++count; }
	}
	if (count < 2) return c.active;
	const size_t vmax = __riscv_vsetvlmax_e16m1();
	uint32_t done = 0, at = c.now;
	uint32_t live = count;       // chs[0..live) are still in lockstep, each with its row
	uint32_t left_at[CHANNELS];  // where a voice that left the lanes stopped
	uint8_t left_ch[CHANNELS], left_row[CHANNELS];
	uint32_t nleft = 0;
	while (done < n && live) {
		// events due: a voice that no longer fits leaves, and finishes alone below
		for (uint32_t k = 0; k < live;) {
			const uint32_t ch = chs[k];
			if (int32_t(at - c.next_event[ch]) >= 0) voice_event(c, ch, at);
			if (!lane_voice(c, ch) || !(c.active >> ch & 1)) {
				left_at[nleft] = done; left_ch[nleft] = uint8_t(ch); left_row[nleft] = rowi[k]; ++nleft;
				--live;
				for (uint32_t x = k; x < live; ++x) { chs[x] = chs[x + 1]; rowi[x] = rowi[x + 1]; }
			} else {
				++k;
			}
		}
		if (!live) break;
		uint32_t seg = n - done;
		for (uint32_t k = 0; k < live; ++k) {
			const uint32_t d = c.next_event[chs[k]] - at;
			if (d < seg) seg = d;
		}
		// groups of consecutive rows, a vector's worth each
		for (uint32_t k = 0; k < live;) {
			uint32_t g = 1;
			while (k + g < live && g < vmax && rowi[k + g] == rowi[k] + g) ++g;
			k_lanes(c, chs + k, rowi + k, g, done, seg);
			k += g;
		}
		done += seg;
		at += seg;
	}
	for (uint32_t k = 0; k < live; ++k) { left_at[nleft] = n; left_ch[nleft] = chs[k]; left_row[nleft] = rowi[k]; ++nleft; }
	for (uint32_t k = 0; k < nleft; ++k) {
		const uint32_t ch = left_ch[k];
		int16_t *row = c.rows[left_row[k]];
		done_mask |= 1u << ch;
		bool s = left_at[k] != 0;
		if (left_at[k] < n) s = voice_fill(c, ch, row, row, n, left_at[k], s);
		if (s) {
			acc_begin(c, n, any, fast);
			mix_in(c, o, ch, row, n, bus, fast);
		}
	}
	return c.active & ~done_mask;
}
#endif
