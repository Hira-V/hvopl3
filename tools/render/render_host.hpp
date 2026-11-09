// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
#pragma once
// render_host -- what the command-line renderers of hvyxg (render_yxg) and hvopl3 (render_opl3)
// share: a Standard MIDI File read into Universal MIDI Packets (lib/midifile.hpp, Hira-V's
// reader), each played into an instrument at its sample, and the audio written to a WAV file.
// The same file in both projects.
//
// The instrument is the engine behind Hira-V's interface (lib/midi.hpp, midi::Instrument), as
// a Hira-V instrument library wraps it: one UMP message at a sample offset into the block about
// to be rendered, then the block. So each renderer is also a short example of its library, and of
// the UMP model: a renderer is its engine's Instrument and its options, and this host does the
// rest. Neither project depends on Hira-V: the two lib/ files are copies.
//
//   class MyInstrument : public render::Instrument { ... };      // event(), render(), info()
//   class MyTool : public render::Tool { ... };                  // options, make()
//   int main(int argc, char** argv) { MyTool t; return render::main(t, argc, argv); }

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "lib/midi.hpp"
#include "lib/midifile.hpp"

namespace render {

/// A Standard MIDI File (or RMID) read whole: its events as UMP, in time order, with their times
/// in microseconds. Null and `error` when it cannot be read.
std::unique_ptr<midifile::Song> read_song(const std::string& path, std::string& error);

/// An event's sample at `rate`: the nearest to its time (whole microseconds, midifile's).
inline std::uint64_t frame_of(std::uint64_t us, std::uint32_t rate) { return (us * rate + 500000) / 1000000; }

/// WAV samples: 32-bit float, 16- or 24-bit PCM (rounded, clipped).
enum class WavFormat { F32, S16, S24 };

/// A stereo RIFF WAVE file, written as it goes; its sizes are filled in by close().
class WavFile {
public:
    WavFile(const std::string& path, std::uint32_t rate, WavFormat format);
    ~WavFile();
    bool ok() const { return static_cast<bool>(out_); }
    /// `n` samples, interleaved (L, R, …), full scale ±1.0.
    void write(const float* x, std::size_t n);
    void close();

private:
    void header();
    std::ofstream out_;
    std::uint32_t rate_;
    WavFormat format_;
    std::uint64_t frames_ = 0;
};

/// What the host knows when it asks for the instrument.
struct Setup {
    midi::Format format;     ///< the rate (--rate, else the tool's default) and 2 channels
    std::uint32_t groups;    ///< UMP groups wanted: the song's ports (--ports)
    std::string song, out;   ///< the paths on the command line
    WavFormat wav;           ///< --format, for any file of the tool's own
};

/// midi::Instrument, and what a file renderer asks beyond it today.
class Instrument : public midi::Instrument {
public:
    /// The block as 32-bit float (full scale ±1.0), when the engine renders float: nothing lost
    /// below a 16-bit step. False when it does not (the host then calls render()).
    virtual bool render_float(float* out, std::uint32_t frames) {
        (void)out;
        (void)frames;
        return false;
    }
    /// The output length is rounded up to a whole number of these frames (the engine's block).
    virtual std::uint32_t block() const { return 1; }
    /// The most frames the host renders at once (each render takes the messages of its frames).
    virtual std::uint32_t chunk() const { return 4096; }
    /// The song is over (after the last render): write what is the instrument's own.
    virtual void finish() {}
    /// One line for the report (the bank, the model).
    virtual std::string describe() const { return {}; }

    // A renderer needs no parameters, setups or banks loaded through the interface.
    std::uint32_t param_count() const override { return 0; }
    const midi::ParamInfo& param(std::uint32_t) const override {
        static const midi::ParamInfo none{"", 0, 0, 0};
        return none;
    }
    float get(std::uint32_t) const override { return 0; }
    void set(std::uint32_t, float) override {}
    std::uint32_t save(void*, std::uint32_t) const override { return 0; }
    bool load(const void*, std::uint32_t) override { return false; }
    bool load_bank(const void*, std::uint32_t, std::uint32_t) override { return false; }
};

/// An option of a renderer's own: `--name ARG` (arg null: a flag).
struct Option {
    const char* name;  ///< "--bank"
    const char* arg;   ///< "FILE", or nullptr for a flag
    const char* help;
};

/// A renderer: its options and its instrument.
class Tool {
public:
    virtual ~Tool() = default;
    virtual const char* name() const = 0;     ///< "render_yxg"
    virtual const char* version() const = 0;  ///< the library's version
    virtual const char* about() const = 0;    ///< one line: what it renders through
    virtual std::vector<Option> options() const = 0;
    /// One of options() given (value empty for a flag). False and `error` for a bad value.
    virtual bool option(const std::string& name, const std::string& value, std::string& error) = 0;
    /// The sample rate when --rate is not given.
    virtual std::uint32_t default_rate() const = 0;
    /// The instrument, once every option is in. Null and `error` when it cannot be made.
    virtual std::unique_ptr<Instrument> make(const Setup& setup, std::string& error) = 0;
};

/// The command line: `NAME [options] SONG.mid OUT.wav`, the common options and the tool's.
int main(Tool& tool, int argc, char** argv);

} // namespace render
