// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3_drv.hpp -- a MIDI driver for 1 to 32 hvopl3 chips: MIDI 1.0 messages in, voices allocated,
// instruments mapped from banks, controllers turned into register writes and the chip's mixer
// extensions. Freestanding, as the chip: no allocation, no writable globals, nothing from the C library
// but memset and memcpy. The caller owns every struct: the Driver, its bank Pool and the Chips.
// docs/DRIVER.md describes the API and every policy.
//
#pragma once
#include "hvopl3.hpp"

#define HVOPL3_DRV_VERSION_MAJOR 0
#define HVOPL3_DRV_VERSION_MINOR 5
#define HVOPL3_DRV_VERSION_PATCH 0
#define HVOPL3_DRV_VERSION_NUMBER (HVOPL3_DRV_VERSION_MAJOR * 10000 + HVOPL3_DRV_VERSION_MINOR * 100 + HVOPL3_DRV_VERSION_PATCH)

// The most chips a Driver drives: a build option, 1..32 (default 32). Every array sized by it follows it
// (on the A25 the Driver is 7.7 KB at 4, 20.5 KB at 16, 37.7 KB at 32: about 1.1 KB a chip).
#ifndef HVOPL3_DRV_MAX_CHIPS
#define HVOPL3_DRV_MAX_CHIPS 32
#endif
static_assert(HVOPL3_DRV_MAX_CHIPS >= 1 && HVOPL3_DRV_MAX_CHIPS <= 32, "HVOPL3_DRV_MAX_CHIPS: 1..32");

namespace hvopl3 {
namespace drv {

enum : uint32_t {
	MAX_CHIPS = HVOPL3_DRV_MAX_CHIPS,
	MAX_SLOTS = CHANNELS * MAX_CHIPS,   // chip channels: 18 a chip, at most 576 (a slot index is 16 bits)
	MIDI_CHANNELS = 16,                 // one port (the arrays are indexed by port x 16 + channel)
	QUEUE = 256,                        // messages held for one block
	PERIOD_48K = 64,                    // the control period at 48 kHz (128 frames at 96 kHz): 1.33 ms
	NONE = 0xFFFF,
};

// ---- patches and banks ------------------------------------------------------------------------

enum Kind : uint8_t { KIND_BLANK = 0, KIND_2OP = 1, KIND_4OP = 2, KIND_PSEUDO = 3 };
enum PatchFlag : uint8_t { PATCH_FIXED = 1 };   // a melodic patch that plays at its own key

// One operator's registers, as the chip has them.
struct Operator { uint8_t r20, r40, r60, r80, rE0; };

// An instrument. Operators in the chip's order: [0] and [1] the first channel's modulator and carrier,
// [2] and [3] the second channel's (a 4-op voice: operators 1 to 4 of the chain; pseudo-4-op: the second
// voice). c0: the C0 register's feedback and connection (bits 3..0) for each channel.
struct Patch {
	Operator op[4];
	uint8_t c0[2];
	uint8_t kind;            // Kind
	uint8_t key;             // percussion key: the note a drum (or a PATCH_FIXED patch) plays at
	int16_t note_offset[2];  // semitones, each voice
	int8_t velocity_offset;
	int8_t detune;           // the second voice's (pseudo-4-op), WOPL's units: about 1/64 semitone
	int8_t gain;             // level correction, 0.75 dB steps, + louder (set_levels)
	uint8_t drum;            // the WOPL rhythm-mode drum type, kept, not used
	uint8_t flags;           // PatchFlag
	uint8_t pad;
};

struct Bank {
	uint8_t percussion, msb, lsb, pad;
	Patch patch[128];        // by program (melodic) or by key (percussion)
};

// The bank pool: a header, then the banks and a hash index, all in one block of caller memory.
struct Pool {
	Bank *banks;
	uint16_t *index;         // open addressing: bank number + 1, 0 empty
	uint32_t capacity, mask, count;
	uint8_t deep;            // the last WOPL's bank-wide flags: bit 0 deep tremolo, bit 1 deep vibrato
	uint8_t volume_model;    // the last WOPL's volume model, as read
	uint8_t curve;           // ... mapped to a Curve (CURVE_AUTO: the driver's default)
	uint8_t pad;
};

size_t pool_bytes(uint32_t banks);                 // the memory a pool of this many banks takes
Pool *pool_init(void *mem, size_t bytes);           // mem 8-aligned; nullptr if too small for one bank
void pool_clear(Pool &);
bool set_patch(Pool &, bool percussion, uint8_t msb, uint8_t lsb, uint8_t program, const Patch &);
const Patch *find_patch(const Pool &, bool percussion, uint8_t msb, uint8_t lsb, uint8_t program);  // nullptr: no bank
const Bank *find_bank(const Pool &, bool percussion, uint8_t msb, uint8_t lsb);

enum Load : int { LOAD_OK = 0, LOAD_FORMAT = -1, LOAD_VERSION = -2, LOAD_TRUNCATED = -3, LOAD_FULL = -4 };
// A WOPL bank file (versions 1 to 3) into the pool: all of it or, on an error, nothing.
int load_wopl(Pool &, const void *data, size_t size);

// Level corrections (0.75 dB steps, + louder) into the patches' gain fields: `programs` for every
// melodic bank's program, `kits` per key for the percussion banks (by LSB; a kit with no table takes
// kit 0's, or none). Either may be null. Re-apply after loading more banks.
struct KitLevels { uint8_t kit; int8_t levels[128]; };
void set_levels(Pool &, const int8_t *programs, const KitLevels *kits, uint32_t nkits);

// ---- the driver ---------------------------------------------------------------------------------

enum Curve : uint8_t {
	CURVE_AUTO = 0,     // the bank's (its WOPL volume model, mapped), else hirav
	CURVE_HIRAV = 1,    // General MIDI's 40 log10(127 / x) for each level, summed in dB
	CURVE_LINEAR = 2,   // the four levels' product, linear in the chip's total-level steps (the driver's own straight line)
	CURVE_TABLE = 3,    // the caller's tables (set_curve_tables)
	// The sound drivers' own volume models (docs/DRIVER.md section 8), fitted to black-box measurements
	// (drv/hvopl3_drv_models.inc): each scales or adds to the operators' own levels in its own way.
	CURVE_DMX = 4, CURVE_APOGEE = 5, CURVE_9X = 6, CURVE_DMX_FIXED = 7, CURVE_APOGEE_FIXED = 8, CURVE_AIL = 9,
	CURVE_9X_GENERIC_FM = 10, CURVE_HMI = 11, CURVE_HMI_OLD = 12, CURVE_MS_ADLIB = 13, CURVE_IMF_CREATOR = 14,
	CURVE_OCONNELL = 15,
	// ... and the generic and native models of the same measurements (not CURVE_LINEAR, not hirav)
	CURVE_GENERIC = 16, CURVE_NATIVE = 17,
};

struct Config {
	uint32_t chips;          // 1..MAX_CHIPS (clamped)
	Rate rate;               // RATE_48K or RATE_96K, every chip
	uint32_t chip_flags;     // hvopl3's HQ_* and output flags, every chip
	uint8_t curve;           // Curve
	bool stream;             // emit HVO2 records (render_stream) instead of driving the chips
	bool hard_pan;           // pan by the chip's C0 left/right bits, not set_pan
	bool level_corrections;  // the patches' gains (with the hirav curve)
	bool restrike;           // a Note On for a sounding note re-strikes its voice
	bool gain_remainder;     // attenuation past total level 63 carried by set_gain
	bool pressure_vibrato;   // channel pressure adds to the vibrato depth
	int8_t deep_tremolo;     // -1 the bank's flag, 0 off, 1 on
	int8_t deep_vibrato;
	bool returning;          // a Note On whose note's voice (same channel, note, patch) is still releasing
	                         // keys that voice again, its envelope going on from where it is
};
Config default_config();     // 1 chip, 48 kHz, CURVE_AUTO, soft pan, corrections, restrike and returning on

struct Event { uint32_t offset; uint8_t status, a, b, c; };

// A chip channel: one 2-op voice, half a pseudo-4-op voice, or one channel of a 4-op pair.
struct Slot {
	uint16_t cprev, cnext;   // the MIDI channel's voices
	uint16_t gprev, gnext;   // the held or the released list
	uint16_t partner;        // pseudo: the other half; 4-op: the pair's other channel
	uint8_t state, role, midi, note, vel, flags;
	uint8_t carriers;        // the operators heard: a bit each (4-op: four, on the pair's first channel)
	uint8_t tl[2];           // the bank's 40 register of this channel's two operators
	uint8_t rr;              // the slowest carrier's release rate
	uint8_t level;           // the attenuation applied, 0.75 dB steps
	int8_t corr;             // the patch's level correction
	uint8_t c0;              // the C0 register as written
	uint8_t pan_w, rev_w, cho_w;
	int8_t gain_w;
	uint8_t wait;            // pending: control periods waited
	uint16_t fnblk;          // the A0/B0 pitch as written
	int32_t base;            // pitch: key, offsets and detune, 1/4096 semitone
	int32_t glide, glide_step;
	uint32_t start, released;   // the control period of the note on, the release
	const Patch *patch;      // pending: what to start
	uint8_t half;            // pending: which half of the patch
	uint8_t drum;            // a percussion channel's voice: 1 + its drum map (GS's per-drum settings); 0 not
	uint8_t pad[2];
};

struct Channel {
	uint16_t head, tail;     // its voices
	uint8_t program, msb, lsb, msb_sel, lsb_sel;
	uint8_t volume, expression, pan, reverb, chorus, modulation, pressure;
	uint8_t porta_time, porta_on, porta_from, last_note;
	uint8_t sustain, sostenuto, soft;
	uint8_t rpn_msb, rpn_lsb, nrpn;
	uint8_t nrpn_msb, nrpn_lsb;   // the NRPN selected (127/127 none): GS's per-drum ones taken
	uint8_t drum_map;        // GS's rhythm part map: 0 map 1, 1 map 2
	uint8_t bend_semis, bend_cents;
	int8_t coarse;
	uint8_t percussion, xg_kit, host_perc;   // the flag; MSB 126/127 selected here; the host's (0 none, 1 off, 2 on)
	uint16_t bend, fine;
	uint16_t gliders;        // voices gliding
	uint32_t vib_phase;
	int32_t offset;          // bend, tuning and master tuning, 1/4096 semitone
};

struct Driver {
	Pool *pool;
	Chip *chips;
	Config cfg;
	uint32_t period, nslots, now, tick, next_tick;
	// a block in progress
	uint32_t blk, pos, frames;
	uint32_t done[MAX_CHIPS];
	Output out;
	uint32_t *rec;
	uint32_t rec_cap, rec_n, sounded;
	uint8_t pending;         // init, reset, panic, at the next block's start
	uint8_t xg, master_volume;
	int8_t master_coarse;
	uint16_t master_fine;
	uint16_t dirty;          // MIDI channels whose pitch changed
	uint16_t held_head, held_tail, rel_head, rel_tail;
	uint32_t free_mask[MAX_CHIPS], rel_mask[MAX_CHIPS], pend_mask[MAX_CHIPS];
	uint8_t four[MAX_CHIPS];      // each chip's 0x104
	uint8_t busy[MAX_CHIPS];      // its channels not free
	uint16_t vel_table[128], vol_table[128];
	// GS's per-drum settings (NRPN 18h, 1Ah, 1Ch, 1Dh, 1Eh with the key as LSB), by drum map and key
	struct Drum { int8_t pitch; uint8_t level, pan, reverb, chorus; } drums[2][128];
	uint32_t qn;
	Channel ch[MIDI_CHANNELS];
	Slot slot[MAX_SLOTS];
	Event queue[QUEUE];
};

// Set up: the chips are initialised at the first render. `chips`: cfg.chips of them, caller-owned (in
// stream mode, the driver's own shadows of the remote chips: they make no samples).
void init(Driver &, Pool &, Chip *chips, const Config &);
// A new chip count, rate or any other setting: a full reset at the next render; the banks are kept.
void configure(Driver &, Chip *chips, const Config &);
// New settings without a reset, where none is needed: the voices sound on, and every channel keeps its
// program, controllers and notes. curve, level_corrections and gain_remainder: every sounding voice's
// level again, at the next render's start, and every note after; restrike and pressure_vibrato: at once;
// deep_tremolo and deep_vibrato: register BD, at the next render's start; in direct mode the chips'
// HQ_* and NO_CLIP flags: hvopl3::set_flags on every chip, at the next render's start. Any other change
// (chips, rate, stream, hard_pan, other chip flags, the chips themselves, or the chips' flags in stream
// mode, which has no record for them) is configure's: a full reset.
void update(Driver &, Chip *chips, const Config &);
// Tables for CURVE_TABLE: attenuation in 1/16 of a 0.75 dB step for each value 0..127; 0xFFFF silent.
// `vol` serves channel volume, expression and master volume alike.
void set_curve_tables(Driver &, const uint16_t *vel, const uint16_t *vol);
// A channel's percussion flag from the host: 1 on, 0 off, -1 the host's setting forgotten (MIDI decides).
void set_percussion(Driver &, uint32_t channel, int on);

// A complete MIDI message (status and data bytes, or SysEx F0 .. F7) at a frame of the next block.
// False if the queue is full: render the block, then send it again. Malformed or unknown messages are
// consumed (true).
bool send(Driver &, uint32_t offset, const uint8_t *msg, uint32_t len);
bool send3(Driver &, uint32_t offset, uint8_t status, uint8_t a, uint8_t b = 0);

// Direct mode: the block, every chip summed into `out` (dry written, buses added to, as hvopl3::render).
// Returns SOUNDED if any chip made sound.
uint32_t render(Driver &, const Output &out, uint32_t frames);
// Stream mode: the block's HVO2 records (docs/TRACE-FORMAT.md) into `records`; replayed with hvopl3::play
// they make render's samples. Returns the frames done: fewer than asked when the records would not fit
// (send the records, then call again for the rest). `capacity` must be at least stream_reserve().
uint32_t render_stream(Driver &, uint32_t *records, uint32_t capacity, uint32_t &count, uint32_t frames);
uint32_t stream_reserve(const Driver &);
// hvopl3::play's flags, to replay a stream: the chips' (the config's, and NO_PAN_LAW with hard pan) and PLAY_HVO2.
uint32_t chip_flags(const Driver &);

// The curve's attenuation for these four levels, in 0.75 dB steps (255: silent), as applied to a voice.
uint32_t attenuation(const Driver &, uint8_t vel, uint8_t vol, uint8_t expr, uint8_t master);

void reset(Driver &);   // at the next block: every note off, every controller its default
void panic(Driver &);   // at the next block: every voice cut, the queue dropped

uint32_t version();     // HVOPL3_DRV_VERSION_NUMBER of the build

}  // namespace drv
}  // namespace hvopl3

// The C ABI: the same calls on caller-allocated memory.
extern "C" {
uint32_t hvopl3_drv_version(void);
size_t hvopl3_drv_size(void);
size_t hvopl3_drv_config_size(void);
void hvopl3_drv_default_config(void *config);
size_t hvopl3_drv_pool_bytes(uint32_t banks);
void *hvopl3_drv_pool_init(void *mem, size_t bytes);
int hvopl3_drv_load_wopl(void *pool, const void *data, size_t size);
void hvopl3_drv_set_levels(void *pool, const int8_t *programs, const void *kits, uint32_t nkits);
void hvopl3_drv_init(void *drv, void *pool, void *chips, const void *config);
void hvopl3_drv_configure(void *drv, void *chips, const void *config);
void hvopl3_drv_update(void *drv, void *chips, const void *config);
void hvopl3_drv_set_percussion(void *drv, uint32_t channel, int on);
int hvopl3_drv_send(void *drv, uint32_t offset, const uint8_t *msg, uint32_t len);
uint32_t hvopl3_drv_render(void *drv, int32_t *dry, int32_t *rev, int32_t *cho, int add_dry, uint32_t frames);
uint32_t hvopl3_drv_render_stream(void *drv, uint32_t *records, uint32_t capacity, uint32_t *count, uint32_t frames);
void hvopl3_drv_reset(void *drv);
void hvopl3_drv_panic(void *drv);
}
