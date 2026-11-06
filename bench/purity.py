#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
"""purity.py OUT.wav -- s_fmdepth's harmonic purity: for each note (blocks 4..7, modulator level 63
down to 0 in steps of 4; mktrace.py), the energy at the harmonics of the note over everything else,
both below 20 kHz, in dB (higher is cleaner: aliasing and rounding noise are "everything else").
Prints each block's mean over the deep half (modulator level <= 31) and the overall mean. Works at
48 or 96 kHz (the 96 kHz mode's quality gate)."""
import sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
from compare import load

def purity(x, rate):
    t, res = 0.0, {}
    for block in (4, 5, 6, 7):
        f0 = 0x157 * (14318180.0 / 288.0) * 2.0 ** (block - 20)
        vals = []
        for tl in range(63, -1, -4):
            s, e = int((t + 0.03) * rate), int((t + 0.115) * rate)
            m = x[s:e].sum(axis=1)
            N = 1 << 18
            p = np.abs(np.fft.rfft(m * np.hanning(len(m)), N)) ** 2
            f = np.fft.rfftfreq(N, 1 / rate)
            aud = f < 20000
            harm = np.zeros_like(aud)
            for k in range(1, int(20000 / f0) + 1):
                harm |= np.abs(f - k * f0) < 3 * rate / len(m)
            h = p[aud & harm].sum(); o = p[aud & ~harm].sum()
            if tl <= 31 and h > 0: vals.append(10 * np.log10(h / max(o, 1e-12)))
            t += 0.13
        res[block] = float(np.mean(vals))
    return res

if __name__ == '__main__':
    x, r = load(sys.argv[1])
    res = purity(x, r)
    print('purity (dB, deep FM): ' + '  '.join('block %d %5.1f' % (b, v) for b, v in res.items()) + '  mean %5.1f' % np.mean(list(res.values())))
