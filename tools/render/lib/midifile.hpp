// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Copied from Hira-V (https://hira-v.com), software/lib/midifile.hpp, as MIDIFILE_STANDALONE builds it; its references to
// docs/ and lib/ are to Hira-V's tree (README.md, "License").
#pragma once
// lib/midifile.hpp -- midifile.lib's interface: Standard MIDI Files read into UMP events in time,
// and a player that sends them to any MIDI destination ahead of their time, time-stamped
// (docs/MIDI.md section 10). A program that includes this links with midifile.lib at load time.
//
//   midifile::Song *s = midifile::open(data, size, &err);    // the file in memory: read whole
//   midifile::Player *p = midifile::player(s);
//   p->start(u::clock().ticks);
//   while (p->pump(writer, u::clock().ticks)) { ... wait a frame ... }
//
// A Song is the file's events in time order -- every track's merged, MIDI 1.0 channel voice
// messages as UMP type 2 and SysEx as type-3 packets, each at its time in microseconds from the
// start (the file's tempo map applied: format 0 and 1; a format 2 file's tracks one after
// another). The player writes them to a midisvc::Writer up to `ahead` before they are due, each
// stamped with its time, so the destination (the Instrument Host) starts each on its sample.
//
// TIME (since 0.6.1): ticks a quarter note through the tempo map, or SMPTE -- 24, 25, 29.97 (drop-frame,
// coded 29) or 30 frames a second and ticks a frame, the tempo map then ignored, as the format says.
// Times stop at 2^40 us (12.7 days): a file whose deltas add up past that ends there. Every track
// the header names and the file holds is read (64 at most before 0.6.1).
//
// GS, XG AND GENERAL MIDI 2 FILES are Standard MIDI Files like any other: what makes them so is in
// their messages -- SysEx, bank select, RPN and NRPN -- which are read and sent as they are; the
// synthesizer is what knows them. Since 0.6.0: a SysEx divided over F7 events read whole, an F7
// escape holding a SysEx read as one, RMID files (RIFF), each event's port (FF 21: the 32-channel
// files of two ports), and a seek that puts each RPN and NRPN back as the song set it.
//
// PORTS (since 0.6.0). A file for a two-port synthesizer (an SC-88's or MU's A and B, 32 channels)
// says each track's port with meta event FF 21. The events keep group 0 -- a program that sends
// them to an instrument itself sees the file as before -- and Song::port says each one's. The
// player writes them all on group 0 unless set_groups says the destination takes more: then each
// port's on its own group.
//
// Not this library's name, midi.lib: lib/midi.hpp is the instrument interface, header only, that
// every instrument's user includes (docs/MIDI.md section 6, "As built").
//
// STANDALONE (MIDIFILE_STANDALONE defined): the reader alone -- Event, Song, open -- with no Player and
// nothing of Hira-V's (MidiSvc, libu), for programs outside Hira-V; hvyxg and hvopl3 carry a copy
// made so (their renderers read files with it). Hira-V's build never defines it.
//
// THE CONTRACT (docs/Dynamic-libraries.md section 3): no state in the library; Song and Player
// are the program's (deleted by it), called through their vtables. Event is passed by its layout.
#include <stddef.h>
#include <stdint.h>

namespace midifile {

struct Event {
	uint64_t us;                  // from the song's start
	uint32_t ump[2];              // one UMP message: type 2 (one word) or 3 (two)
	uint32_t words;
};

enum Error : int32_t {
	OK = 0,
	NOT_SMF = -1,                 // no MThd
	BAD = -2,                     // a chunk or an event runs past its end
	NO_MEMORY = -3,
	SMPTE = -4,                   // time in SMPTE frames: not read before 0.6.1 (read since; no longer returned)
};

class Song {
public:
	virtual ~Song() = default;
	virtual uint32_t count() const = 0;
	virtual const Event &event(uint32_t i) const = 0;
	virtual uint64_t length_us() const = 0;           // the last event's time (or the end of track's)
	virtual uint32_t format() const = 0;              // 0, 1 or 2
	virtual uint32_t tracks() const = 0;
	virtual const char *title() const = 0;            // the first track's name ("" if none)
	// A DUAL-MODE file (Microsoft's MPC authoring, Windows 3.1's MIDI Mapper; CANYON.MID): the song
	// twice -- an "extended" arrangement on channels 1-10, drums on 10 (the General MIDI layout),
	// and a "base" one for simpler synths on 13-16, drums on 16. Seen as: notes on 10 and on 16,
	// 16's all in the drum range (27-87), some on 13-15, and none on 11 or 12. A General MIDI synth
	// would play both, the base drums as a melodic instrument. Since 0.4.0 (appended).
	virtual bool dual_mode() const = 0;
	// How many ports the song uses: one more than the highest port (FF 21) an event is on; 1 for a
	// file that never says. Since 0.6.0 (appended).
	virtual uint32_t ports() const = 0;
	// Event i's port, 0 to 15 (0 out of range). Since 0.6.0 (appended).
	virtual uint32_t port(uint32_t i) const = 0;
};

// Reads the whole file (copied: `smf` may go after). Null and *err why, if it cannot. Since 0.6.0 a
// RIFF file of form RMID too: the Standard MIDI File in its "data" chunk.
Song *open(const void *smf, uint32_t size, int32_t *err);


} // namespace midifile
