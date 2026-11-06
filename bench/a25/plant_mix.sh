#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# plant_mix.sh -- proves digest.sh can fail on the A25's mix: each planted error in its passes (a copy of
# src/) against the host's scalar build (the definition), over a few traces. Every one must be caught.
#
cd "$(dirname "$0")"; T=$(mktemp -d); bad=0
TR=${TRACES:-../traces}
TRACES="$TR/s_stereo.hvo1 $TR/s_full.hvo1 $TR/s_rhythm.hvo1 $TR/s_4op.hvo1 $TR/b_nes_netopia_mix.hvo1"   # the song: many voices send
try() { # name, sed expression (on hvopl3_dsp.inc)
  rm -rf $T/src; cp -R ../../src $T/src
  sed "$2" ../../src/hvopl3_dsp.inc > $T/src/hvopl3_dsp.inc
  if cmp -s $T/src/hvopl3_dsp.inc ../../src/hvopl3_dsp.inc; then echo "NOT APPLIED: $1"; bad=1; return; fi
  SRC=$T/src BUILD=$T/build sh ./digest.sh $TRACES > $T/out 2>&1
  d=$(grep -c " DIFF " $T/out); a=$(grep -c -E " (same|DIFF) " $T/out)
  if [ "$a" = 0 ]; then echo "UNSURE (no build or run): $1"; bad=1
  elif [ "$d" = 0 ]; then echo "MISSED: $1"; bad=1
  else echo "caught: $1 ($d of $a traces differ)"; fi
}
try "pairs packed the wrong way round" 's/p\[q\] = dsp_pkbb(uint32_t(x\[(2 \* q + 1) \* BLOCK + j\]), uint32_t(x\[2 \* q \* BLOCK + j\]));/p[q] = dsp_pkbb(uint32_t(x[2 * q * BLOCK + j]), uint32_t(x[(2 * q + 1) * BLOCK + j]));/'
try "buses given out twice" 's/b.bus = out ? b.bus \& ~want : b.bus | want;/b.bus = b.bus | want;/'
try "an odd row's partner heard" 's/const bool two = 2 \* k + 1 < nr;/const bool two = true;/'
try "the dry output not rounded" 's/int32_t dl = dsp_clip16((l + 2048) >> 12), dr = dsp_clip16((r + 2048) >> 12);/int32_t dl = dsp_clip16(l >> 12), dr = dsp_clip16(r >> 12);/'
try "the dry sums read before they begin" 's/b.any ? c.acc_l : ZERO_SUMS, b.any ? c.acc_r : ZERO_SUMS/c.acc_l, c.acc_r/'
try "a bus's sums read before they begin" 's/const int32_t u = rx_dot<NP>(BF ? 0 : b0\[j\], p, k.vl), v = rx_dot<NP>(BF ? 0 : b1\[j\], p, k.vr);/const int32_t u = rx_dot<NP>(b0[j], p, k.vl), v = rx_dot<NP>(b1[j], p, k.vr);/'
try "the only row's dry scaled as Q13" 's/wl = cl << 3, wr = cr << 3;/wl = cl << 2, wr = cr << 3;/'
try "the only row's sends scaled as Q15" 's/const int32_t vl = send_coef(cl, q) << 1,/const int32_t vl = send_coef(cl, q) << 2,/'
rm -rf $T; exit $bad
