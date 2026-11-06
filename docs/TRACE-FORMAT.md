# HVO1: the trace format

A trace is what a MIDI player told up to four OPL3 chips, in order, with the timing of every run
of samples between the writes: everything needed to make the same sound again, nothing about how.
Written by a recorder (a MIDI player's OPL3 writes) and by `bench/mktrace.py`;
read by `bench/hvo1.hpp`. Little-endian 32-bit words.

## Header (4 words)

| Word | |
|---|---|
| 0 | `0x314F5648` ("HVO1") |
| 1 | version, 1 |
| 2 | the rate the runs are counted at: 48000 |
| 3 | flags: bit 0 soft panning in use (PAN records carry real pans); bit 1 per-voice gains in use |

## Records (one word each)

| Bits | |
|---|---|
| 3..0 | op |
| 5..4 | chip, 0..3 |
| 14..6 | REG: the register, 0x000..0x1FF; PAN/SENDS/GAIN: the channel, 0..17; RUN: low bits of frames |
| 22..15 | REG: the value; PAN: the pan, 0..127; SENDS: reverb send 0..127; GAIN: the level, signed |
| 30..23 | SENDS: chorus send 0..127 |

| Op | Name | Meaning |
|---|---|---|
| 1 | REG | write the value to the register (0x000-0x0FF the first bank, 0x100-0x1FF the second) |
| 2 | PAN | the channel's soft pan (an extension: `src/hvopl3.hpp`) |
| 3 | SENDS | the channel's reverb and chorus sends (extension) |
| 4 | RESET | the chip to its power-on state (every register 0, OPL2 mode; extensions at their defaults) |
| 5 | RUN | make the next N frames (bits 21..6, 1..65535) at 48 kHz -- at 96 kHz, 2N |
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
