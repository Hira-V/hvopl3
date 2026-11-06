// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3: the mix with the Andes DSP extension (the A25's -mext-dsp).
// Included by hvopl3.cpp when HV_DSP is set: the fast path of the mix
// (hvopl3.cpp has its definition, which the scalar build gives too; bench/a25/digest.sh checks these bits).
//
// Rows in pairs, two voices' samples packed in a word; two products added to a sum an instruction. Where the block
// takes the fast path, mix_bound keeps every sum below 2^31, so kmada never saturates.
//   pkbb16 rd, a, b   rd = a.H[0] << 16 | b.H[0]
//   kmada rd, a, b    rd += a.H[1] x b.H[1] + a.H[0] x b.H[0] (saturating; never reached here)
//   kmmwb2.u rd, a, b (a x b.H[0] + 2^14) >> 15: with a = c << 3, round(s x c / 2^12) (the dry output of a row
//                     that is the block's only one); with a = q << 1, round(s x q / 2^14) (its buses)
//   sclip32 rd, a, 15 a limited to [-32768, 32767]: the dry clip
// (inline asm, as Hira-V's own A25 code writes them, silicon-checked there; rv32emu models each.)
//

#ifndef HV_DSP_VOICES
HV_INLINE int32_t dsp_clip16(int32_t v) {
	int32_t r;
	asm("sclip32 %0, %1, 15" : "=r"(r) : "r"(v));
	return r;
}
HV_INLINE uint32_t dsp_pkbb(uint32_t hi, uint32_t lo) {
	uint32_t r;
	asm("pkbb16 %0, %1, %2" : "=r"(r) : "r"(hi), "r"(lo));
	return r;
}
HV_INLINE int32_t dsp_kmada(int32_t acc, uint32_t a, uint32_t b) {
	asm("kmada %0, %1, %2" : "+r"(acc) : "r"(a), "r"(b));
	return acc;
}
HV_INLINE int32_t dsp_scale(int32_t a, int32_t s) {
	int32_t r;
	asm("kmmwb2.u %0, %1, %2" : "=r"(r) : "r"(a), "r"(s));
	return r;
}
HV_INLINE uint32_t half_pair(int32_t hi, int32_t lo) { return uint32_t(hi) << 16 | (uint32_t(lo) & 0xFFFF); }

// The chip's dry sums (Q12), rounded, clipped, to the dry output.
template <bool CLIP, bool ADD>
void dry_out_t(const Chip &c, int32_t *dry, uint32_t n) {
	for (uint32_t j = 0; j < n; ++j) {
		int32_t l = (c.acc_l[j] + 2048) >> 12, r = (c.acc_r[j] + 2048) >> 12;
		if (CLIP) { l = dsp_clip16(l); r = dsp_clip16(r); }
		if (ADD) { l += dry[2 * j]; r += dry[2 * j + 1]; }
		dry[2 * j] = l;
		dry[2 * j + 1] = r;
	}
}
void dry_out(const Chip &c, int32_t *dry, bool add, uint32_t n) {
	if (c.flags & NO_CLIP) add ? dry_out_t<false, true>(c, dry, n) : dry_out_t<false, false>(c, dry, n);
	else add ? dry_out_t<true, true>(c, dry, n) : dry_out_t<true, false>(c, dry, n);
}

#else   // HV_DSP_VOICES: after voice_fill

HV_INLINE bool sends(const Chip &c, const Output &o, uint32_t ch) { return (o.rev && c.rev_q[ch]) || (o.cho && c.cho_q[ch]); }
alignas(8) const int32_t ZERO_SUMS[BLOCK] = {};    // a sum not begun yet: read as 0 (no memset, no template)

// A pass's coefficients, a word a pair (w dry, v reverb, h chorus; l and r). An odd last row pairs with
// the next row of the buffer under a 0 coefficient: whatever it holds, times 0, adds 0.
template <uint32_t NP>
struct RxCoef {
	uint32_t wl[NP], wr[NP], vl[NP], vr[NP], hl[NP], hr[NP];
	HV_INLINE RxCoef(const Chip &c, const Output &o, const uint8_t *chs, uint32_t nr) {
#pragma GCC unroll 4
		for (uint32_t k = 0; k < NP; ++k) {
			const uint32_t a = chs[2 * k];
			const bool two = 2 * k + 1 < nr;
			const uint32_t b = two ? chs[2 * k + 1] : a;
			const int32_t la = c.coef_l[a], ra = c.coef_r[a], lb = two ? c.coef_l[b] : 0, rb = two ? c.coef_r[b] : 0;
			const int32_t qa = o.rev ? c.rev_q[a] : 0, qb = o.rev && two ? c.rev_q[b] : 0;
			const int32_t ga = o.cho ? c.cho_q[a] : 0, gb = o.cho && two ? c.cho_q[b] : 0;
			wl[k] = half_pair(lb, la); wr[k] = half_pair(rb, ra);
			vl[k] = half_pair(send_coef(lb, qb), send_coef(la, qa)); vr[k] = half_pair(send_coef(rb, qb), send_coef(ra, qa));
			hl[k] = half_pair(send_coef(lb, gb), send_coef(la, ga)); hr[k] = half_pair(send_coef(rb, gb), send_coef(ra, ga));
		}
	}
};
template <uint32_t NP>
HV_INLINE int32_t rx_dot(int32_t acc, const uint32_t *p, const uint32_t *w) {
#pragma GCC unroll 4
	for (uint32_t k = 0; k < NP; ++k) acc = dsp_kmada(acc, p[k], w[k]);
	return acc;
}
// Where a pass's dry sums come from (the chip's, or ZERO_SUMS where they have not begun) and where its
// outputs go: the caller's buses (OUT) and dry output (DM).
struct RxIo { const int32_t *il, *ir; int32_t *rv, *cv, *dry; };
// One pass of NP pairs of rows from c.mono[base]. BUS: the buses it adds to (1 reverb, 2 chorus); BF: their
// sums begin here (set, not added to: no memset). OUT: its buses' last pass: their sums, rounded, go to the
// caller's buses. DM: the dry output made here (the block's last pass): 1 clipped, 2 clipped and added;
// 0, the chip's sums kept.
template <uint32_t NP, uint32_t BUS, bool BF, bool OUT, uint32_t DM>
void rx_pass(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t base, uint32_t n, const RxIo &io) {
	const RxCoef<NP> k(c, o, chs, nr);
	const int16_t *x = c.mono[base];
	int32_t *al = c.acc_l, *ar = c.acc_r;
	int32_t *b0 = c.bus_acc[0], *b1 = c.bus_acc[1], *b2 = c.bus_acc[2], *b3 = c.bus_acc[3];
	for (uint32_t j = 0; j < n; ++j) {
		uint32_t p[NP];
#pragma GCC unroll 4
		for (uint32_t q = 0; q < NP; ++q) p[q] = dsp_pkbb(uint32_t(x[(2 * q + 1) * BLOCK + j]), uint32_t(x[2 * q * BLOCK + j]));
		const int32_t l = rx_dot<NP>(io.il[j], p, k.wl), r = rx_dot<NP>(io.ir[j], p, k.wr);
		if (DM == 0) { al[j] = l; ar[j] = r; }
		else {
			int32_t dl = dsp_clip16((l + 2048) >> 12), dr = dsp_clip16((r + 2048) >> 12);
			if (DM == 2) { dl += io.dry[2 * j]; dr += io.dry[2 * j + 1]; }
			io.dry[2 * j] = dl; io.dry[2 * j + 1] = dr;
		}
		if (BUS & 1) {
			const int32_t u = rx_dot<NP>(BF ? 0 : b0[j], p, k.vl), v = rx_dot<NP>(BF ? 0 : b1[j], p, k.vr);
			if (OUT) { io.rv[2 * j] += (u + 8192) >> 14; io.rv[2 * j + 1] += (v + 8192) >> 14; } else { b0[j] = u; b1[j] = v; }
		}
		if (BUS & 2) {
			const int32_t u = rx_dot<NP>(BF ? 0 : b2[j], p, k.hl), v = rx_dot<NP>(BF ? 0 : b3[j], p, k.hr);
			if (OUT) { io.cv[2 * j] += (u + 8192) >> 14; io.cv[2 * j + 1] += (v + 8192) >> 14; } else { b2[j] = u; b3[j] = v; }
		}
	}
}
// A pass's template, by what it makes: dm 0 (the chip's sums kept), 1 or 2 (rx_pass's DM).
template <uint32_t NP, uint32_t BUS, bool BF, bool OUT>
HV_INLINE void rx_dm(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t base, uint32_t n, const RxIo &io, uint32_t dm) {
	if (dm == 1) rx_pass<NP, BUS, BF, OUT, 1>(c, o, chs, nr, base, n, io);
	else if (dm == 2) rx_pass<NP, BUS, BF, OUT, 2>(c, o, chs, nr, base, n, io);
	else rx_pass<NP, BUS, BF, OUT, 0>(c, o, chs, nr, base, n, io);
}
template <uint32_t NP, uint32_t BUS>
HV_INLINE void rx_bus(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t n, const RxIo &io, bool bf, bool out, uint32_t dm) {
	if (out) bf ? rx_dm<NP, BUS, true, true>(c, o, chs, nr, 8, n, io, dm) : rx_dm<NP, BUS, false, true>(c, o, chs, nr, 8, n, io, dm);
	else bf ? rx_pass<NP, BUS, true, false, 0>(c, o, chs, nr, 8, n, io) : rx_pass<NP, BUS, false, false, 0>(c, o, chs, nr, 8, n, io);
}
// The block's state between passes: any (the chip's dry sums begun), bus (the buses with sums kept).
struct RxBlock { bool any; uint32_t bus; uint32_t off; };
// The voices that do not send: nr rows from c.mono[0]; dm: the dry output made here (0: not the last pass).
template <uint32_t NP>
void rx_dry(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t n, RxBlock &b, uint32_t dm) {
	const RxIo io = {b.any ? c.acc_l : ZERO_SUMS, b.any ? c.acc_r : ZERO_SUMS, nullptr, nullptr, o.dry + 2 * b.off};
	b.any = true;
	rx_dm<NP, 0, false, false>(c, o, chs, nr, 0, n, io, dm);
}
// The voices that send: nr rows from c.mono[8]; out: the buses' last pass; dm: and the dry output's.
template <uint32_t NP>
void rx_send(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t n, RxBlock &b, bool out, uint32_t dm) {
	uint32_t want = 0;
	for (uint32_t k = 0; k < nr; ++k) want |= (o.rev && c.rev_q[chs[k]] ? 1u : 0u) | (o.cho && c.cho_q[chs[k]] ? 2u : 0u);
	const uint32_t fresh = want & ~b.bus;
	const bool bf = fresh == want;   // every bus of the pass begins here; else the few that do are zeroed
	if (!bf) for (uint32_t k = 0; k < 2; ++k) if (fresh >> k & 1) { memset(c.bus_acc[2 * k], 0, n * 4); memset(c.bus_acc[2 * k + 1], 0, n * 4); }
	const RxIo io = {b.any ? c.acc_l : ZERO_SUMS, b.any ? c.acc_r : ZERO_SUMS,
	                 o.rev ? o.rev + 2 * b.off : nullptr, o.cho ? o.cho + 2 * b.off : nullptr, o.dry + 2 * b.off};
	b.any = true;
	b.bus = out ? b.bus & ~want : b.bus | want;   // given out here, or kept
	if (want == 1) rx_bus<NP, 1>(c, o, chs, nr, n, io, bf, out, dm);
	else if (want == 2) rx_bus<NP, 2>(c, o, chs, nr, n, io, bf, out, dm);
	else if (want == 3) rx_bus<NP, 3>(c, o, chs, nr, n, io, bf, out, dm);
	else rx_dm<NP, 0, false, false>(c, o, chs, nr, 8, n, io, dm);   // sends to buses not given
}
// The block's only row (one voice, not the rhythm pair): each output is that row's one product, rounded,
// so one kmmwb2.u an output gives the definition's bits. BUS: its buses (1 reverb, 2 chorus); DM 1 or 2.
template <uint32_t BUS, uint32_t DM>
void rx_one(Chip &c, const Output &o, uint32_t ch, const int16_t *x, uint32_t off, uint32_t n) {
	const int32_t cl = c.coef_l[ch], cr = c.coef_r[ch], wl = cl << 3, wr = cr << 3;
	const int32_t q = o.rev ? c.rev_q[ch] : 0, g = o.cho ? c.cho_q[ch] : 0;
	const int32_t vl = send_coef(cl, q) << 1, vr = send_coef(cr, q) << 1, hl = send_coef(cl, g) << 1, hr = send_coef(cr, g) << 1;
	int32_t *dry = o.dry + 2 * off, *rv = o.rev ? o.rev + 2 * off : nullptr, *cv = o.cho ? o.cho + 2 * off : nullptr;
	for (uint32_t j = 0; j < n; ++j) {
		const int32_t s = x[j];
		int32_t l = dsp_clip16(dsp_scale(wl, s)), r = dsp_clip16(dsp_scale(wr, s));
		if (DM == 2) { l += dry[2 * j]; r += dry[2 * j + 1]; }
		dry[2 * j] = l; dry[2 * j + 1] = r;
		if (BUS & 1) { rv[2 * j] += dsp_scale(vl, s); rv[2 * j + 1] += dsp_scale(vr, s); }
		if (BUS & 2) { cv[2 * j] += dsp_scale(hl, s); cv[2 * j + 1] += dsp_scale(hr, s); }
	}
}
HV_INLINE void rx_one_n(Chip &c, const Output &o, uint32_t ch, const int16_t *x, uint32_t off, uint32_t n, uint32_t dm) {
	const uint32_t bus = (o.rev && c.rev_q[ch] ? 1u : 0u) | (o.cho && c.cho_q[ch] ? 2u : 0u);
	switch (bus * 2 + (dm - 1)) {
	case 0: rx_one<0, 1>(c, o, ch, x, off, n); break;
	case 1: rx_one<0, 2>(c, o, ch, x, off, n); break;
	case 2: rx_one<1, 1>(c, o, ch, x, off, n); break;
	case 3: rx_one<1, 2>(c, o, ch, x, off, n); break;
	case 4: rx_one<2, 1>(c, o, ch, x, off, n); break;
	case 5: rx_one<2, 2>(c, o, ch, x, off, n); break;
	case 6: rx_one<3, 1>(c, o, ch, x, off, n); break;
	default: rx_one<3, 2>(c, o, ch, x, off, n); break;
	}
}
HV_INLINE void rx_dry_n(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t n, RxBlock &b, uint32_t dm) {
	switch ((nr + 1) / 2) {
	case 1: rx_dry<1>(c, o, chs, nr, n, b, dm); break;
	case 2: rx_dry<2>(c, o, chs, nr, n, b, dm); break;
	case 3: rx_dry<3>(c, o, chs, nr, n, b, dm); break;
	default: rx_dry<4>(c, o, chs, nr, n, b, dm); break;
	}
}
HV_INLINE void rx_send_n(Chip &c, const Output &o, const uint8_t *chs, uint32_t nr, uint32_t n, RxBlock &b, bool out, uint32_t dm) {
	nr > 2 ? rx_send<2>(c, o, chs, nr, n, b, out, dm) : rx_send<1>(c, o, chs, nr, n, b, out, dm);
}

// The block's voices: eight rows of voices that do not send, four of voices that do, each group mixed as
// it fills. The last group of senders gives out the buses' sums, and makes the dry output too (clipped)
// where no voice that does not send is left; else the last group of the others makes it. Returns 0 (nothing
// sounded), 1 (the chip's sums, Q12, for dry_out) or 2 (the dry output made); bus: the buses whose sums are
// still to be given out (bus_out).
uint32_t dsp_voices(Chip &c, const Output &o, uint32_t mask, uint32_t off, uint32_t n, uint32_t &bus) {
	uint8_t dch[8], sch[4];
	uint32_t dw = 0, sw = 0;
	RxBlock b = {false, 0, off};
	for (uint32_t m = mask; m; m &= m - 1) {
		const uint32_t ch = lowest_bit(m);
		const bool rhy = c.kind[ch] == K_RHY;
		const uint32_t need = rhy ? 2 : 1;
		uint8_t *chs;
		uint32_t *w, row;
		if (sends(c, o, ch) || (rhy && sends(c, o, 8))) {
			if (sw + need > 4) { rx_send_n(c, o, sch, sw, n, b, false, 0); sw = 0; }
			chs = sch; w = &sw; row = 8 + sw;
		} else {
			if (dw + need > 8) { rx_dry_n(c, o, dch, dw, n, b, 0); dw = 0; }
			chs = dch; w = &dw; row = dw;
		}
		if (!voice_fill(c, ch, c.mono[row], c.mono[rhy ? row + 1 : row], n, 0, false)) continue;
		if (rhy) { chs[*w] = 7; chs[*w + 1] = 8; *w += 2; }
		else chs[(*w)++] = uint8_t(ch);
	}
	const uint32_t dm = (c.flags & NO_CLIP) ? 0 : o.add_dry ? 2 : 1;   // unclipped: the sums to dry_out
	if (dm && !b.any && sw + dw == 1) {   // the block's only row
		if (sw) rx_one_n(c, o, sch[0], c.mono[8], b.off, n, dm); else rx_one_n(c, o, dch[0], c.mono[0], b.off, n, dm);
		return 2;
	}
	if (sw) rx_send_n(c, o, sch, sw, n, b, true, dw ? 0 : dm);
	if (dw) rx_dry_n(c, o, dch, dw, n, b, dm);
	const uint32_t made = dm && (sw || dw) ? 2 : 0;
	bus = b.bus;
	return made ? made : b.any ? 1 : 0;
}
#endif
