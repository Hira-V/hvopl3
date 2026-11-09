// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3.hpp -- a YMF262 (OPL3) synthesizer at 48 or 96 kHz, with soft pan, send buses and voice
// gain. Freestanding: no libc beyond memset, no allocation, no writable globals. The caller owns each Chip.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

// The version, semantic (major.minor.patch). HVOPL3_VERSION_NUMBER orders as an integer
// (major * 10000 + minor * 100 + patch); version() returns the compiled library's, to compare.
#define HVOPL3_VERSION_MAJOR 0
#define HVOPL3_VERSION_MINOR 12
#define HVOPL3_VERSION_PATCH 0
#define HVOPL3_VERSION_NUMBER (HVOPL3_VERSION_MAJOR * 10000 + HVOPL3_VERSION_MINOR * 100 + HVOPL3_VERSION_PATCH)
#define HVOPL3_STR_(x) #x
#define HVOPL3_XSTR_(x) HVOPL3_STR_(x)
#define HVOPL3_VERSION HVOPL3_XSTR_(HVOPL3_VERSION_MAJOR) "." HVOPL3_XSTR_(HVOPL3_VERSION_MINOR) "." HVOPL3_XSTR_(HVOPL3_VERSION_PATCH)

namespace hvopl3 {

enum Rate : uint32_t { RATE_48K = 48000, RATE_96K = 96000 };  // 96 kHz: to be halved to 48 kHz

enum Flag : uint32_t {
	HQ_PHASE = 1,        // high quality, each its own: the wave at the whole phase, not its table index
	HQ_ENVELOPE = 2,     // the envelope's gain continuous, not in steps
	HQ_MODULATION = 4,   // modulation and feedback unrounded
	NO_PAN_LAW = 8,   // the chip's own C0 bits alone, at unity
	NO_CLIP = 16,     // the dry output is not clipped to 16 bits
};

enum : uint32_t { CHANNELS = 18, OPERATORS = 36, BLOCK = 64 };

// Where a render goes: stereo interleaved. Dry is written, or added to if add_dry; the buses, when
// given, are added to. No buses, no cost for them.
struct Output {
	int32_t *dry;
	int32_t *rev;
	int32_t *cho;
	bool add_dry;
};

enum : uint32_t { SOUNDED = 1 };  // render's result: a voice made sound; if not, nothing was added

// One chip's whole state, flat: structure of arrays (docs/DESIGN.md).
struct Chip {
	// time: `now` is the next sample; tick + tfrac / 2^32 the chip's position there
	uint32_t now, tick, tfrac;
	uint32_t r_int, r_frac;   // ticks a sample
	uint32_t rinv_q16;        // samples a tick
	uint32_t inc_k;           // phase scale: ticks a sample, Q31
	uint32_t flags;
	uint32_t active;          // channels with any operator not fully off
	uint32_t am_chans, vib_chans;
	uint32_t lfo_next;
	uint32_t noise, rhy_at;
	uint16_t am;              // the tremolo's attenuation now
	int8_t pm;
	uint8_t r_08, r_bd, r_104, r_105, fb_step;
	uint32_t key_dirty[2];    // operators whose key may have changed since the last sample

	// operators
	uint32_t ph[OPERATORS], inc[OPERATORS], next_tick[OPERATORS];
	uint16_t att[OPERATORS];  // what the kernels add: (envelope + level + tremolo) in log-sine units
	uint16_t env[OPERATORS], base[OPERATORS], sl[OPERATORS];
	uint8_t state[OPERATORS], rate[OPERATORS][4], wave[OPERATORS], mult2[OPERATORS];
	uint8_t r_20[OPERATORS], r_40[OPERATORS], r_60[OPERATORS], r_80[OPERATORS], key[OPERATORS];
	uint8_t keyed[OPERATORS], pending[OPERATORS];

	// channels
	uint32_t next_event[CHANNELS];
	uint32_t idle_at[CHANNELS];   // an idle voice: the sample its phases are good to
	int32_t coef_l[CHANNELS], coef_r[CHANNELS];
	int16_t fb0[CHANNELS], fb1[CHANNELS], fb2[CHANNELS], fb3[CHANNELS];
	uint16_t fnblk[CHANNELS];
	uint8_t r_c0[CHANNELS], kon[CHANNELS], kind[CHANNELS], fb_shift[CHANNELS];
	uint16_t rev_q[CHANNELS], cho_q[CHANNELS];
	uint8_t pan[CHANNELS];
	int8_t gain[CHANNELS];

	// the mix (each output the voices' exact sum, rounded once: hvopl3.cpp): its sums, Q12 (dry l, r) and
	// Q14 (reverb l, r; chorus l, r), and the mono matrix (one row, two for the rhythm pair)
	int32_t acc_l[BLOCK], acc_r[BLOCK];
	int32_t bus_acc[4][BLOCK];
	int32_t mix_hi[6][BLOCK];        // past the fast path's bound: the sums of the coefficients' high parts
	uint8_t mix_bound;               // the fast path's bound over every channel: 0 not known, 1 holds, 2 not
#if defined(__riscv_dsp) && !defined(__riscv_vector) && !defined(__riscv_xtheadvector) && !defined(HVOPL_NO_DSP)
	alignas(4) int16_t mono[12][BLOCK];   // the Andes DSP mix (the A25): eight rows of voices that do not send, four that do
#else
	int16_t mono[2][BLOCK];
#endif
#if defined(__riscv_vector) || defined(__riscv_xtheadvector)
	int16_t rows[CHANNELS][BLOCK];   // the lanes kernel's matrix: voices x frames
	// its lanes, loaded and stored around each chunk (here, not on the stack: a 2 KiB stack budget)
	uint32_t l_pa[CHANNELS], l_pb[CHANNELS], l_ia[CHANNELS], l_ib[CHANNELS];
	uint16_t l_aa[CHANNELS], l_ab[CHANNELS], l_wa[CHANNELS], l_wb[CHANNELS], l_sh[CHANNELS];
	int16_t l_y1[CHANNELS], l_y2[CHANNELS], l_y3[CHANNELS], l_y4[CHANNELS];
#endif
#if defined(__riscv_vector) && (defined(__riscv_zve32f) || defined(__riscv_xtheadvector))
	// high quality in lanes across voices (RVV with float): [op of the voice][lane], then [lane]
	float h_from[4][CHANNELS], h_slope[4][CHANNELS], h_d[4][CHANNELS];
	uint32_t h_ph[4][CHANNELS], h_inc[4][CHANNELS], h_ws[4][CHANNELS];
	float h_f[4][CHANNELS], h_fbk[CHANNELS];
	uint32_t h_fbs[CHANNELS], h_bits[CHANNELS], h_row[CHANNELS];
	float h_v[2][16][CHANNELS], h_sum[16][CHANNELS];   // a sub-block: an operator's outputs, the voices' sums
	// a lane is kept from one block to the next while nothing but the lanes changed its voice: h_ok (a bit a
	// channel) is cleared by register writes, keys, flags and LFO steps; h_slot and h_ch pair a channel and its lane
	uint32_t h_ok, h_shp[CHANNELS];
	uint8_t h_slot[CHANNELS], h_ch[CHANNELS];
#endif
	float hq_gain[OPERATORS], hq_slope[OPERATORS];  // HQ_ENVELOPE: each gain's line, from sample hq_t0
	uint32_t hq_t0[OPERATORS], hq_tick[OPERATORS];  // ... and what it was drawn for
	uint16_t hq_att[OPERATORS];
	float hq_fb0[CHANNELS], hq_fb1[CHANNELS], hq_fb2[CHANNELS], hq_fb3[CHANNELS];
};

uint32_t version();          // the library's HVOPL3_VERSION_NUMBER: differs from the header's on a mismatch
const char *version_string();  // the library's HVOPL3_VERSION, e.g. "0.12.0"

void init(Chip &, Rate, uint32_t flags = 0);
void write(Chip &, uint32_t reg, uint8_t value);   // 0x000..0x1FF, in force at the next sample
void set_pan(Chip &, uint32_t channel, uint8_t pan);                      // 0..127, 64 centre
void set_sends(Chip &, uint32_t channel, uint8_t reverb, uint8_t chorus); // 0..127
void set_gain(Chip &, uint32_t channel, int8_t level);                    // 0.75 dB steps, -32..32
void set_flags(Chip &, uint32_t flags);
uint32_t render(Chip &, const Output &, uint32_t frames);
// One command stream for every target: HVO2 records (register writes, extensions, resets, runs; the
// format is docs/TRACE-FORMAT.md's) applied in order to up to 32 chips, each chip adding to the Output
// from its own timeline's position. The caller clears dry and the buses. `flags` are the chips' (RESET
// records).
uint32_t play(Chip *chips, uint32_t nchips, const uint32_t *records, size_t count, const Output &, Rate, uint32_t flags = 0);
// Time, keys and envelopes for n frames without making samples: afterwards `active` and the envelopes
// are as render(n) leaves them. For a shadow of a chip that renders elsewhere (the MIDI driver's stream).
void advance(Chip &, uint32_t frames);
// The voice on channel `channel` (a 4-op pair: its first channel) can make no more sound until it is
// keyed again or its registers change: every operator it is heard through (its connection's carriers)
// is keyed off, in its release, and at least as attenuated as the renderer's silence (72 dB: there a
// full-scale operator's output is below one unit, and render() skips the voice whole). Its output is
// exactly zero from here on, tremolo or not, though `active` may still say it sounds (that waits for
// the envelope's end, 96 dB). Rhythm-mode voices: false.
bool inaudible(const Chip &, uint32_t channel);

}  // namespace hvopl3

extern "C" {
uint32_t hvopl3_version(void);
const char *hvopl3_version_string(void);
size_t hvopl3_chip_size(void);
void hvopl3_init(void *chip, uint32_t rate, uint32_t flags);
void hvopl3_write(void *chip, uint32_t reg, uint8_t value);
void hvopl3_set_pan(void *chip, uint32_t channel, uint8_t pan);
void hvopl3_set_sends(void *chip, uint32_t channel, uint8_t reverb, uint8_t chorus);
void hvopl3_set_gain(void *chip, uint32_t channel, int8_t level);
void hvopl3_set_flags(void *chip, uint32_t flags);
uint32_t hvopl3_render(void *chip, int32_t *dry, int32_t *rev, int32_t *cho, int add_dry, uint32_t frames);
int hvopl3_inaudible(const void *chip, uint32_t channel);
}
