// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// render_host: the command line, the song played into the instrument, the WAV file
// (render_host.hpp). The same file in hvyxg and hvopl3.
#include "render_host.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

namespace render {

namespace {

struct Common {
    std::uint32_t rate = 0;
    WavFormat format = WavFormat::F32;
    double gain_db = 0.0;
    double tail = 3.0;
    std::uint32_t ports = 0;  // 0: as many as the song uses
    bool base_level = false;
    int solo = -1;
    bool quiet = false;
};

const Option kCommon[] = {
    {"--rate", "HZ", "sample rate (default: the engine's)"},
    {"--format", "f32|s16|s24", "WAV samples: 32-bit float (default), 16- or 24-bit PCM (clipped)"},
    {"--gain", "DB", "output gain in dB (default 0)"},
    {"--tail", "SECONDS", "rendered after the last event (default 3)"},
    {"--ports", "N", "the song's ports (FF 21) on UMP groups 0..N-1 (default: as many as it uses and the engine takes; 1: all on group 0)"},
    {"--base-level", nullptr, "a dual-mode file (CANYON.MID): also channels 13-16, its base-level arrangement"},
    {"--solo", "CH", "MIDI channel 1..16 only (the other channels' note-ons dropped)"},
    {"--quiet", nullptr, "no report"},
    {"--help", nullptr, "this text"},
    {"--version", nullptr, "the version"},
};

void usage(Tool& t, std::FILE* out) {
    std::fprintf(out, "%s %s -- %s\nusage: %s [options] SONG.mid OUT.wav\n", t.name(), t.version(), t.about(), t.name());
    const auto list = [&](const char* title, const std::vector<Option>& opts) {
        std::fprintf(out, "%s:\n", title);
        for (const Option& o : opts) {
            const std::string head = std::string(o.name) + (o.arg ? std::string(" ") + o.arg : "");
            if (head.size() <= 26) std::fprintf(out, "  %-26s %s\n", head.c_str(), o.help);
            else std::fprintf(out, "  %s\n  %-26s %s\n", head.c_str(), "", o.help);
        }
    };
    list("options", std::vector<Option>(std::begin(kCommon), std::end(kCommon)));
    list(t.name(), t.options());
}

bool common_option(Common& c, const std::string& n, const std::string& v, std::string& err) {
    char* end = nullptr;
    const auto number = [&](double& out) {
        out = std::strtod(v.c_str(), &end);
        if (v.empty() || *end) err = n + ": not a number: " + v;
        return err.empty();
    };
    double d = 0;
    if (n == "--rate") {
        if (!number(d)) return false;
        if (d < 8000 || d > 192000) return (err = "--rate: 8000 … 192000"), false;
        c.rate = static_cast<std::uint32_t>(d);
    } else if (n == "--format") {
        if (v == "f32") c.format = WavFormat::F32;
        else if (v == "s16") c.format = WavFormat::S16;
        else if (v == "s24") c.format = WavFormat::S24;
        else return (err = "--format: f32, s16 or s24"), false;
    } else if (n == "--gain") {
        return number(c.gain_db);
    } else if (n == "--tail") {
        if (!number(c.tail)) return false;
        if (c.tail < 0) return (err = "--tail: not negative"), false;
    } else if (n == "--ports") {
        if (!number(d)) return false;
        if (d < 1 || d > 16) return (err = "--ports: 1 … 16"), false;
        c.ports = static_cast<std::uint32_t>(d);
    } else if (n == "--solo") {
        if (!number(d)) return false;
        if (d < 1 || d > 16) return (err = "--solo: 1 … 16"), false;
        c.solo = static_cast<int>(d) - 1;
    } else if (n == "--base-level") {
        c.base_level = true;
    } else if (n == "--quiet") {
        c.quiet = true;
    }
    return true;
}

const Option* find(const std::vector<Option>& opts, const std::string& n) {
    for (const Option& o : opts)
        if (n == o.name) return &o;
    return nullptr;
}

} // namespace

WavFile::WavFile(const std::string& path, std::uint32_t rate, WavFormat format)
    : out_(path, std::ios::binary), rate_(rate), format_(format) {
    if (out_) header();
}

WavFile::~WavFile() {
    if (out_.is_open()) close();
}

void WavFile::write(const float* x, std::size_t n) {
    if (format_ == WavFormat::F32) {
        out_.write(reinterpret_cast<const char*>(x), static_cast<std::streamsize>(n * sizeof(float)));
    } else {
        const double scale = format_ == WavFormat::S16 ? 32768.0 : 8388608.0;
        const auto hi = static_cast<long>(scale) - 1, lo = -static_cast<long>(scale);
        for (std::size_t i = 0; i < n; ++i) {
            const long v = std::clamp(std::lround(static_cast<double>(x[i]) * scale), lo, hi);
            const char b[3] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16)};
            out_.write(b, format_ == WavFormat::S16 ? 2 : 3);
        }
    }
    frames_ += n / 2;
}

void WavFile::close() {
    out_.seekp(0);
    header();
    out_.close();
}

void WavFile::header() {
    const auto le = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out_.put(static_cast<char>(v >> (8 * i)));
    };
    const std::uint32_t bps = format_ == WavFormat::F32 ? 4 : format_ == WavFormat::S16 ? 2 : 3;
    const bool fl = format_ == WavFormat::F32;
    const auto data = static_cast<std::uint32_t>(frames_ * 2 * bps);
    const std::uint32_t fmt_len = fl ? 18 : 16, fact_len = fl ? 12 : 0;
    out_.write("RIFF", 4);
    le(4 + (8 + fmt_len) + fact_len + (8 + data), 4);
    out_.write("WAVEfmt ", 8);
    le(fmt_len, 4);
    le(fl ? 3 : 1, 2);  // WAVE_FORMAT_IEEE_FLOAT / PCM
    le(2, 2);
    le(rate_, 4);
    le(rate_ * 2u * bps, 4);
    le(2 * bps, 2);
    le(8 * bps, 2);
    if (fl) {
        le(0, 2);  // cbSize
        out_.write("fact", 4);
        le(4, 4);
        le(frames_, 4);
    }
    out_.write("data", 4);
    le(data, 4);
}

std::unique_ptr<midifile::Song> read_song(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return nullptr;
    }
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::int32_t why = 0;
    std::unique_ptr<midifile::Song> song(midifile::open(file.data(), static_cast<std::uint32_t>(file.size()), &why));
    if (!song)
        error = path + ": " +
                (why == midifile::NOT_SMF ? "not a Standard MIDI File" : why == midifile::NO_MEMORY ? "out of memory" : "malformed");
    return song;
}

int main(Tool& tool, int argc, char** argv) {
    const std::vector<Option> common(std::begin(kCommon), std::end(kCommon)), own = tool.options();
    Common c;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage(tool, stdout);
            return 0;
        }
        if (a == "--version") {
            std::printf("%s %s\n", tool.name(), tool.version());
            return 0;
        }
        if (a.size() < 2 || a[0] != '-') {
            pos.push_back(a);
            continue;
        }
        const Option* o = find(common, a);
        const bool mine = o == nullptr;
        if (mine) o = find(own, a);
        if (!o) {
            std::fprintf(stderr, "%s: unknown option %s (--help)\n", tool.name(), a.c_str());
            return 2;
        }
        std::string v;
        if (o->arg) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: %s needs %s\n", tool.name(), a.c_str(), o->arg);
                return 2;
            }
            v = argv[++i];
        }
        std::string err;
        if (!(mine ? tool.option(a, v, err) : common_option(c, a, v, err))) {
            std::fprintf(stderr, "%s: %s\n", tool.name(), err.c_str());
            return 2;
        }
    }
    if (pos.size() != 2) {
        usage(tool, stderr);
        return 2;
    }

    // The song: every track's events in time order, as UMP, with their times in microseconds.
    std::string err;
    const std::unique_ptr<midifile::Song> song = read_song(pos[0], err);
    if (!song) {
        std::fprintf(stderr, "%s: %s\n", tool.name(), err.c_str());
        return 1;
    }

    // The instrument.
    const std::uint32_t rate = c.rate ? c.rate : tool.default_rate();
    const std::uint32_t want = c.ports ? c.ports : std::max<std::uint32_t>(1, song->ports());
    std::unique_ptr<Instrument> inst = tool.make(Setup{midi::Format{rate, 2}, want, pos[0], pos[1], c.format}, err);
    if (!inst) {
        std::fprintf(stderr, "%s: %s\n", tool.name(), err.c_str());
        return 1;
    }
    const std::uint32_t groups = std::min(want, std::max<std::uint32_t>(1, inst->info().groups));

    // Each event at its sample: round(t · rate). A port the instrument takes goes on its own UMP
    // group, any other on group 0, as midifile's player does.
    struct Timed {
        std::uint64_t frame;
        std::uint32_t ump[2], words;
    };
    std::vector<Timed> ev;
    ev.reserve(song->count());
    for (std::uint32_t i = 0; i < song->count(); ++i) {
        const midifile::Event& e = song->event(i);
        Timed t{frame_of(e.us, rate), {e.ump[0], e.ump[1]}, e.words};
        const std::uint32_t port = song->port(i);
        if (groups > 1 && port < groups) t.ump[0] = (t.ump[0] & ~0x0F000000u) | port << 24;
        if (midi::ump_type(t.ump[0]) == 2) {
            const std::uint32_t st = midi::ump_status(t.ump[0]), ch = st & 15;
            if (song->dual_mode() && !c.base_level && ch >= 12) continue;
            if (c.solo >= 0 && (st & 0xF0) == 0x90 && static_cast<int>(ch) != c.solo) continue;
        }
        ev.push_back(t);
    }
    const std::uint64_t last = ev.empty() ? 0 : ev.back().frame;
    const std::uint32_t block = std::max<std::uint32_t>(1, inst->block());
    const std::uint64_t end = (last + static_cast<std::uint64_t>(c.tail * rate) + block - 1) / block * block;

    WavFile wav(pos[1], rate, c.format);
    if (!wav.ok()) {
        std::fprintf(stderr, "%s: cannot write %s\n", tool.name(), pos[1].c_str());
        return 1;
    }
    const float gain = static_cast<float>(std::pow(10.0, c.gain_db / 20.0));
    const std::uint32_t kChunk = std::max<std::uint32_t>(1, inst->chunk());
    std::vector<float> buf(2 * static_cast<std::size_t>(kChunk));
    std::vector<std::int32_t> ibuf(2 * static_cast<std::size_t>(kChunk));
    float peak = 0;
    std::uint64_t over = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t next = 0;
    for (std::uint64_t at = 0; at < end;) {
        auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(kChunk, end - at));
        // The messages of the block about to be rendered, at their offsets in it.
        for (; next < ev.size() && ev[next].frame < at + n; ++next) {
            const auto off = static_cast<std::uint32_t>(ev[next].frame > at ? ev[next].frame - at : 0);
            if (!inst->event(off, ev[next].ump, ev[next].words)) {
                n = std::max<std::uint32_t>(off, 1);  // no room: render up to it, then send it again
                break;
            }
        }
        if (!inst->render_float(buf.data(), n)) {
            inst->render(ibuf.data(), n);
            for (std::uint32_t i = 0; i < 2 * n; ++i) buf[i] = static_cast<float>(ibuf[i]) / 32768.0f;
        }
        for (std::uint32_t i = 0; i < 2 * n; ++i) {
            if (gain != 1.0f) buf[i] *= gain;
            const float a = std::fabs(buf[i]);
            peak = std::max(peak, a);
            over += a > 1.0f;
        }
        wav.write(buf.data(), 2 * static_cast<std::size_t>(n));
        at += n;
    }
    wav.close();
    inst->finish();
    if (!c.quiet) {
        const double secs = static_cast<double>(end) / rate;
        const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::string d = inst->describe();
        std::fprintf(stderr, "%s: %s%s%s -> %s: %.2f s at %u Hz%s, peak %+.1f dBFS%s; %.2f s (%.0fx real time)\n", tool.name(),
                     pos[0].c_str(), *song->title() ? " (" : "", *song->title() ? (std::string(song->title()) + ")").c_str() : "",
                     pos[1].c_str(), secs, rate, groups > 1 ? (", " + std::to_string(groups) + " ports").c_str() : "",
                     peak > 0 ? 20.0 * std::log10(peak) : -INFINITY,
                     over ? (c.format == WavFormat::F32 ? " (over full scale)" : " (clipped)") : "", spent,
                     spent > 0 ? secs / spent : 0.0);
        if (!d.empty()) std::fprintf(stderr, "  %s\n", d.c_str());
    }
    return 0;
}

} // namespace render
