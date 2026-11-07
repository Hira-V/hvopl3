# Testing hvopl3

The scalar build is the definition of hvopl3's output. Every accelerated path (the Andes DSP mix,
each RVV variant) must produce **exactly the same samples** in every mode, and the tests check that
bit for bit. Each test suite also has a set of planted errors that it must catch, so a passing run is
known to be able to fail.

## Unit tests (host)

```sh
make -C tests          # t_ext: the extensions and the output contract
sh tests/plant.sh      # plants errors in a copy of src/; every one must be caught
```

`t_ext` checks pan, sends, gain, the mix (fast and slow paths, clipping, adding to the dry output),
and that the samples do not depend on how a run is cut into `render` calls, in every mode.

## Traces

The bench replays **HVO2 traces**: register writes, extension calls and sample counts, recorded from
a player or written by hand ([TRACE-FORMAT.md](TRACE-FORMAT.md)). The scripts read them from
`bench/traces/`. An old HVO1 trace is refused, by its magic; `python3 -I tools/hvo1to2.py OLD.hvo1`
converts it (to the same samples).

```sh
mkdir -p bench/traces
python3 -I bench/mktrace.py bench/traces    # synthetic tests: envelopes, waves, 4-op, rhythm, pan...
```

## The bench (host)

```sh
make -C bench -j
bench/build/play TRACE.hvo2 hvopl OUT       # writes OUT.dry.wav (+ .rev.wav, .cho.wav)
```

`play` renders a trace to 32-bit stereo WAV files and reports the time spent in `render`. Its chip
names: `hvopl` (48 kHz), `hvopl96` (96 kHz), `hvoplhq` and `hvopl96hq` (all high-quality flags).
`bench/native` renders at the chip's own rate. `bench/compare.py` compares two WAV files (alignment,
gain, spectral and envelope distance); `bench/purity.py` measures harmonic purity on the `s_fmdepth`
trace.

## Vector targets under qemu

```sh
bench/vec/run.sh x280,v128,t2,baby,rv64   # each trace's digests on each target vs the host build
HQ=1 bench/vec/run.sh x280                # ... and every high-quality flag set
bench/vec/t_ext.sh x280,v128              # the unit tests, bare metal
sh bench/vec/plant.sh                     # planted errors in the RVV high-quality path
COUNT=1 bench/vec/run.sh x280             # instructions per frame instead (qemu -icount)
```

Targets: `x280` (RVV 1.0, VLEN 512), `v128` (RVV 1.0, VLEN 128), `t2` (RV32 Zve32x), `baby`
(RV32IMF) and `rv64` (RV64GC, scalar). Needs a RISC-V GCC (`GXX=`, default
`~/opt/riscv64-elf-gcc15/bin/riscv64-unknown-elf-g++`) and `qemu-system-riscv32/64`. `JOBS=` sets
how many qemu runs go at once; `QEMU_SECS=` the time limit for each (default 600). The C920 has no
emulator: its build is checked by compiling it and, for high quality, by running the RVV 1.0 build
with `-DHVOPL_NO_RTZ`.

## The A25

```sh
bench/a25/run.sh hvopl          # instructions per 48 kHz frame, per trace (hvopl_nodsp: the scalar path)
bench/a25/digest.sh             # the samples, bit for bit against the host build
bench/a25/t_ext.sh              # the unit tests on the A25 build
sh bench/a25/plant_mix.sh       # planted errors in the Andes DSP mix
```

These build with the Andes GCC (`ANDES=`, default `~/opt/andes-rv32imafdc`) and run under the
Hira-V emulator, `rv32emu`, which models each Andes DSP instruction. They expect this repository to
be checked out inside [Hira-V](https://hira-v.com), where they find `emu/rv32emu`. The emulator
counts instructions, not cycles.

## The MIDI driver

```sh
make -C tests                 # t_drv and t_drv_asan (under the sanitizers) beside t_ext
sh tests/plant_drv.sh         # planted errors in drv/hvopl3_drv.cpp; every one must be caught
sh bench/vec/drv.sh           # tests/song.hpp with DMXOPL3, six set-ups, on each qemu target vs the host
sh bench/a25/drv.sh           # the same on the A25, and the driver's instructions a frame apart from the chips'
EVENTS=F sh bench/a25/drv.sh  # ... and a real song's 8 s (bench/build/mid2wav SONG.mid OUT.wav --dump F --dump-from S)
```

`bench/build/mid2wav SONG.mid OUT.wav` (make -C bench) renders a Standard MIDI File through the driver to
a WAV for listening: `--bank`, `--levels` (a `kGmLevels` / `kGmKits` C table), `--chips`, `--96k`, `--hq`,
`--hard-pan`, `--linear`, `--stream`, `--buses`. MIDI files and WAVs are never committed.
docs/DRIVER.md section 12 lists what t_drv checks.
