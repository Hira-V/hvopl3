// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// render_opl3 -- a Standard MIDI File through the MIDI driver (drv/) and its chips into a WAV file.
//
//   render_opl3 [options] SONG.mid OUT.wav      (render_opl3 --help: every option)
//
// Also the shortest example of the driver: hvopl3::drv behind Hira-V's instrument interface
// (midi::Instrument: UMP messages at sample offsets, then the block), as tools/render/render_host.hpp
// plays one -- the same host as hvyxg's render_yxg, so both take the same common options. OplInstrument
// is the whole of the glue (UMP to the driver's MIDI 1.0 bytes, SysEx packets gathered, a System Reset
// as GM System On); the rest is options. Bench extras: --stream renders through the HVO2 record stream
// and hvopl3::play, --buses also writes the send buses, --dump writes 8 s of the song's messages for
// bench/a25/drv.sh (EVENTS=).
//
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "hvopl3.hpp"
#include "hvopl3_drv.hpp"
#include "render/render_host.hpp"
#include "../tests/song.hpp"

using namespace hvopl3;

#define HVOPL3_STR2(x) #x
#define HVOPL3_STR(x) HVOPL3_STR2(x)

namespace {

std::vector<uint8_t> slurp(const std::string &path) {
	std::vector<uint8_t> v;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) return v;
	for (int c; (c = std::fgetc(f)) != EOF;) v.push_back(uint8_t(c));
	std::fclose(f);
	return v;
}

// Level corrections from C source: the 128 numbers after "kGmLevels", then each "{kit, {128}}" after "kGmKits".
bool read_levels(const std::vector<uint8_t> &src, int8_t *prog, std::vector<drv::KitLevels> &kits) {
	std::string s(src.begin(), src.end());
	const auto numbers = [&](size_t from, size_t count, std::vector<int> &v) {
		size_t p = from;
		while (v.size() < count && p < s.size()) {
			while (p < s.size() && !(isdigit(uint8_t(s[p])) || (s[p] == '-' && p + 1 < s.size() && isdigit(uint8_t(s[p + 1]))))) {
				if (s[p] == '/' && p + 1 < s.size() && s[p + 1] == '/') { while (p < s.size() && s[p] != '\n') ++p; }
				else ++p;
			}
			if (p >= s.size()) break;
			char *e;
			v.push_back(int(strtol(&s[p], &e, 10)));
			p = size_t(e - s.data());
		}
		return p;
	};
	size_t a = s.find("kGmLevels");
	if (a == std::string::npos) return false;
	a = s.find('{', a);
	std::vector<int> v;
	numbers(a, 128, v);
	if (v.size() != 128) return false;
	for (int i = 0; i < 128; ++i) prog[i] = int8_t(v[i]);
	size_t k = s.find("kGmKits");
	if (k == std::string::npos) return true;
	k = s.find('{', k);
	for (;;) {
		k = s.find('{', k + 1);
		if (k == std::string::npos) break;
		std::vector<int> w;
		const size_t e = numbers(k, 129, w);
		if (w.size() != 129) break;
		drv::KitLevels kl;
		kl.kit = uint8_t(w[0]);
		for (int i = 0; i < 128; ++i) kl.levels[i] = int8_t(w[1 + i]);
		kits.push_back(kl);
		k = s.find('}', e);
		if (k == std::string::npos) break;
		k = s.find('}', k + 1);   // past the kit's closing brace
		if (k == std::string::npos) break;
	}
	return true;
}

struct Settings {
	drv::Config cfg = [] { drv::Config c = drv::default_config(); c.chips = 2; return c; }();
	std::string bank = "drv/banks/DMXOPL3-GS.wopl", levels, dump;
	double dump_from = 0;
	bool buses = false;
};

// The driver and its chips as a midi::Instrument.
class OplInstrument final : public render::Instrument {
public:
	// Set up from the settings; false and `err` if the bank or the levels cannot be read.
	bool open(const Settings &s, const render::Setup &setup, std::string &err) {
		s_ = s;
		const std::vector<uint8_t> bank = slurp(s.bank);
		if (bank.empty()) { err = "cannot read the bank " + s.bank + " (--bank FILE.wopl)"; return false; }
		pool_mem_.assign(drv::pool_bytes(64), 0);
		pool_ = drv::pool_init(pool_mem_.data(), pool_mem_.size());
		const int lr = drv::load_wopl(*pool_, bank.data(), bank.size());
		if (lr != drv::LOAD_OK) { err = s.bank + ": not a WOPL bank (" + std::to_string(lr) + ")"; return false; }
		if (!s.levels.empty()) {
			int8_t prog[128];
			std::vector<drv::KitLevels> kits;
			if (!read_levels(slurp(s.levels), prog, kits)) { err = "no kGmLevels in " + s.levels; return false; }
			drv::set_levels(*pool_, prog, kits.data(), uint32_t(kits.size()));
		}
		d_ = std::make_unique<drv::Driver>();
		chips_ = std::make_unique<Chip[]>(drv::MAX_CHIPS);
		replay_ = std::make_unique<Chip[]>(drv::MAX_CHIPS);
		drv::init(*d_, *pool_, chips_.get(), s_.cfg);
		rate_ = setup.format.rate;
		if (s.buses) {
			const std::string &o = setup.out;
			const std::string stem = o.size() > 4 && o.substr(o.size() - 4) == ".wav" ? o.substr(0, o.size() - 4) : o;
			rev_wav_ = std::make_unique<render::WavFile>(stem + ".rev.wav", rate_, setup.wav);
			cho_wav_ = std::make_unique<render::WavFile>(stem + ".cho.wav", rate_, setup.wav);
		}
		info_ = {"OPL3", 1, midi::MIDI1};
		return true;
	}

	// One UMP message at its frame in the next block, to the driver as MIDI 1.0 bytes. False: the
	// driver's queue is full (the host renders up to here and sends it again).
	bool event(uint32_t offset, const uint32_t *ump, uint32_t words) override {
		const uint32_t type = midi::ump_type(ump[0]);
		if (type == 2) {
			const uint8_t st = uint8_t(midi::ump_status(ump[0]));
			const uint8_t m[3] = {st, uint8_t(midi::ump_data1(ump[0])), uint8_t(midi::ump_data2(ump[0]))};
			return send(offset, m, (st & 0xE0) == 0xC0 ? 2 : 3);   // program change, channel pressure: two bytes
		}
		if (type == 1 && midi::ump_status(ump[0]) == 0xFF) {      // System Reset: GM System On, the power-on state
			static const uint8_t kGmOn[] = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
			return send(offset, kGmOn, sizeof kGmOn);
		}
		if (type == 3 && words >= 2) {                            // 7-bit SysEx: gathered into one F0 .. F7
			const uint32_t st = (ump[0] >> 20) & 15, n = std::min<uint32_t>((ump[0] >> 16) & 15, 6);
			const uint8_t b[6] = {uint8_t(ump[0] >> 8), uint8_t(ump[0]), uint8_t(ump[1] >> 24), uint8_t(ump[1] >> 16),
			                      uint8_t(ump[1] >> 8), uint8_t(ump[1])};
			const size_t was = sx_.size();
			if (st == 0 || st == 1) sx_.assign(1, 0xF0);
			else if (sx_.empty()) return true;                     // a continuation with no start
			for (uint32_t i = 0; i < n; ++i) sx_.push_back(b[i] & 0x7F);
			if (sx_.size() > 512) { sx_.clear(); return true; }    // longer than any the driver takes
			if (st == 0 || st == 3) {
				sx_.push_back(0xF7);
				if (!send(offset, sx_.data(), uint32_t(sx_.size()))) {
					sx_.resize(was);                               // as before this packet: it comes again
					return false;
				}
				sx_.clear();
			}
		}
		return true;   // anything else: not the driver's
	}

	// The block at a 16-bit sample's scale, the chips' sum (direct mode), or through the HVO2 stream.
	void render(int32_t *out, uint32_t frames) override {
		rev_.assign(2 * size_t(frames), 0);
		cho_.assign(2 * size_t(frames), 0);
		const Output o = {out, rev_.data(), cho_.data(), false};
		if (!s_.cfg.stream) {
			drv::render(*d_, o, frames);
		} else {
			std::fill(out, out + 2 * size_t(frames), 0);
			if (recs_.empty()) recs_.resize(drv::stream_reserve(*d_) * 4 + 4096);
			for (uint32_t done = 0; done < frames;) {
				uint32_t cnt = 0;
				const uint32_t m = drv::render_stream(*d_, recs_.data(), uint32_t(recs_.size()), cnt, frames - done);
				const Output p = {o.dry + 2 * done, o.rev + 2 * done, o.cho + 2 * done, true};
				play(replay_.get(), s_.cfg.chips, recs_.data(), cnt, p, s_.cfg.rate, drv::chip_flags(*d_));
				done += m;
				if (!m && !cnt) break;
			}
		}
		uint32_t busy = 0;
		for (uint32_t k = 0; k < d_->cfg.chips; ++k) busy += d_->busy[k];
		most_ = std::max(most_, busy);
		if (rev_wav_) {
			std::vector<float> f(2 * size_t(frames));
			for (size_t i = 0; i < f.size(); ++i) f[i] = float(rev_[i]) / 32768.0f;
			rev_wav_->write(f.data(), f.size());
			for (size_t i = 0; i < f.size(); ++i) f[i] = float(cho_[i]) / 32768.0f;
			cho_wav_->write(f.data(), f.size());
		}
		pos_ += frames;
	}

	void reset() override { drv::reset(*d_); }
	const midi::InstrumentInfo &info() const override { return info_; }
	uint32_t chunk() const override { return rate_ / 100; }   // 10 ms: the driver's queue holds one block's messages

	void finish() override {
		if (rev_wav_) { rev_wav_->close(); cho_wav_->close(); }
		if (!s_.dump.empty()) {
			FILE *f = std::fopen(s_.dump.c_str(), "wb");
			if (f) { std::fwrite(dump_.data(), sizeof(song::Ev), dump_.size(), f); std::fclose(f); }
			std::fprintf(stderr, "render_opl3: %zu messages to %s\n", dump_.size(), s_.dump.c_str());
		}
	}

	std::string describe() const override {
		return std::to_string(s_.cfg.chips) + " chips (" + std::to_string(s_.cfg.chips * 18) + " 2-op voices; at most " +
		       std::to_string(most_) + " busy), " + s_.bank + (s_.cfg.stream ? ", through the HVO2 stream" : "");
	}

private:
	bool send(uint32_t offset, const uint8_t *m, uint32_t len) {
		if (!drv::send(*d_, offset, m, len)) return false;
		if (!s_.dump.empty()) record(pos_ + offset, m, len);
		return true;
	}
	// --dump: 8 seconds of messages at 48 kHz from --dump-from on, the controllers and programs before it at 0.
	void record(uint64_t frame, const uint8_t *m, uint32_t len) {
		const double sec = double(frame) / rate_;
		const bool note = (m[0] & 0xE0) == 0x80;
		if ((sec < s_.dump_from && note) || len > 11) return;
		const uint32_t at = sec < s_.dump_from ? 0 : uint32_t((sec - s_.dump_from) * 48000 + 0.5);
		if (at >= 8 * 48000) return;
		song::Ev e{at, uint8_t(len), {}};
		std::memcpy(e.m, m, len);
		dump_.push_back(e);
	}

	Settings s_;
	std::vector<uint8_t> pool_mem_;
	drv::Pool *pool_ = nullptr;
	std::unique_ptr<drv::Driver> d_;
	std::unique_ptr<Chip[]> chips_, replay_;
	std::vector<int32_t> rev_, cho_;
	std::vector<uint32_t> recs_;
	std::vector<uint8_t> sx_;
	std::unique_ptr<render::WavFile> rev_wav_, cho_wav_;
	std::vector<song::Ev> dump_;
	midi::InstrumentInfo info_{};
	uint32_t rate_ = 48000, most_ = 0;
	uint64_t pos_ = 0;
};

class Opl final : public render::Tool {
public:
	const char *name() const override { return "render_opl3"; }
	const char *version() const override { return HVOPL3_STR(HVOPL3_VERSION_MAJOR) "." HVOPL3_STR(HVOPL3_VERSION_MINOR) "." HVOPL3_STR(HVOPL3_VERSION_PATCH); }
	const char *about() const override { return "a Standard MIDI File through hvopl3's MIDI driver and OPL3 chips"; }
	uint32_t default_rate() const override { return 48000; }

	std::vector<render::Option> options() const override {
		return {
			{"--bank", "FILE.wopl", "the bank (default drv/banks/DMXOPL3-GS.wopl, from the repository root)"},
			{"--levels", "FILE", "level corrections: C source with a kGmLevels[128] table and {kit, {128 values}} kit tables"},
			{"--chips", "N", "1..32 (default 2), 18 2-op voices each"},
			{"--hq", nullptr, "every high-quality flag (the wave between its steps, the envelope and modulation unrounded)"},
			{"--hard-pan", nullptr, "pan by the chip's C0 left/right bits"},
			{"--linear", nullptr, "the linear volume curve"},
			{"--no-corrections", nullptr, "the patches' level corrections off"},
			{"--no-restrike", nullptr, "a Note On for a sounding note starts another voice"},
			{"--stream", nullptr, "through the HVO2 record stream and hvopl3::play (the same samples)"},
			{"--buses", nullptr, "also OUT.rev.wav and OUT.cho.wav, the reverb and chorus send buses"},
			{"--dump", "FILE", "8 s of the song's messages for bench/a25/drv.sh (EVENTS=FILE)"},
			{"--dump-from", "SECONDS", "... from this second on (the controllers and programs before it at the start)"},
		};
	}

	bool option(const std::string &n, const std::string &v, std::string &err) override {
		if (n == "--bank") s_.bank = v;
		else if (n == "--levels") s_.levels = v;
		else if (n == "--chips") {
			const long c = std::strtol(v.c_str(), nullptr, 10);
			if (c < 1 || c > long(drv::MAX_CHIPS)) { err = "--chips: 1 .. " + std::to_string(drv::MAX_CHIPS); return false; }
			s_.cfg.chips = uint32_t(c);
		} else if (n == "--hq") s_.cfg.chip_flags |= HQ_PHASE | HQ_ENVELOPE | HQ_MODULATION;
		else if (n == "--hard-pan") s_.cfg.hard_pan = true;
		else if (n == "--linear") s_.cfg.curve = drv::CURVE_LINEAR;
		else if (n == "--no-corrections") s_.cfg.level_corrections = false;
		else if (n == "--no-restrike") s_.cfg.restrike = false;
		else if (n == "--stream") s_.cfg.stream = true;
		else if (n == "--buses") s_.buses = true;
		else if (n == "--dump") s_.dump = v;
		else if (n == "--dump-from") s_.dump_from = std::strtod(v.c_str(), nullptr);
		return true;
	}

	std::unique_ptr<render::Instrument> make(const render::Setup &setup, std::string &err) override {
		if (setup.format.rate != RATE_48K && setup.format.rate != RATE_96K) { err = "--rate: 48000 or 96000 (the chips' rates)"; return nullptr; }
		s_.cfg.rate = Rate(setup.format.rate);
		auto in = std::make_unique<OplInstrument>();
		if (!in->open(s_, setup, err)) return nullptr;
		return in;
	}

private:
	Settings s_;
};

}  // namespace

int main(int argc, char **argv) {
	Opl tool;
	return render::main(tool, argc, argv);
}
