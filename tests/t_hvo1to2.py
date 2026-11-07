#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# t_hvo1to2.py: tools/hvo1to2.py, run as a program. A well-formed HVO1 file becomes the HVO2 words
# written out here by hand; every malformed file is refused with exit status 1, a message, and no
# output file. (That the bench replays a converted trace to the same samples is shown on the real
# traces: docs/TESTING.md.)
#
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, '..', 'tools', 'hvo1to2.py')
HVO1, HVO2 = 0x314F5648, 0x324F5648
checks = fails = 0


def check(ok, what):
    global checks, fails
    checks += 1
    if not ok:
        fails += 1
        print('FAIL t_hvo1to2: ' + what)


def words(*w):
    return struct.pack('<%dI' % len(w), *w)


def run(d, data, name='in.hvo1'):
    src, dst = os.path.join(d, name), os.path.join(d, 'out.hvo2')
    if os.path.exists(dst):
        os.remove(dst)
    with open(src, 'wb') as f:
        f.write(data)
    p = subprocess.run([sys.executable, '-I', TOOL, src, dst], capture_output=True, text=True)
    out = open(dst, 'rb').read() if os.path.exists(dst) else None
    return p.returncode, p.stderr, out


# Every op, chips 0..3, the chorus send's every bit (100 = 0b1100100 sets HVO1's bit 29, 127 all seven).
GOOD_IN = [HVO1, 1, 48000, 3,
           4 | 3 << 4,                                   # RESET chip 3
           1 | 2 << 4 | 0x1A5 << 6 | 0xC3 << 15,         # REG chip 2: 0x1A5 = 0xC3
           2 | 1 << 4 | 17 << 6 | 127 << 15,             # PAN chip 1, channel 17, pan 127
           3 | 3 << 4 | 9 << 6 | 90 << 15 | 100 << 23,   # SENDS chip 3, channel 9, reverb 90, chorus 100
           3 | 0 << 4 | 17 << 6 | 127 << 15 | 127 << 23, # SENDS chip 0, channel 17, 127 and 127
           3 | 1 << 4 | 2 << 6 | 5 << 15 | 15 << 23,     # SENDS chorus 15: only its low four bits
           6 | 2 << 4 | 3 << 6 | 0xE0 << 15,             # GAIN chip 2, channel 3, level -32
           5 | 3 << 4 | 65535 << 6]                      # RUN chip 3, 65535 frames
GOOD_OUT = [HVO2, 1, 48000, 3,
            GOOD_IN[4], GOOD_IN[5], GOOD_IN[6],
            3 | 3 << 4 | 9 << 6 | 90 << 15 | (100 & 15) << 11 | (100 >> 4) << 23,
            3 | 0 << 4 | 17 << 6 | 127 << 15 | 15 << 11 | 7 << 23,
            3 | 1 << 4 | 2 << 6 | 5 << 15 | 15 << 11,
            GOOD_IN[10], GOOD_IN[11]]

BAD = [
    ('empty file', b''),
    ('a partial word', words(*GOOD_IN) + b'\x01'),
    ('short header', words(HVO1, 1, 48000)),
    ('already HVO2', words(*GOOD_OUT)),
    ('another magic', words(0x12345678, 1, 48000, 0)),
    ('version 2', words(HVO1, 2, 48000, 0)),
    ('rate 44100', words(HVO1, 1, 44100, 0)),
    ('unknown header flag', words(HVO1, 1, 48000, 4)),
    ('op 0', words(HVO1, 1, 48000, 0, 0)),
    ('op 7', words(HVO1, 1, 48000, 0, 7)),
    ('a chip high bit (31)', words(HVO1, 1, 48000, 0, 4 | 1 << 31)),
    ('REG bit 23', words(HVO1, 1, 48000, 0, 1 | 1 << 23)),
    ('RUN bit 22', words(HVO1, 1, 48000, 0, 5 | 10 << 6 | 1 << 22)),
    ('RESET with a register', words(HVO1, 1, 48000, 0, 4 | 5 << 6)),
    ('PAN channel 32', words(HVO1, 1, 48000, 0, 2 | 32 << 6)),
    ('GAIN bit 23', words(HVO1, 1, 48000, 0, 6 | 1 << 23)),
    ('SENDS bit 30 (chorus past 127)', words(HVO1, 1, 48000, 0, 3 | 1 << 30)),
    ('SENDS channel 32', words(HVO1, 1, 48000, 0, 3 | 32 << 6)),
    ('a bad record after good ones', words(*GOOD_IN) + words(1 | 1 << 31)),
]

# what the message must say, where the reason is not just "a bad bit"
WHY = {'already HVO2': 'already HVO2', 'another magic': 'not an HVO1 trace', 'op 7': 'unknown op',
       'a bad record after good ones': 'record 8', 'rate 44100': 'rate 44100'}

with tempfile.TemporaryDirectory() as d:
    rc, err, out = run(d, words(*GOOD_IN))
    check(rc == 0, 'a good HVO1 file: exit %d (%s)' % (rc, err.strip()))
    check(out == words(*GOOD_OUT), 'a good HVO1 file: the HVO2 words')
    rc, err, out = run(d, words(HVO1, 1, 48000, 0))
    check(rc == 0 and out == words(HVO2, 1, 48000, 0), 'a header alone: exit %d' % rc)
    for what, data in BAD:
        rc, err, out = run(d, data)
        check(rc == 1, '%s: exit %d, want 1' % (what, rc))
        check(out is None, '%s: an output file was written' % what)
        check('nothing written' in err, '%s: no message (%r)' % (what, err))
        check(what not in WHY or WHY[what] in err, '%s: the message does not say %r (%r)' % (what, WHY.get(what), err))
    # the default output name, and an output that would replace the input
    src = os.path.join(d, 'song.hvo1')
    open(src, 'wb').write(words(*GOOD_IN))
    rc = subprocess.run([sys.executable, '-I', TOOL, src], capture_output=True).returncode
    dflt = os.path.join(d, 'song.hvo2')
    check(rc == 0 and os.path.exists(dflt) and open(dflt, 'rb').read() == words(*GOOD_OUT), 'IN.hvo1 -> IN.hvo2 by default')
    rc = subprocess.run([sys.executable, '-I', TOOL, src, src], capture_output=True).returncode
    check(rc == 2 and open(src, 'rb').read() == words(*GOOD_IN), 'the input is never replaced: exit %d' % rc)

print('t_hvo1to2: %d checks, %d failed' % (checks, fails))
sys.exit(fails != 0)
