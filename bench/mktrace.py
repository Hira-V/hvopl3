#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
"""mktrace.py -- synthetic HVO1 traces, each exercising one part of the YMF262 (or of our
extensions). Written for this project from the chip's public register map.

    python3 -I mktrace.py OUTDIR          writes s_*.hvo1 (one chip, 48 kHz)

Each test is a function below; its docstring says what it checks. Add tests freely."""
import struct, sys, os

MAGIC, VERSION = 0x314F5648, 1
REG, PAN, SENDS, RESET, RUN, GAIN = 1, 2, 3, 4, 5, 6
RATE = 48000
# operator register offsets of each channel's two slots (channel 0..8 of a bank)
OP1 = [0, 1, 2, 8, 9, 10, 16, 17, 18]

class Trace:
    def __init__(self, flags=0):
        self.w = []; self.flags = flags
        self.rec(RESET, 0, 0, 0)
    def rec(self, op, chip, a, d, d2=0):
        self.w.append(op | chip << 4 | (a & 0x1FF) << 6 | (d & 0xFF) << 15 | (d2 & 0xFF) << 23)
    def reg(self, a, d): self.rec(REG, 0, a, d)
    def pan(self, ch, p): self.rec(PAN, 0, ch, p)
    def sends(self, ch, r, c): self.rec(SENDS, 0, ch, r, c)
    def gain(self, ch, lvl): self.rec(GAIN, 0, ch, lvl & 0xFF)
    def run(self, frames):
        while frames > 0:
            n = min(frames, 256); self.w.append(RUN | n << 6); frames -= n
    def secs(self, s): self.run(int(round(s * RATE)))
    def save(self, path):
        with open(path, 'wb') as f:
            f.write(struct.pack('<4I', MAGIC, VERSION, RATE, self.flags))
            f.write(struct.pack('<%dI' % len(self.w), *self.w))

def opl3_on(t):
    t.reg(0x105, 0x01)                     # NEW: OPL3 mode
    t.reg(0x104, 0x00)                     # no 4-op pairs

def bank(ch): return 0x100 if ch >= 9 else 0
def slot(ch, n): return bank(ch) + OP1[ch % 9] + (3 if n else 0)
def chreg(ch, base): return bank(ch) + base + ch % 9

def op(t, ch, n, mult=1, tl=0, ar=15, dr=0, sl=0, rr=15, ws=0, am=0, vib=0, egt=1, ksr=0, ksl=0):
    s = slot(ch, n)
    t.reg(0x20 + s, am << 7 | vib << 6 | egt << 5 | ksr << 4 | mult)
    t.reg(0x40 + s, ksl << 6 | tl)
    t.reg(0x60 + s, ar << 4 | dr)
    t.reg(0x80 + s, sl << 4 | rr)
    t.reg(0xE0 + s, ws)

def chan(t, ch, fb=0, cnt=0, out=0x30): t.reg(chreg(ch, 0xC0), out | fb << 1 | cnt)
def key(t, ch, block, fnum, on=1):
    t.reg(chreg(ch, 0xA0), fnum & 0xFF)
    t.reg(chreg(ch, 0xB0), on << 5 | block << 2 | fnum >> 8)

def sine_voice(t, ch=0, **kw):
    op(t, ch, 0, tl=63)                    # modulator silent
    op(t, ch, 1, **kw)
    chan(t, ch)

def s_pitch(t):
    """Pitch: a pure sine (modulator silent), every block 0..7 at three F-numbers, each held 0.3 s.
    Checks: the frequency of each note (f = fnum * 49716 * 2^(block-20) Hz for MULT 1), within 0.01%."""
    opl3_on(t); sine_voice(t)
    for block in range(8):
        for fnum in (0x158, 0x241, 0x3FF):
            key(t, 0, block, fnum); t.secs(0.3); key(t, 0, block, fnum, 0); t.secs(0.05)

def s_mult(t):
    """Frequency multiple: MULT 0..15 (0 is x0.5; 11 and 13 are x10 and x12, 14 is x15) on a sine."""
    opl3_on(t)
    for m in range(16):
        sine_voice(t, mult=m); key(t, 0, 3, 0x200); t.secs(0.25); key(t, 0, 3, 0x200, 0); t.secs(0.05)

def s_waves(t):
    """Waveforms 0..7 (OPL3), a carrier alone, 0.3 s each at A3-ish."""
    opl3_on(t)
    for ws in range(8):
        sine_voice(t, ws=ws); key(t, 0, 4, 0x244); t.secs(0.3); key(t, 0, 4, 0x244, 0); t.secs(0.05)

def s_env(t):
    """Envelopes: every attack rate 1..15 (decay 0, sustain), then decays 1..15 to SL 8, then
    releases 1..15 from full, each without and with KSR at block 6. Checks: timing and shape."""
    opl3_on(t)
    for ksr, rates in ((0, range(1, 16)), (1, range(6, 16, 3))):
        for ar in rates:
            sine_voice(t, ar=ar, dr=0, sl=0, rr=12, ksr=ksr); key(t, 0, 6, 0x200)
            t.secs({1: 3.0, 2: 1.5, 3: 0.8}.get(ar, 0.4)); key(t, 0, 6, 0x200, 0); t.secs(0.05)
        for dr in rates:
            sine_voice(t, ar=15, dr=dr, sl=8, rr=15, ksr=ksr, egt=1); key(t, 0, 6, 0x200)
            t.secs({1: 3.0, 2: 1.5, 3: 0.8}.get(dr, 0.4)); key(t, 0, 6, 0x200, 0); t.secs(0.02)
        for rr in rates:
            sine_voice(t, ar=15, rr=rr, ksr=ksr); key(t, 0, 6, 0x200); t.secs(0.05)
            key(t, 0, 6, 0x200, 0); t.secs({1: 3.0, 2: 1.5, 3: 0.8}.get(rr, 0.4))
    # percussive mode (EGT 0): decay then release without key-off
    sine_voice(t, ar=15, dr=6, sl=4, rr=6, egt=0); key(t, 0, 5, 0x200); t.secs(1.5); key(t, 0, 5, 0x200, 0); t.secs(0.1)

def s_ksl(t):
    """Key scale level 0..3 (0, 3, 1.5, 6 dB/oct) across blocks 0..7."""
    opl3_on(t)
    for ksl in range(4):
        for block in range(8):
            sine_voice(t, ksl=ksl); key(t, 0, block, 0x300); t.secs(0.15); key(t, 0, block, 0x300, 0); t.secs(0.02)

def s_feedback(t):
    """Feedback 0..7 on an audible modulator (additive connection, carrier silent)."""
    opl3_on(t)
    for fb in range(8):
        op(t, 0, 0, tl=0); op(t, 0, 1, tl=63); chan(t, 0, fb=fb, cnt=1)
        key(t, 0, 4, 0x200); t.secs(0.3); key(t, 0, 4, 0x200, 0); t.secs(0.05)

def s_fmdepth(t):
    """FM depth: modulator level 63 -> 0 in steps of 4 at C4, C5, C6, C7 (aliasing and accuracy of
    deep FM; the 96 kHz mode's quality test)."""
    opl3_on(t)
    for block, fnum in ((4, 0x157), (5, 0x157), (6, 0x157), (7, 0x157)):
        for tl in range(63, -1, -4):
            op(t, 0, 0, tl=tl, mult=1); op(t, 0, 1, tl=0, mult=1); chan(t, 0, fb=0, cnt=0)
            key(t, 0, block, fnum); t.secs(0.12); key(t, 0, block, fnum, 0); t.secs(0.01)

def s_lfo(t):
    """Tremolo and vibrato: AM and VIB bits with each depth setting (0xBD bits 7 and 6), 1 s each."""
    opl3_on(t)
    for depth in (0x00, 0x40, 0x80, 0xC0):
        t.reg(0xBD, depth)
        for am, vib in ((1, 0), (0, 1), (1, 1)):
            sine_voice(t, am=am, vib=vib); key(t, 0, 5, 0x200); t.secs(1.0); key(t, 0, 5, 0x200, 0); t.secs(0.05)
    t.reg(0xBD, 0)

PAIRS = [(0, 3), (1, 4), (2, 5), (9, 12), (10, 13), (11, 14)]   # 0x104 bits 0..5

def s_4op(t):
    """4-operator voices: each of the six pairs (0x104 bits 0..5), each of the four connections
    (CNT of the first and second channel: FM-FM, AM-FM, FM-AM, AM-AM), every operator audible,
    feedback on O1. The pair keyed, pitched and fed back by its FIRST channel alone."""
    opl3_on(t); t.reg(0x104, 0x3F)
    for n, (a, b) in enumerate(PAIRS):
        for c0, c3 in ((0, 0), (1, 0), (0, 1), (1, 1)):
            op(t, a, 0, tl=20, mult=1, ar=13, dr=3, sl=4, rr=7); op(t, a, 1, tl=14, mult=2, ar=14, dr=2, sl=3, rr=6)
            op(t, b, 0, tl=10, mult=3, ar=12, dr=4, sl=5, rr=8); op(t, b, 1, tl=0, mult=1, ar=15, dr=2, sl=2, rr=6)
            chan(t, a, fb=4, cnt=c0); chan(t, b, fb=0, cnt=c3)
            key(t, a, 4, 0x180 + 0x20 * n); t.secs(0.3); key(t, a, 4, 0x180 + 0x20 * n, 0); t.secs(0.08)
    t.reg(0x104, 0)

def s_4op_rules(t):
    """4-operator rules, each a short note:
      1. the second channel's F-number/block are ignored (set an octave away)
      2. the second channel's KON is ignored: keyed alone it sounds nothing; with the first, nothing more
      3. the second channel's FB is ignored; the first's applies to O1
      4. the output bits are the SECOND channel's: first left-only, second right-only -> right
      5. 0x104 cleared under a held pair: two 2-op channels from then on; set again: a pair"""
    opl3_on(t); t.reg(0x104, 0x01)
    def ops(tl=(0, 63, 63, 63)):
        op(t, 0, 0, tl=tl[0]); op(t, 0, 1, tl=tl[1]); op(t, 3, 0, tl=tl[2]); op(t, 3, 1, tl=tl[3])
    ops(); chan(t, 0, cnt=1); chan(t, 3, cnt=1)                         # AM-AM: O1 alone audible
    key(t, 3, 6, 0x100, 0); key(t, 0, 4, 0x200); t.secs(0.3); key(t, 0, 4, 0x200, 0); t.secs(0.1)   # 1
    ops((63, 63, 63, 0))                                                 # O4 alone audible
    key(t, 3, 4, 0x200); t.secs(0.3); key(t, 3, 4, 0x200, 0); t.secs(0.1)                       # 2: nothing
    key(t, 0, 4, 0x200); key(t, 3, 4, 0x200); t.secs(0.3); key(t, 0, 4, 0x200, 0); key(t, 3, 4, 0x200, 0); t.secs(0.1)
    ops(); chan(t, 0, fb=0, cnt=1); chan(t, 3, fb=7, cnt=1)
    key(t, 0, 4, 0x200); t.secs(0.3); key(t, 0, 4, 0x200, 0); t.secs(0.1)                       # 3
    chan(t, 0, fb=7, cnt=1); chan(t, 3, fb=0, cnt=1)
    key(t, 0, 4, 0x200); t.secs(0.3); key(t, 0, 4, 0x200, 0); t.secs(0.1)
    chan(t, 0, fb=0, cnt=1, out=0x10); chan(t, 3, fb=0, cnt=1, out=0x20)                      # 4
    for tl in ((0, 63, 63, 63), (63, 63, 63, 0)):
        ops(tl); key(t, 0, 4, 0x200); t.secs(0.3); key(t, 0, 4, 0x200, 0); t.secs(0.1)
    chan(t, 0, cnt=1); chan(t, 3, cnt=1); ops((0, 63, 63, 0))                                  # 5
    key(t, 0, 4, 0x200); key(t, 3, 5, 0x200); t.secs(0.3); t.reg(0x104, 0x00); t.secs(0.3)
    t.reg(0x104, 0x01); t.secs(0.3); key(t, 0, 4, 0x200, 0); key(t, 3, 5, 0x200, 0); t.secs(0.2)
    t.reg(0x104, 0)

def s_rhythm(t):
    """Rhythm mode (0xBD bit 5): bass drum, snare, tom, cymbal, hi-hat, alone and together."""
    opl3_on(t)
    op(t, 6, 0, tl=10, ar=15, dr=6, sl=15, rr=6, egt=0, mult=0); op(t, 6, 1, tl=0, ar=15, dr=6, sl=15, rr=6, egt=0)
    chan(t, 6, fb=4)
    op(t, 7, 0, tl=4, ar=15, dr=8, sl=15, rr=8, egt=0, mult=1)   # hi-hat
    op(t, 7, 1, tl=4, ar=15, dr=7, sl=15, rr=7, egt=0, mult=1)   # snare
    op(t, 8, 0, tl=4, ar=15, dr=6, sl=15, rr=6, egt=0, mult=2)   # tom
    op(t, 8, 1, tl=4, ar=15, dr=5, sl=15, rr=5, egt=0, mult=1)   # cymbal
    chan(t, 7); chan(t, 8)
    key(t, 6, 2, 0x200, 0); key(t, 7, 3, 0x200, 0); key(t, 8, 3, 0x1C0, 0)
    for bits in (0x10, 0x08, 0x04, 0x02, 0x01, 0x1F, 0x11, 0x09):
        t.reg(0xBD, 0x20); t.run(64); t.reg(0xBD, 0x20 | bits); t.secs(0.4)
    t.reg(0xBD, 0x00)

def s_opl2(t):
    """OPL2 compatibility (0x105 = 0): waveforms 0..7 written with the waveform-enable bit (0x01
    bit 5) on and off; the output on both sides; C0's output bits ignored."""
    t.reg(0x105, 0x00)
    for wse in (0x20, 0x00):
        t.reg(0x01, wse)
        for ws in range(8):
            sine_voice(t, ws=ws); key(t, 0, 4, 0x244); t.secs(0.2); key(t, 0, 4, 0x244, 0); t.secs(0.03)

def s_rekey(t):
    """Key-on again in attack, in decay and in release; a note too short for its attack; a key-off
    and key-on in the same sample."""
    opl3_on(t)
    sine_voice(t, ar=6, dr=4, sl=6, rr=5)
    for hold, gap in ((0.02, 0.2), (0.15, 0.05), (0.4, 0.15), (0.005, 0.3)):
        key(t, 0, 5, 0x200); t.secs(hold); key(t, 0, 5, 0x200, 0); t.secs(gap)
    key(t, 0, 5, 0x200); t.secs(0.3)
    for _ in range(4):
        key(t, 0, 5, 0x200, 0); key(t, 0, 5, 0x200); t.secs(0.2)
    key(t, 0, 5, 0x200, 0); t.secs(0.5)

def s_stereo(t):
    """OUR EXTENSIONS (no chip has them): soft pan 0..127 on a
    held note (the C0 bits both on), then reverb and chorus sends 0/64/127, then per-voice gain
    -32..32 (0.75 dB steps)."""
    opl3_on(t); sine_voice(t, ch=0); sine_voice(t, ch=9)
    key(t, 0, 4, 0x244); key(t, 9, 5, 0x244)
    for p in range(0, 128, 8):
        t.pan(0, p); t.pan(9, 127 - p); t.secs(0.1)
    t.pan(0, 64); t.pan(9, 64)
    for r, c in ((0, 0), (127, 0), (0, 127), (64, 64), (127, 127)):
        t.sends(0, r, c); t.sends(9, c, r); t.secs(0.2)
    for g in range(-32, 33, 4):
        t.gain(0, g); t.gain(9, -g); t.secs(0.08)
    key(t, 0, 4, 0x244, 0); key(t, 9, 5, 0x244, 0); t.secs(0.3)

def s_full(t):
    """Worst case for speed: all 18 channels deep FM with feedback, held, plus a slow LFO (2 s)."""
    opl3_on(t); t.reg(0xBD, 0xC0)
    for ch in range(18):
        op(t, ch, 0, tl=6, mult=1 + ch % 4, am=1, vib=1); op(t, ch, 1, tl=2, mult=1, vib=1); chan(t, ch, fb=6)
        key(t, ch, 3 + ch % 4, 0x180 + 16 * ch)
    t.secs(2.0)

def s_idle(t):
    """Best case for speed: OPL3 on, every channel keyed off and silent (2 s)."""
    opl3_on(t); t.secs(2.0)

def s_rates(t):
    """Envelope rates whose low two bits are 0, 1 and 2 (s_env's are nearly all 3): attack, decay and
    release at rates 1..14 x 4 + 0..2, set by KSR with a low block and F-number (key codes 0, 1, 2) and a
    high MULT to keep the tone audible. Checks the rate patterns."""
    opl3_on(t)
    span = lambda r: min(4.0, 12.0 / 2 ** (r - 1) + 0.05)
    codes = ((0, 0x100), (0, 0x300), (1, 0x100))     # key code = block x 2 + F-number bit 9
    for block, fnum in codes:
        for r in (1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 13, 14):
            sine_voice(t, mult=15, ar=r, dr=0, sl=0, rr=15, ksr=1); key(t, 0, block, fnum); t.secs(span(r)); key(t, 0, block, fnum, 0); t.secs(0.03)
            sine_voice(t, mult=15, ar=15, dr=r, sl=8, rr=15, ksr=1); key(t, 0, block, fnum); t.secs(span(r)); key(t, 0, block, fnum, 0); t.secs(0.03)
            sine_voice(t, mult=15, ar=15, rr=r, ksr=1); key(t, 0, block, fnum); t.secs(0.03); key(t, 0, block, fnum, 0); t.secs(span(r))

def s_patch(t):
    """Three voices taken from a song where the level differs from the chip by over 1 dB: deep feedback,
    tremolo and vibrato bits, waveforms 1 and 2, slow attacks, low blocks. Each alone, keyed 1.2 s, released."""
    opl3_on(t)
    def raw(ch, n, r20, r40, r60, r80, ws):
        s = slot(ch, n); t.reg(0x20 + s, r20); t.reg(0x40 + s, r40); t.reg(0x60 + s, r60); t.reg(0x80 + s, r80); t.reg(0xE0 + s, ws)
    for a, b, fb in (((0x84, 0x9D, 0x90, 0x52, 2), (0x41, 0x94, 0x43, 0x53, 1), 6),
                     ((0x82, 0x9D, 0x90, 0x52, 2), (0x41, 0x94, 0x43, 0x53, 0), 6),
                     ((0x21, 0x98, 0xA1, 0x45, 2), (0x21, 0x20, 0xA1, 0x26, 1), 6)):
        for block, fnum in ((2, 0x364), (3, 0x243)):
            raw(0, 0, *a); raw(0, 1, *b); chan(t, 0, fb=fb)
            key(t, 0, block, fnum); t.secs(1.2); key(t, 0, block, fnum, 0); t.secs(0.8)

TESTS = [s_pitch, s_mult, s_waves, s_env, s_ksl, s_feedback, s_fmdepth, s_lfo, s_4op, s_4op_rules, s_rhythm,
         s_opl2, s_rekey, s_stereo, s_full, s_idle, s_rates, s_patch]

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    for f in TESTS:
        t = Trace(flags=1 if f is s_stereo else 0)
        f(t); t.run(256)
        t.save(os.path.join(out, f.__name__ + '.hvo1'))
        frames = sum((w >> 6) & 0xFFFF for w in t.w if w & 15 == RUN)
        print('%-12s %6.1f s  %s' % (f.__name__, frames / RATE, f.__doc__.split('\n')[0]))
