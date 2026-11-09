// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Copied from Hira-V (https://hira-v.com), software/lib/midi.hpp; its references to
// docs/ and lib/ are to Hira-V's tree (README.md, "License").
#pragma once
// lib/midi.hpp -- MIDI on Hira-V: the instrument interface, and Universal MIDI Packets
// (docs/MIDI.md). Header only: an instrument is a library of its own (lib/opl3.hpp's,
// for one) that implements midi::Instrument; a program that plays one includes this and
// that library's header.
//
// PLAYING AN INSTRUMENT, a block at a time:
//
//   midi::Instrument *in = opl3::create(fmt, cfg);
//   u32 w[2];
//   in->event(0, w, midi::note_on(w, 0, 0, 60, 100));    // group 0, channel 0, middle C
//   in->render(out, 800);                                 // a display frame's samples
//
// event() takes one message for the block about to be rendered, at a sample OFFSET
// within it; render() makes the block and starts each message exactly on its sample. A
// message's offset past the block is held for the next one, less the block's length.
//
// THE CONTRACT (docs/Dynamic-libraries.md section 3): an instrument keeps no state of
// its own; it is all in the object, which the program owns (the library's create, or its
// size and init) and deletes. It is called through its vtable. The plain structs here
// are passed by their layout: a change to one is a new version of every instrument.
#include <stddef.h>
#include <stdint.h>

namespace midi {

// The audio an instrument makes: interleaved, `channels` a frame (2 today), at `rate`
// (48000 today: AudioSvc's, docs/Audio-Service.md).
struct Format {
	uint32_t rate;
	uint32_t channels;
};

enum Protocol : uint32_t {
	MIDI1 = 1,                    // MIDI 1.0 messages (UMP type 2, and SysEx as type 3)
	MIDI2 = 2,                    // MIDI 2.0 channel voice messages (UMP type 4) as well
};

struct InstrumentInfo {
	const char *name;             // "OPL3"
	uint32_t    groups;           // UMP groups it takes (each 16 channels), from group 0
	Protocol    protocol;
};

struct ParamInfo {
	const char *name;
	float       min, max, def;
};

class Instrument {
public:
	virtual ~Instrument() = default;

	// One UMP message (1 to 4 words), at a sample offset in the block about to be
	// rendered. A message the instrument does not take is ignored. False: no room to
	// hold it (render the block, then send it again).
	virtual bool event(uint32_t offset, const uint32_t *ump, uint32_t words) = 0;
	// The block: frames x channels samples, each one at a 16-bit sample's scale in a
	// 32-bit container, NOT clipped (the program mixes and clips). Overwrites `out`.
	virtual void render(int32_t *out, uint32_t frames) = 0;
	// All notes off, every controller to its default, held messages dropped.
	virtual void reset() = 0;

	virtual const InstrumentInfo &info() const = 0;

	// Parameters: a host may list, read, set and automate them.
	virtual uint32_t param_count() const = 0;
	virtual const ParamInfo &param(uint32_t i) const = 0;
	virtual float get(uint32_t i) const = 0;
	virtual void set(uint32_t i, float value) = 0;

	// A whole setup, saved and restored (banks loaded, parameters). save() returns the
	// bytes it needs; it writes them only if `max` is at least that.
	virtual uint32_t save(void *out, uint32_t max) const = 0;
	virtual bool load(const void *in, uint32_t size) = 0;

	// A bank, in one of the instrument's own formats (its header names them).
	virtual bool load_bank(const void *data, uint32_t size, uint32_t kind) = 0;
};

// ---------------------------------------------------------------------------
//  Universal MIDI Packets (the MIDI 2.0 specification's, M2-104-UM)
// ---------------------------------------------------------------------------
// A message is 1 to 4 32-bit words; its first word's top 4 bits, the message type,
// say how many. Type 2 is a MIDI 1.0 channel voice message (one word), type 3 a 7-bit
// SysEx packet (two words, up to 6 bytes of the message).

inline uint32_t ump_words(uint32_t w0)
{
	static constexpr uint8_t kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
	return kWords[w0 >> 28];
}
inline uint32_t ump_type(uint32_t w0)   { return w0 >> 28; }
inline uint32_t ump_group(uint32_t w0)  { return (w0 >> 24) & 15; }
inline uint32_t ump_status(uint32_t w0) { return (w0 >> 16) & 0xFF; }  // type 2: the MIDI 1.0 status byte
inline uint32_t ump_data1(uint32_t w0)  { return (w0 >> 8) & 0x7F; }
inline uint32_t ump_data2(uint32_t w0)  { return w0 & 0x7F; }

// A MIDI 1.0 channel voice message (status 0x80..0xEF and its data bytes) as type 2.
// Returns the word count (1), so a caller can write in->event(t, w, midi1(w, ...)).
inline uint32_t midi1(uint32_t *w, uint32_t group, uint32_t status, uint32_t d1, uint32_t d2 = 0)
{
	w[0] = 0x20000000u | (group & 15) << 24 | (status & 0xFF) << 16 | (d1 & 0x7F) << 8 | (d2 & 0x7F);
	return 1;
}
inline uint32_t note_on(uint32_t *w, uint32_t g, uint32_t ch, uint32_t note, uint32_t vel)  { return midi1(w, g, 0x90 | (ch & 15), note, vel); }
inline uint32_t note_off(uint32_t *w, uint32_t g, uint32_t ch, uint32_t note, uint32_t vel = 0) { return midi1(w, g, 0x80 | (ch & 15), note, vel); }
inline uint32_t control(uint32_t *w, uint32_t g, uint32_t ch, uint32_t cc, uint32_t value) { return midi1(w, g, 0xB0 | (ch & 15), cc, value); }
inline uint32_t program(uint32_t *w, uint32_t g, uint32_t ch, uint32_t prog)                { return midi1(w, g, 0xC0 | (ch & 15), prog); }
inline uint32_t pitch_bend(uint32_t *w, uint32_t g, uint32_t ch, uint32_t bend14)          { return midi1(w, g, 0xE0 | (ch & 15), bend14 & 0x7F, bend14 >> 7); }

// A 7-bit SysEx packet: status 0 (complete in one), 1 (start), 2 (continue), 3 (end),
// up to 6 data bytes. A message longer than 6 bytes is several packets in order.
inline uint32_t sysex7(uint32_t *w, uint32_t group, uint32_t status, const uint8_t *data, uint32_t n)
{
	uint8_t b[6] = {};
	for (uint32_t i = 0; i < n && i < 6; ++i)
		b[i] = data[i] & 0x7F;
	w[0] = 0x30000000u | (group & 15) << 24 | (status & 15) << 20 | (n > 6 ? 6 : n) << 16 | uint32_t(b[0]) << 8 | b[1];
	w[1] = uint32_t(b[2]) << 24 | uint32_t(b[3]) << 16 | uint32_t(b[4]) << 8 | b[5];
	return 2;
}

// ---- MIDI 2.0 channel voice messages (type 4, two words) ----------------------------------
// Word 0 as type 2's (group, status and channel, then two bytes: a note, an index, a bank), word 1
// the value at its full width: velocity 16 bits (with a 16-bit attribute), controllers and
// pressure 32, pitch bend 32 (0x80000000 the centre).
inline uint32_t midi2(uint32_t *w, uint32_t group, uint32_t status, uint32_t b2, uint32_t b3, uint32_t data)
{
	w[0] = 0x40000000u | (group & 15) << 24 | (status & 0xFF) << 16 | (b2 & 0xFF) << 8 | (b3 & 0xFF);
	w[1] = data;
	return 2;
}
inline uint32_t note_on2(uint32_t *w, uint32_t g, uint32_t ch, uint32_t note, uint32_t vel16)  { return midi2(w, g, 0x90 | (ch & 15), note & 0x7F, 0, vel16 << 16); }
inline uint32_t note_off2(uint32_t *w, uint32_t g, uint32_t ch, uint32_t note, uint32_t vel16 = 0) { return midi2(w, g, 0x80 | (ch & 15), note & 0x7F, 0, vel16 << 16); }
inline uint32_t control2(uint32_t *w, uint32_t g, uint32_t ch, uint32_t cc, uint32_t value32) { return midi2(w, g, 0xB0 | (ch & 15), cc & 0x7F, 0, value32); }
inline uint32_t pitch_bend2(uint32_t *w, uint32_t g, uint32_t ch, uint32_t value32)            { return midi2(w, g, 0xE0 | (ch & 15), 0, 0, value32); }
// A program, and a bank with it if bank_valid (MSB, LSB: 7 bits each).
inline uint32_t program2(uint32_t *w, uint32_t g, uint32_t ch, uint32_t prog, bool bank_valid = false, uint32_t msb = 0, uint32_t lsb = 0)
{
	return midi2(w, g, 0xC0 | (ch & 15), 0, bank_valid ? 1 : 0, (prog & 0x7F) << 24 | (msb & 0x7F) << 8 | (lsb & 0x7F));
}
// A registered parameter (RPN: bank = its MSB, index = its LSB), set to a 32-bit value.
inline uint32_t rpn2(uint32_t *w, uint32_t g, uint32_t ch, uint32_t bank, uint32_t index, uint32_t value32)
{
	return midi2(w, g, 0x20 | (ch & 15), bank & 0x7F, index & 0x7F, value32);
}

// ---- UMP stream messages (type F, four words): an endpoint and its function blocks --------
// Word 0: type F, its form (bits 27-26: 0 complete, 1 start, 2 continue, 3 end), its status
// (bits 25-16), 16 bits of its own. Asked by Endpoint Discovery and Function Block Discovery, a
// MIDI 2.0 endpoint says what it is on its output (here: the source of the same name).
enum Stream : uint32_t {
	ENDPOINT_DISCOVERY = 0x000, ENDPOINT_INFO = 0x001, DEVICE_IDENTITY = 0x002, ENDPOINT_NAME = 0x003,
	FB_DISCOVERY = 0x010, FB_INFO = 0x011, FB_NAME = 0x012,
};
// Endpoint Discovery's filter (word 1) and Function Block Discovery's (word 0, bits 7-0).
constexpr uint32_t kWantInfo = 1, kWantIdentity = 2, kWantName = 4;      // endpoint
constexpr uint32_t kWantFbInfo = 1, kWantFbName = 2;                     // function block
inline uint32_t stream(uint32_t *w, uint32_t form, uint32_t status, uint32_t data16, uint32_t w1 = 0, uint32_t w2 = 0, uint32_t w3 = 0)
{
	w[0] = 0xF0000000u | (form & 3) << 26 | (status & 0x3FF) << 16 | (data16 & 0xFFFF);
	w[1] = w1;
	w[2] = w2;
	w[3] = w3;
	return 4;
}
inline uint32_t stream_status(uint32_t w0) { return (w0 >> 16) & 0x3FF; }
inline uint32_t stream_form(uint32_t w0)   { return (w0 >> 26) & 3; }
// A name as stream messages of `status` (ENDPOINT_NAME: 14 bytes each; FB_NAME: 13, after the
// block's number `fb`): into out[k][4], up to `cap` messages; how many.
inline uint32_t stream_name(uint32_t (*out)[4], uint32_t cap, uint32_t status, const char *name, uint32_t fb = 0)
{
	const bool block = status == FB_NAME;
	const uint32_t per = block ? 13 : 14;
	uint32_t n = 0;
	while (name[n]) ++n;
	const uint32_t msgs = n ? (n + per - 1) / per : 1;
	uint32_t k = 0;
	for (; k < msgs && k < cap; ++k) {
		uint8_t b[14] = {};
		for (uint32_t i = 0; i < per && k * per + i < n; ++i) b[i] = uint8_t(name[k * per + i]);
		const uint32_t form = msgs == 1 ? 0 : k == 0 ? 1 : k == msgs - 1 ? 3 : 2;
		const uint8_t *t = block ? b + 1 : b + 2;     // after word 0's own bytes
		stream(out[k], form, status, block ? (fb & 0x7F) << 8 | b[0] : uint32_t(b[0]) << 8 | b[1],
		       uint32_t(t[0]) << 24 | uint32_t(t[1]) << 16 | uint32_t(t[2]) << 8 | t[3],
		       uint32_t(t[4]) << 24 | uint32_t(t[5]) << 16 | uint32_t(t[6]) << 8 | t[7],
		       uint32_t(t[8]) << 24 | uint32_t(t[9]) << 16 | uint32_t(t[10]) << 8 | t[11]);
	}
	return k;
}

// A MIDI 2.0 channel voice message as MIDI 1.0 (type 2), by the UMP specification's default
// translation (M2-104-UM, "Translation"): out[] gets up to 4 one-word messages; returns how many.
// Values lose their low bits (16-bit velocity >> 9, 32-bit >> 25; pitch bend >> 18); a note-on's
// velocity that becomes 0 is 1 (0 would be a note-off); a program with its bank is the two bank
// selects first; an RPN or NRPN is its four controllers (101/100 or 99/98, then 6 and 38). Per-note
// messages and relative (N)RPNs have no MIDI 1.0 form: 0. Anything not type 4: 0.
inline uint32_t to_midi1(const uint32_t *in, uint32_t *out)
{
	if (ump_type(in[0]) != 4) return 0;
	const uint32_t g = ump_group(in[0]), st = ump_status(in[0]), ch = st & 15;
	const uint32_t b2 = (in[0] >> 8) & 0x7F, b3 = in[0] & 0x7F, v = in[1];
	switch (st & 0xF0) {
	case 0x80: return midi1(out, g, st, b2, v >> 25);
	case 0x90: { const uint32_t vel = v >> 25; return midi1(out, g, st, b2, vel ? vel : 1); }
	case 0xA0: return midi1(out, g, st, b2, v >> 25);
	case 0xB0: return midi1(out, g, st, b2, v >> 25);
	case 0xC0: {
		uint32_t n = 0;
		if (in[0] & 1) {                                     // the bank is valid
			n += control(out + n, g, ch, 0, (v >> 8) & 0x7F);
			n += control(out + n, g, ch, 32, v & 0x7F);
		}
		return n + midi1(out + n, g, st, (v >> 24) & 0x7F);
	}
	case 0xD0: return midi1(out, g, st, v >> 25);
	case 0xE0: { const uint32_t b = v >> 18; return midi1(out, g, st, b & 0x7F, b >> 7); }
	case 0x20: case 0x30: {
		const bool rpn = (st & 0xF0) == 0x20;
		uint32_t n = control(out, g, ch, rpn ? 101 : 99, b2);
		n += control(out + n, g, ch, rpn ? 100 : 98, b3);
		n += control(out + n, g, ch, 6, v >> 25);
		return n + control(out + n, g, ch, 38, (v >> 18) & 0x7F);
	}
	default: return 0;
	}
}

} // namespace midi
