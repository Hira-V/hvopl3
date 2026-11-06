#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# plant.sh: proves t_ext can fail. Each planted error compiled directly (no make: macOS make's 1 s
# timestamps once gave a false green here) from a copy; every one must be caught.
#
cd "$(dirname "$0")/.."; T=$(mktemp -d); bad=0
try() { # name, sed expression
  sed "$2" src/hvopl3.cpp > $T/h.cpp
  if cmp -s $T/h.cpp src/hvopl3.cpp; then echo "NOT APPLIED: $1"; bad=1; return; fi
  cp src/hvopl3_tables.inc $T/
  c++ -std=gnu++20 -O1 -w -Isrc tests/t_ext.cpp $T/h.cpp -I$T -o $T/t || { echo "NO BUILD: $1"; bad=1; return; }
  if $T/t >$T/out 2>&1; then echo "MISSED: $1"; bad=1; else echo "caught: $1 ($(tail -1 $T/out); $(grep -m1 FAIL $T/out | cut -c1-70))"; fi
}
try "pan swapped" 's/const uint32_t pl = law ? PAN_L\[c.pan\[ch\]\] : 32768, pr = law ? PAN_R\[c.pan\[ch\]\] : 32768;/const uint32_t pl = law ? PAN_R[c.pan[ch]] : 32768, pr = law ? PAN_L[c.pan[ch]] : 32768;/'
try "4-op bits from the first channel" 's/const uint32_t oc = c.kind\[ch\] == K_4OP ? ch + 3 : ch;/const uint32_t oc = ch;/'
try "no clip" 's/const bool clip = !(c.flags \& NO_CLIP);/const bool clip = false;/'
try "gain one step off" 's/const uint32_t g = GAIN\[c.gain\[ch\] + 32\];/const uint32_t g = GAIN[c.gain[ch] + 33 > 64 ? 64 : c.gain[ch] + 33];/'
try "silent chip reports sound" 's/uint32_t res = 0, off = 0;/uint32_t res = 1, off = 0;/'
try "silent drums leak" 's/const int32_t hh = ah >= QUIET ? 0 : op_out/const int32_t hh = op_out/'
try "HQ drums not taken" 's/if (c.flags \& (HQ_PHASE | HQ_ENVELOPE)) k_rhythm_hq(c, at, row + done, row8 + done, seg);/if (0) k_rhythm_hq(c, at, row + done, row8 + done, seg);/'
try "HQ hi-hat out of the mix" 's/const float v7 = read(14, hihat_index(r)) \* gain_of(g\[0\]) + read(15, snare_index(r)) \* gain_of(g\[1\]);/const float v7 = read(15, snare_index(r)) * gain_of(g[1]);/'
try "HQ gain does not ramp" 's/for (uint32_t j = 0; j < 4; ++j) g\[j\].d += 1.0f;//'
try "HQ line redrawn at every segment" 's/if (next != c.hq_tick\[i\] || c.att\[i\] != c.hq_att\[i\]) {/if (true) {/'
# the mix (t_ext's t_mix holds it to its definition, computed there in 64 bits)
try "mix: the fast path past its bound" 's/return c.mix_bound == 1 || mix_bound(c, o.rev, o.cho, mask);/return true;/'
try "mix: the slow path without its high parts" 's/int32_t l = c.mix_hi\[0\]\[j\] + ((c.acc_l\[j\] + 2048) >> 12), r = c.mix_hi\[1\]\[j\] + ((c.acc_r\[j\] + 2048) >> 12);/int32_t l = (c.acc_l[j] + 2048) >> 12, r = (c.acc_r[j] + 2048) >> 12;/'
try "mix: the slow path's Q14 split" 's/return (hi >> 2) + ((((hi \& 3) << 12) + lo + 8192) >> 14);/return (hi >> 2) + ((lo + 8192) >> 14);/'
try "mix: buses rounded from Q12" 's/b\[2 \* j\] += (bl\[j\] + 8192) >> 14; b\[2 \* j + 1\] += (br\[j\] + 8192) >> 14;/b[2 * j] += (bl[j] + 2048) >> 12; b[2 * j + 1] += (br[j] + 2048) >> 12;/'
try "mix: dry not rounded" 's/int32_t l = (c.acc_l\[j\] + 2048) >> 12, r = (c.acc_r\[j\] + 2048) >> 12;/int32_t l = c.acc_l[j] >> 12, r = c.acc_r[j] >> 12;/'
try "mix: the cached bound never cleared" 's/#define HV_MIX_STALE(c) ((c).mix_bound = 0)/#define HV_MIX_STALE(c) ((void)0)/'
try "mix: combined send not rounded" 's/return (c \* q + 512) >> 10; }/return (c * q) >> 10; }/'
try "mix: a send not scaled" 's/if (o.rev \&\& c.rev_q\[ch\]) bus_sum(c, 0, bus, row, send_coef(cl, c.rev_q\[ch\]), send_coef(cr, c.rev_q\[ch\]), n);/if (o.rev \&\& c.rev_q[ch]) bus_sum(c, 0, bus, row, cl, cr, n);/'
# the version
try "version: the library's number not the header's" 's/uint32_t version() { return HVOPL3_VERSION_NUMBER; }/uint32_t version() { return HVOPL3_VERSION_NUMBER + 1; }/'
rm -rf $T; exit $bad
