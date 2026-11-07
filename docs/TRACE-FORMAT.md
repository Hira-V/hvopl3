# HVO2: the trace format

A trace is what a MIDI player told some OPL3 chips, in order, with the timing of every run of samples
between the writes: everything needed to make the same sound again, nothing about how. Written by a
recorder (a MIDI player's OPL3 writes), by `bench/mktrace.py`, and by the MIDI driver's stream mode
(`drv/`, docs/DRIVER.md). Replayed by `hvopl3::play`; `bench/hvo2.hpp` reads trace files for the
bench. Little-endian 32-bit words. Up to 32 chips.

## Header (4 words)

| Word | |
|---|---|
| 0 | `0x324F5648` ("HVO2") |
| 1 | version, 1 |
| 2 | the rate the runs are counted at: 48000 |
| 3 | flags: bit 0 soft panning in use (PAN records carry real pans); bit 1 per-voice gains in use |

Records also travel without the header (the driver's stream to `play`); `play` takes HVO2 records
only.

## Records (one word each)

| Bits | |
|---|---|
| 3..0 | op |
| 5..4 | the chip's low two bits |
| 31..29 | the chip's high three bits: chip = bits 5..4 + 4 x bits 31..29, 0..31 |
| 14..6 | REG: the register, 0x000..0x1FF; RUN: low bits of the frames |
| 10..6 | PAN/SENDS/GAIN: the channel, 0..17 |
| 22..15 | REG: the value; PAN: the pan, 0..127; SENDS: reverb send 0..127; GAIN: the level, signed |
| 14..11, 25..23 | SENDS: chorus send 0..127: its bits 3..0 in 14..11, its bits 6..4 in 25..23 |
| 22 | RUN: the frames are at the replay's own rate (48 or 96 kHz), not 48 kHz |

Every other bit is 0.

| Op | Name | Meaning |
|---|---|---|
| 1 | REG | write the value to the register (0x000-0x0FF the first bank, 0x100-0x1FF the second) |
| 2 | PAN | the channel's soft pan (an extension: `src/hvopl3.hpp`) |
| 3 | SENDS | the channel's reverb and chorus sends (extension) |
| 4 | RESET | the chip to its power-on state (every register 0, OPL2 mode; extensions at their defaults) |
| 5 | RUN | make the next N frames (bits 21..6, 1..65535) at 48 kHz -- at 96 kHz, 2N; with bit 22, N at the replay's rate |
| 6 | GAIN | the channel's per-voice gain, in 0.75 dB steps, -32..+32 (extension) |

**Channels** are numbered as the registers number them: channel n (0..17) is the one whose
C0/A0/B0 registers are at `0xC0 + n % 9`, plus 0x100 for n >= 9. A 4-operator pair is named by its
first channel (0, 1, 2, 9, 10, 11).

**Time.** A chip's writes take effect at the sample after the RUNs before them: each chip has its
own timeline, the sum of its RUNs. Chips are independent; a replay mixes them by adding each
chip's samples at its own timeline position (the songs' four chips run in step, block by block).
The songs' runs are as short as 1 frame -- a MIDI event lands on its own sample -- and at most
256 (the player's block).

**What a RUN asks for**: the dry output (stereo) and, when the extensions are in use, the two send
buses (stereo each). A replay that has no buses ignores PAN/SENDS/GAIN, as the real chip would.

## HVO1, retired

HVO1 was the first format: the bench and `play` read it up to 0.10.0 (`play`'s `PLAY_HVO2` flag
chose HVO2), and 0.11.0 retired it. An HVO1 file is told by its magic, `0x314F5648` ("HVO1"); the
rest of its header is HVO2's. It addresses 4 chips: the chip is bits 5..4 alone, and bits 31..29 are 0. A SENDS record keeps its chorus send in bits 29..23 (so
bit 29 is the chorus's top bit, which HVO2 reads as a chip's), and PAN/SENDS/GAIN name the channel
with bits 14..6. Everything else is as above (RUN has no bit 22).

`tools/hvo1to2.py` converts a file: it moves each SENDS record's chorus send and writes the HVO2
magic; every other word is unchanged, and the replay gives the same samples. It refuses (writes
nothing) a file that is not a well-formed HVO1 trace: a bit HVO1 does not define, an unknown op,
a channel past 31, or a header that is not HVO1's. The bench's reader and tools refuse an HVO1 file
by its magic and name the converter.

The magic changed with the layout so that a reader cannot take one format for the other without a
sign: read as HVO2, an HVO1 file's chorus sends of 64 and above would land on chips 4..7.
