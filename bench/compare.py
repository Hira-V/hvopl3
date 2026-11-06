#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
"""compare.py -- how close one output is to another (numpy).

    python3 -I compare.py CAND.wav GOLD.wav [--pitch]

Prints one line: lag, gain, SNR, spectral distance, envelope distance, level. Definitions (lower
distance is closer):
  lag/gain   the best alignment within +-64 frames and the least-squares gain of CAND onto GOLD
  SNR        GOLD against GOLD - gain*CAND, dB (sample accuracy; FM syntheses differ in phase, so
             this is low between any two different chips: read the distances first)
  spectral   1/3-octave band levels (44 Hz up) in 2048-frame Hann windows at 48 kHz (4096 at 96),
             mean |dB| difference over windows within 40 dB of the loudest and bands within 40 dB of
             the window's total -- tone, timbre and aliasing
  envelope   10 ms RMS level, mean |dB| difference over frames within 60 dB of the loudest -- timing
             of attacks, decays, releases, tremolo
  level      RMS of CAND over RMS of GOLD, dB, before the gain (the output scale; 0 is the target)
--pitch: also s_pitch's note frequencies against the chip's formula (max error in cents, notes > 60 Hz)."""
import sys, struct
import numpy as np

def load(path):
    d = open(path, 'rb').read()
    fmt, ch, rate = struct.unpack('<HHI', d[20:28])
    bits = struct.unpack('<H', d[34:36])[0]
    i = 12
    while d[i:i + 4] != b'data':
        i += 8 + struct.unpack('<I', d[i + 4:i + 8])[0]
    n = struct.unpack('<I', d[i + 4:i + 8])[0]
    raw = d[i + 8:i + 8 + n]
    x = np.frombuffer(raw, dtype=np.float32 if fmt == 3 else (np.int32 if bits == 32 else np.int16)).astype(np.float64)
    return x.reshape(-1, ch), rate

def align(a, b, maxlag=64):
    n = min(len(a), len(b)) - 2 * maxlag
    A = a[maxlag:maxlag + n]
    best = (-np.inf, 0, 0.0)
    for lag in range(-maxlag, maxlag + 1):
        B = b[maxlag + lag:maxlag + lag + n]
        ab = float((A * B).sum()); bb = float((B * B).sum())
        if bb == 0: continue
        s = ab / np.sqrt(bb)
        if s > best[0]: best = (s, lag, ab / bb)
    return best[1], best[2]

def bands(x, rate):
    W = 2048 * rate // 48000
    m = x.sum(axis=1)
    edges = [44.2]
    while edges[-1] * 2 ** (1 / 3) < 20000: edges.append(edges[-1] * 2 ** (1 / 3))
    f = np.fft.rfftfreq(W, 1 / rate)
    idx = [np.where((f >= edges[q]) & (f < edges[q + 1]))[0] for q in range(len(edges) - 1)]
    win = np.hanning(W)
    nw = (len(m) - W) // W
    out = np.empty((nw, len(idx)))
    for w in range(nw):
        p = np.abs(np.fft.rfft(m[w * W:(w + 1) * W] * win)) ** 2
        out[w] = [p[i].sum() + 1e-3 for i in idx]
    return out

def spectral(a, b, rate):
    A, B = bands(a, rate), bands(b, rate)
    n = min(len(A), len(B)); A, B = A[:n], B[:n]
    tot = A.sum(axis=1); keep = tot >= tot.max() * 1e-4
    s, c = 0.0, 0
    for w in np.where(keep)[0]:
        sel = (A[w] >= tot[w] * 1e-4) | (B[w] >= tot[w] * 1e-4)
        s += np.abs(10 * np.log10(A[w][sel] / B[w][sel])).sum(); c += sel.sum()
    return s / c if c else float('nan')

def envelope(a, b, rate):
    F = rate // 100
    def rms(x):
        m = x.sum(axis=1); n = len(m) // F
        return np.sqrt((m[:n * F].reshape(n, F) ** 2).mean(axis=1) + 1e-6)
    A, B = rms(a), rms(b); n = min(len(A), len(B)); A, B = A[:n], B[:n]
    keep = A >= A.max() * 1e-3
    return float(np.abs(20 * np.log10(A[keep] / np.maximum(B[keep], 1e-6))).mean()) if keep.any() else float('nan')

def pitch(c, rate):
    # s_pitch's schedule (mktrace.py): blocks 0..7 x fnums, 0.3 s on, 0.05 s off
    worst, t = 0.0, 0.0
    for block in range(8):
        for fnum in (0x158, 0x241, 0x3FF):
            f0 = fnum * (14318180.0 / 288.0) * 2.0 ** (block - 20)
            s, e = int((t + 0.05) * rate), int((t + 0.28) * rate)
            seg = c[s:e].sum(axis=1)
            if f0 > 60 and len(seg) > 1000:
                N = 1 << 20
                spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), N))
                k = int(np.argmax(spec[1:])) + 1
                y0, y1, y2 = np.log(spec[k - 1:k + 2] + 1e-12)
                k += 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2)
                cents = abs(1200 * np.log2((k * rate / N) / f0))
                worst = max(worst, cents)
            t += 0.35
    return worst

def main():
    if len(sys.argv) < 3: print(__doc__); sys.exit(1)
    c, rc = load(sys.argv[1]); g, rg = load(sys.argv[2])
    if rc != rg: sys.exit('rates differ: %d %d' % (rc, rg))
    lag, gain = align(g, c)
    n = min(len(g), len(c)) - 128
    G = g[64:64 + n]; C = c[64 + lag:64 + lag + n] * gain
    snr = 10 * np.log10((G ** 2).sum() / max(((G - C) ** 2).sum(), 1e-9))
    cs = np.roll(c, -lag, axis=0) * gain
    lvl = 20 * np.log10(np.sqrt((c ** 2).mean()) / max(np.sqrt((g ** 2).mean()), 1e-9)) if (g ** 2).mean() > 0 else float('nan')
    line = 'lag %3d  gain %6.3f  SNR %6.1f dB  spectral %5.2f dB  envelope %5.2f dB  level %+6.2f dB' % (
        lag, gain, snr, spectral(g, cs, rg), envelope(g, cs, rg), lvl)
    if '--pitch' in sys.argv: line += '  pitch max %.2f cents' % pitch(c, rc)
    print(line)

if __name__ == '__main__':
    main()
