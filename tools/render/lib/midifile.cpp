// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Copied from Hira-V (https://hira-v.com), software/lib/midifile.cpp, as MIDIFILE_STANDALONE builds it; its references to
// docs/ and lib/ are to Hira-V's tree (README.md, "License").
// lib/midifile.cpp -- midifile.lib: Standard MIDI Files read into UMP events in time, and a
// player that writes them to a MIDI destination ahead of time, time-stamped (lib/midifile.hpp;
// docs/MIDI.md section 10).
//
// READING. Two passes over every track: the first counts the events (to allocate once), the
// second makes them -- channel voice messages (running status kept) as UMP type 2, a SysEx as
// type-3 packets of up to 6 bytes, the tempo (FF 51) into the tempo map, the first track's name
// (FF 03) the title, the port (FF 21) each event's port from there on in its track; other metas
// are skipped. Then the tracks merged by tick (a format 2 file's one after another), equal ticks
// in track order (a heap of the tracks: any number of them, since 0.6.1), and each tick turned into
// microseconds by the tempo map (500 000 us a quarter note until it says) -- or, with SMPTE time
// (since 0.6.1), by the frame rate and ticks a frame alone.
//
// SYSEX (since 0.6.0). An F0 event whose bytes end in F7 is one message. One that does not is
// the start of a divided message: F7 events after it carry the rest, each at its own time, the
// last ending in F7 -- the packets go out as the bytes come (a start, continuations, an end). A
// divided message that is never finished ends at its last piece, when anything else comes in its
// track (or the track ends). An F7 event otherwise is an escape: its bytes as they are. One that
// holds a whole SysEx (F0 .. F7) is read as one; any other is skipped (a real-time message or a
// song position has no meaning in a file played by its own clock).
//
// RMID (since 0.6.0): a RIFF file of form RMID (Microsoft's) holds the Standard MIDI File in its
// "data" chunk; that is read.
//
// Every length is checked against the chunk it is in: a file cut short or lying gives BAD, never a
// read past the end.
#include <initializer_list>
#include <new>
#include "lib/midifile.hpp"
#include "lib/midi.hpp"

#define MIDIFILE_EXPORT __attribute__((visibility("default")))

namespace {

using midifile::Event;

struct Raw {                          // an event as read: its tick, then its track's order
	uint64_t tick;
	uint32_t ump[2];
	uint8_t words;
	uint8_t port;                     // FF 21's, 0 until a track says (Song::port)
};
constexpr uint32_t kPorts = 16;       // a port is a UMP group: 16 of them
struct Tempo {
	uint64_t tick;
	uint32_t us_per_quarter;
};

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t be16(const uint8_t *p) { return uint32_t(p[0]) << 8 | p[1]; }

// Times past kSat (2^40 us: some 12.7 days) stay there. A file's deltas can add up to more than 64
// bits hold once multiplied, and a time that wrapped would run backwards; and the player multiplies
// a time by mtime's ticks a microsecond, which must fit too. No song is that long.
constexpr uint64_t kSat = 1ull << 40;
uint64_t add_sat(uint64_t a, uint64_t b) { return a >= kSat || b >= kSat - a ? kSat : a + b; }
// a x m / d, saturating at kSat: m < 2^31 and d < 2^24 here, so the remainder's product fits.
// Standalone, the compiler's division: the quotient and remainder of one.
uint64_t muldiv_sat(uint64_t a, uint64_t m, uint64_t d)
{
	const uint64_t q = a / d, r = a % d;
	if (q && m > kSat / q) return kSat;
	return add_sat(q * m, r * m / d);
}

// One track's events, counted (out null) or made. False: the track lies about its length.
struct Track {
	const uint8_t *p, *end;
};
bool vlq(Track &t, uint32_t &v)
{
	v = 0;
	for (int i = 0; i < 4; ++i) {
		if (t.p >= t.end) return false;
		const uint8_t b = *t.p++;
		v = v << 7 | (b & 0x7F);
		if (!(b & 0x80)) return true;
	}
	return false;
}

// A SysEx message made into type-3 packets as its bytes come, which may be in several events (a
// divided message). A packet of 6 goes out only once a byte past it comes, so the last is known
// for the last; each packet at the time of the event that completed it (the last: of the last piece).
struct SysexOut {
	Raw *out;
	uint32_t *n;
	uint8_t port = 0;
	bool open = false;                    // a message started and not ended
	bool first = true;                    // none of its packets out yet
	uint8_t carry[6] = {};
	uint32_t carried = 0;
	uint64_t last_tick = 0;               // its last piece's

	void packet(uint64_t tick, uint32_t status)
	{
		if (out) {
			Raw &r = out[*n];
			r.tick = tick;
			r.port = port;
			r.words = uint8_t(midi::sysex7(r.ump, 0, status, carry, carried));
		}
		++*n;
		carried = 0;
		first = false;
	}
	void begin(uint8_t p) { open = true; first = true; carried = 0; port = p; }
	void bytes(uint64_t tick, const uint8_t *b, uint32_t len)
	{
		last_tick = tick;
		for (uint32_t i = 0; i < len; ++i) {
			if (carried == 6) packet(tick, first ? 1 : 2);   // a start, or a continuation
			carry[carried++] = b[i] & 0x7F;
		}
	}
	void end()                            // its last packet (the whole message, if one)
	{
		if (!open) return;
		packet(last_tick, first ? 0 : 3);
		open = false;
	}
};

// Reads one track: events into out (if not null; counted in *n either way), tempos into tempo (if
// not null; counted in *nt), the name (if `name`). Ticks offset by `base`; *end_tick gets its last;
// *port_max the highest port its events are on.
bool read_track(Track t, uint64_t base, Raw *out, uint32_t *n, Tempo *tempo, uint32_t *nt,
                char *name, uint32_t name_cap, uint64_t *end_tick, uint32_t *port_max)
{
	uint64_t tick = base;
	uint8_t running = 0, port = 0;
	SysexOut sx{out, n};
	auto used = [&](uint8_t p) { if (p > *port_max) *port_max = p; };
	while (t.p < t.end) {
		uint32_t dt;
		if (!vlq(t, dt)) return false;
		tick += dt;
		if (t.p >= t.end) return false;
		uint8_t st = *t.p;
		if (st >= 0x80) ++t.p;
		else if (running) st = running;           // running status: the byte read is data
		else return false;
		if (st == 0xF7 && sx.open) {              // the next piece of a divided SysEx
			uint32_t len;
			if (!vlq(t, len) || len > uint32_t(t.end - t.p)) return false;
			const bool last = len && t.p[len - 1] == 0xF7;
			running = 0;
			sx.bytes(tick, t.p, last ? len - 1 : len);
			if (last) sx.end();
			t.p += len;
			continue;
		}
		sx.end();                                 // anything else: a divided SysEx left open ends here
		if (st == 0xFF) {                         // a meta event
			if (t.p >= t.end) return false;
			const uint8_t type = *t.p++;
			uint32_t len;
			if (!vlq(t, len) || len > uint32_t(t.end - t.p)) return false;
			if (type == 0x51 && len == 3) {
				if (tempo) tempo[*nt] = Tempo{tick, uint32_t(t.p[0]) << 16 | uint32_t(t.p[1]) << 8 | t.p[2]};
				++*nt;
			} else if (type == 0x03 && name && !name[0]) {
				uint32_t k = 0;
				for (; k < len && k + 1 < name_cap; ++k) name[k] = char(t.p[k]);
				name[k] = 0;
			} else if (type == 0x21 && len == 1) {
				port = uint8_t(t.p[0] % kPorts);  // the MIDI port: this track's events from here on
			}
			t.p += len;
			if (type == 0x2F) break;              // the end of the track
			continue;
		}
		if (st == 0xF0 || st == 0xF7) {           // SysEx (F0), or an escape (F7)
			running = 0;
			uint32_t len;
			if (!vlq(t, len) || len > uint32_t(t.end - t.p)) return false;
			const uint8_t *b = t.p;
			uint32_t body = len;
			if (st == 0xF7) {                     // an escape: read only if it holds a whole SysEx
				if (len < 2 || b[0] != 0xF0 || b[len - 1] != 0xF7) { t.p += len; continue; }
				++b;
				body = len - 2;
			} else if (len && b[len - 1] == 0xF7) {
				body = len - 1;                   // its bytes, less the F7
			}
			used(port);
			sx.begin(port);
			sx.bytes(tick, b, body);
			if (st == 0xF7 || (len && t.p[len - 1] == 0xF7))
				sx.end();                         // whole; else divided: F7 events carry the rest
			t.p += len;
			continue;
		}
		if (st < 0x80 || st >= 0xF0) return false; // a system common message in a file: not valid here
		running = st;
		const uint32_t need = (st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 ? 1 : 2;
		if (uint32_t(t.end - t.p) < need) return false;
		const uint8_t d1 = t.p[0] & 0x7F, d2 = need == 2 ? t.p[1] & 0x7F : 0;
		t.p += need;
		used(port);
		if (out) {
			Raw &r = out[*n];
			r.tick = tick;
			r.port = port;
			r.words = uint8_t(midi::midi1(r.ump, 0, st, d1, d2));
			r.ump[1] = 0;
		}
		++*n;
	}
	sx.end();                                     // a divided SysEx the track never finished
	*end_tick = tick;
	return true;
}

class SongImpl final : public midifile::Song {
public:
	~SongImpl() override { delete[] ev_; delete[] port_; }
	uint32_t count() const override { return n_; }
	const Event &event(uint32_t i) const override { return ev_[i < n_ ? i : 0]; }
	uint64_t length_us() const override { return length_; }
	uint32_t format() const override { return format_; }
	uint32_t tracks() const override { return tracks_; }
	const char *title() const override { return title_; }
	bool dual_mode() const override { return dual_; }
	uint32_t ports() const override { return ports_; }
	uint32_t port(uint32_t i) const override { return i < n_ ? port_[i] : 0; }

	int32_t read(const uint8_t *d, uint32_t size)
	{
		// RMID: a RIFF file whose "data" chunk is the Standard MIDI File
		if (size >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
		    d[8] == 'R' && d[9] == 'M' && d[10] == 'I' && d[11] == 'D') {
			auto le32 = [](const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; };
			uint32_t at = 12;
			for (;;) {
				if (size - at < 8) return midifile::NOT_SMF;            // no "data" chunk
				const uint32_t len = le32(d + at + 4);
				if (len > size - at - 8) return midifile::BAD;
				if (d[at] == 'd' && d[at + 1] == 'a' && d[at + 2] == 't' && d[at + 3] == 'a') {
					d += at + 8;
					size = len;
					break;
				}
				at += 8 + len;
				if (len & 1) {                                          // a chunk is padded to even
					if (at >= size) return midifile::NOT_SMF;
					++at;
				}
			}
		}
		if (size < 14 || d[0] != 'M' || d[1] != 'T' || d[2] != 'h' || d[3] != 'd' || be32(d + 4) < 6)
			return midifile::NOT_SMF;
		const uint32_t hlen = be32(d + 4);
		if (hlen > size - 8) return midifile::BAD;
		format_ = be16(d + 8);
		const uint32_t ntrk = be16(d + 10), division = be16(d + 12);
		if (format_ > 2) return midifile::BAD;
		// THE TIME BASE: ticks a quarter note (the tempo map applies), or SMPTE -- frames a second
		// (24, 25, 29 = 29.97 drop-frame, 30) and ticks a frame, the tempo map ignored (since 0.6.1).
		// A tick is num/den microseconds of the tempo's (ticks a quarter) or outright (SMPTE).
		const bool smpte = division & 0x8000;
		uint64_t smpte_num = 0, smpte_den = 0;
		if (smpte) {
			const uint32_t fps = uint32_t(-int32_t(int8_t(division >> 8))), tpf = division & 0xFF;
			if ((fps != 24 && fps != 25 && fps != 29 && fps != 30) || !tpf) return midifile::BAD;
			smpte_num = fps == 29 ? 1001000000ull : 1000000ull;              // 29.97: 30000/1001 frames a second
			smpte_den = fps == 29 ? 30000ull * tpf : uint64_t(fps) * tpf;
		} else if (!division) {
			return midifile::BAD;
		}
		// the tracks: where each is -- as many as the header says and the file holds (since 0.6.1; 64 before)
		uint32_t have = 0;
		for (uint32_t at = 8 + hlen; at + 8 <= size && have < ntrk;) {
			const uint32_t len = be32(d + at + 4);
			if (len > size - at - 8) return midifile::BAD;
			if (d[at] == 'M' && d[at + 1] == 'T' && d[at + 2] == 'r' && d[at + 3] == 'k') ++have;
			at += 8 + len;
		}
		struct T { Track tr; uint32_t count, first, pos; };
		T *tk = new (std::nothrow) T[have ? have : 1];
		if (!tk) return midifile::NO_MEMORY;
		uint32_t nt = 0;
		for (uint32_t at = 8 + hlen; at + 8 <= size && nt < have;) {
			const uint32_t len = be32(d + at + 4);
			if (d[at] == 'M' && d[at + 1] == 'T' && d[at + 2] == 'r' && d[at + 3] == 'k')
				tk[nt++] = T{Track{d + at + 8, d + at + 8 + len}, 0, 0, 0};
			at += 8 + len;
		}
		tracks_ = nt;
		// pass 1: how many events and tempos (and each track's end, for format 2)
		uint32_t ntempo = 0, total = 0, port_max = 0;
		uint64_t base = 0;
		for (uint32_t k = 0; k < nt; ++k) {
			uint64_t end = 0;
			if (!read_track(tk[k].tr, base, nullptr, &tk[k].count, nullptr, &ntempo, nullptr, 0, &end, &port_max)) {
				delete[] tk;
				return midifile::BAD;
			}
			if (format_ == 2) base = end;
			total += tk[k].count;
		}
		ports_ = port_max + 1;
		Raw *raw = new (std::nothrow) Raw[total ? total : 1];
		Tempo *tempo = new (std::nothrow) Tempo[ntempo ? ntempo : 1];
		uint32_t *heap = new (std::nothrow) uint32_t[nt ? nt : 1];
		ev_ = new (std::nothrow) Event[total ? total : 1];
		port_ = new (std::nothrow) uint8_t[total ? total : 1];
		if (!raw || !tempo || !heap || !ev_ || !port_) {
			delete[] raw; delete[] tempo; delete[] heap; delete[] tk;
			return midifile::NO_MEMORY;
		}
		// pass 2: the events, each track's after the last's
		uint32_t made = 0, tmade = 0;
		uint64_t last_tick = 0;
		base = 0;
		for (uint32_t k = 0; k < nt; ++k) {
			tk[k].first = made;
			uint32_t m = 0;
			uint64_t end = 0;
			uint32_t pm = 0;
			read_track(tk[k].tr, base, raw + made, &m, tempo, &tmade, k == 0 ? title_ : nullptr, sizeof title_, &end, &pm);
			made += m;
			if (end > last_tick) last_tick = end;
			if (format_ == 2) base = end;
		}
		// the tempo map in tick order (stable: an insertion sort, as a file has a handful)
		for (uint32_t i = 1; i < tmade; ++i)
			for (uint32_t j = i; j > 0 && tempo[j - 1].tick > tempo[j].tick; --j) {
				const Tempo t = tempo[j]; tempo[j] = tempo[j - 1]; tempo[j - 1] = t;
			}
		// each tick to microseconds: (ticks x num) / den, saturating far past any song's length
		uint64_t seg_tick = 0, seg_us = 0;
		uint32_t seg_tempo = 500000, ti = 0;
		auto us_of = [&](uint64_t tick) -> uint64_t {
			if (smpte) return muldiv_sat(tick, smpte_num, smpte_den);
			while (ti < tmade && tempo[ti].tick <= tick) {
				seg_us = add_sat(seg_us, muldiv_sat(tempo[ti].tick - seg_tick, seg_tempo, division));
				seg_tick = tempo[ti].tick;
				seg_tempo = tempo[ti].us_per_quarter ? tempo[ti].us_per_quarter : 500000;
				++ti;
			}
			return add_sat(seg_us, muldiv_sat(tick - seg_tick, seg_tempo, division));
		};
		// merged by tick, equal ticks in track order: a heap of the tracks by their next event
		auto key_less = [&](uint32_t a, uint32_t b) {
			const uint64_t ta = raw[tk[a].first + tk[a].pos].tick, tb = raw[tk[b].first + tk[b].pos].tick;
			return ta < tb || (ta == tb && a < b);
		};
		uint32_t hn = 0;
		auto sift_down = [&](uint32_t i) {
			for (;;) {
				uint32_t m = i;
				const uint32_t l = 2 * i + 1, r = l + 1;
				if (l < hn && key_less(heap[l], heap[m])) m = l;
				if (r < hn && key_less(heap[r], heap[m])) m = r;
				if (m == i) return;
				const uint32_t t = heap[i]; heap[i] = heap[m]; heap[m] = t;
				i = m;
			}
		};
		for (uint32_t k = 0; k < nt; ++k)
			if (tk[k].count) heap[hn++] = k;
		for (uint32_t i = hn / 2; i-- > 0;) sift_down(i);
		n_ = 0;
		while (hn) {
			T &t = tk[heap[0]];
			const Raw &r = raw[t.first + t.pos++];
			port_[n_] = r.port;
			Event &e = ev_[n_++];
			e.us = us_of(r.tick);
			e.ump[0] = r.ump[0];
			e.ump[1] = r.ump[1];
			e.words = r.words;
			if (t.pos == t.count) heap[0] = heap[--hn];          // that track is done
			sift_down(0);
		}
		length_ = us_of(last_tick);
		delete[] raw;
		delete[] tempo;
		delete[] heap;
		delete[] tk;
		// dual-mode (Song::dual_mode): each channel's notes, and channel 16's range
		uint32_t notes[16] = {};
		uint32_t lo16 = 127, hi16 = 0;
		for (uint32_t i = 0; i < n_; ++i) {
			const uint32_t w = ev_[i].ump[0];
			if (port_[i]) continue;                    // dual-mode is a one-port layout
			if (midi::ump_type(w) != 2 || (midi::ump_status(w) & 0xF0) != 0x90 || !midi::ump_data2(w)) continue;
			const uint32_t ch = midi::ump_status(w) & 15, note = midi::ump_data1(w);
			++notes[ch];
			if (ch == 15) { lo16 = note < lo16 ? note : lo16; hi16 = note > hi16 ? note : hi16; }
		}
		dual_ = notes[9] && notes[15] && lo16 >= 27 && hi16 <= 87 && (notes[12] || notes[13] || notes[14]) &&
		        !notes[10] && !notes[11];
		return midifile::OK;
	}

private:
	Event *ev_ = nullptr;
	uint8_t *port_ = nullptr;                      // each event's port (Song::port)
	uint32_t n_ = 0, format_ = 0, tracks_ = 0, ports_ = 1;
	uint64_t length_ = 0;
	char title_[64] = {};
	bool dual_ = false;
};


} // namespace

namespace midifile {

MIDIFILE_EXPORT Song *open(const void *smf, uint32_t size, int32_t *err)
{
	auto *s = new (std::nothrow) SongImpl();
	if (!s) { if (err) *err = NO_MEMORY; return nullptr; }
	const int32_t e = s->read(static_cast<const uint8_t *>(smf), size);
	if (err) *err = e;
	if (e != OK) { delete s; return nullptr; }
	return s;
}


} // namespace midifile
