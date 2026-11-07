#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
"""hvo1to2.py -- convert an HVO1 trace (the format retired in hvopl3 0.11.0) to HVO2.

    python3 -I tools/hvo1to2.py IN.hvo1 [OUT.hvo2]     OUT defaults to IN with the suffix .hvo2

The HVO2 file replays to the same samples: the header gets the HVO2 magic (version, rate and flags
kept); a SENDS record's chorus send moves from bits 29..23 to 14..11 and 25..23; every other record
is the same word. docs/TRACE-FORMAT.md has both layouts.

Strict: HVO1 says every bit it does not define is 0, and a bit that is not 0 would mean something
else in HVO2 (a chip's high bits, a run's rate, a channel's high bits). So the file is refused, and
nothing written, if it is not a whole number of words, if its header is not HVO1's (version 1, 48000,
flags bits 0 and 1 only), or if any record has an unknown op, a bit HVO1 does not define, or a
channel past 31 (HVO2 has 5 bits for it; the chip has 18 channels). Values inside the defined fields
are copied as they are (both formats read them alike). Exit status 0 on success, 1 on a refused
file, 2 on a usage error."""
import os, struct, sys

HVO1, HVO2, VERSION, RATE = 0x314F5648, 0x324F5648, 1, 48000
REG, PAN, SENDS, RESET, RUN, GAIN = 1, 2, 3, 4, 5, 6
# per op, the bits an HVO1 record may set: op 3..0 and chip 5..4, then the op's own fields
CHIP = 0x3F
ALLOWED = {
    REG:   CHIP | 0x1FF << 6 | 0xFF << 15,   # register 14..6, value 22..15
    PAN:   CHIP | 0x1F << 6 | 0xFF << 15,    # channel (0..31 fits HVO2's 10..6), pan 22..15
    GAIN:  CHIP | 0x1F << 6 | 0xFF << 15,    # channel, level 22..15
    SENDS: CHIP | 0x1F << 6 | 0xFF << 15 | 0x7F << 23,   # channel, reverb 22..15, chorus 29..23
    RESET: CHIP,
    RUN:   CHIP | 0xFFFF << 6,               # frames 21..6 (bit 22 is HVO2's own-rate bit)
}


class Refused(Exception):
    pass


def convert(data):
    """HVO1 bytes -> HVO2 bytes; raises Refused with the reason."""
    if len(data) % 4:
        raise Refused('%d bytes: not a whole number of 32-bit words' % len(data))
    w = list(struct.unpack('<%dI' % (len(data) // 4), data))
    if len(w) < 4:
        raise Refused('%d words: shorter than the 4-word header' % len(w))
    if w[0] == HVO2:
        raise Refused('already HVO2')
    if w[0] != HVO1:
        raise Refused('not an HVO1 trace (magic 0x%08X)' % w[0])
    if w[1] != VERSION or w[2] != RATE:
        raise Refused('header: version %d, rate %d; HVO1 is version 1 at 48000' % (w[1], w[2]))
    if w[3] & ~3:
        raise Refused('header: flags 0x%X; HVO1 defines bits 0 and 1 only' % w[3])
    out = [HVO2, w[1], w[2], w[3]]
    for i, r in enumerate(w[4:]):
        op = r & 15
        if op not in ALLOWED:
            raise Refused('record %d (word %d): unknown op %d (0x%08X)' % (i, i + 4, op, r))
        if r & ~ALLOWED[op]:
            raise Refused('record %d (word %d): bits 0x%08X set that HVO1 does not define for op %d (0x%08X)'
                          % (i, i + 4, r & ~ALLOWED[op], op, r))
        if op == SENDS:
            chorus = r >> 23 & 0x7F
            r = (r & ~(0x7F << 23)) | (chorus & 15) << 11 | (chorus >> 4) << 23
        out.append(r)
    return struct.pack('<%dI' % len(out), *out)


def main(argv):
    if len(argv) not in (2, 3) or argv[1].startswith('-'):
        sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
        return 2
    src = argv[1]
    dst = argv[2] if len(argv) == 3 else os.path.splitext(src)[0] + '.hvo2'
    if os.path.abspath(dst) == os.path.abspath(src):
        sys.stderr.write('hvo1to2: %s: the output would replace the input\n' % src)
        return 2
    try:
        with open(src, 'rb') as f:
            data = convert(f.read())
    except Refused as e:
        sys.stderr.write('hvo1to2: %s: %s; nothing written\n' % (src, e))
        return 1
    except OSError as e:
        sys.stderr.write('hvo1to2: %s\n' % e)
        return 1
    tmp = dst + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, dst)
    print('hvo1to2: %s -> %s, %d records' % (src, dst, len(data) // 4 - 4))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
