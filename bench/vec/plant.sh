#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# plant.sh [TARGET] -- proves the high-quality digests can fail: each planted error in the RVV high-quality
# path (a copy of src/), built for TARGET (default x280) and run over a few short traces (HQ=1 run.sh), or
# through t_ext (t_ext.sh) for the ones a trace does not reach; every one must be caught. Exit 1 if one is missed.
# EXTRA=-DHVOPL_NO_RTZ: all of them in the build without rtz (the C920's v_trunc), on RVV 1.0 under qemu.
# PLANTS= how many plants at once (default 3), each with its own pool of JOBS= qemu runs (default 6: its traces).
# A run that times out is UNSURE, never caught.
#
cd "$(dirname "$0")"; T=$(mktemp -d); bad=0; TG=${1:-x280}
ONLY=; [ "$2" = --only ] && ONLY=$3   # --only K: plant K alone (internal: the pool's jobs)
i=0
TR=${TRACES:-../traces}
TRACES="$TR/s_waves.hvo2 $TR/s_feedback.hvo2 $TR/s_4op.hvo2 $TR/s_lfo.hvo2 $TR/s_env.hvo2 $TR/s_rhythm.hvo2"
try() { # name, file, sed expression[, t_ext[, more flags for this one]]
  i=$((i+1))
  if [ -z "$ONLY" ]; then echo $i >> $T/list; return; fi
  [ "$i" = "$ONLY" ] || return
  rm -rf $T/src; cp -R ../../src $T/src
  sed "$3" ../../src/$2 > $T/src/$2
  if cmp -s $T/src/$2 ../../src/$2; then echo "NOT APPLIED: $1"; bad=1; return; fi
  if [ "$4" = t_ext ]; then
    if SRC=$T/src BUILD=$T/build ./t_ext.sh $TG > $T/out 2>&1; then echo "MISSED: $1"; bad=1; else echo "caught: $1 (t_ext: $(grep -o '[0-9]* failed' $T/out))"; fi
    return
  fi
  if SRC=$T/src BUILD=$T/build HQ=1 QEMU_SECS=900 JOBS=${JOBS:-6} EXTRA="$EXTRA $5" ./run.sh $TG $TRACES > $T/out 2>&1; then echo "MISSED: $1"; bad=1
  elif grep -q TIMEOUT $T/out; then echo "UNSURE (a run timed out): $1"; bad=1
  else echo "caught: $1 ($(grep -c DIFF $T/out) of $(wc -l < $T/out | tr -d ' ') traces differ)"; fi
}
# Not planted, because they are no error: frac_cycle with a rounding conversion (x - 1 + 1 is exact for x in
# [1/2, 1)), and fabs for the scalar code's s < 0 ? -s : s (a -0 there is never heard). Both are kept as the
# scalar code has them all the same.
try "gain fused (vfmacc)" hvopl3_rvv.inc 's/__riscv_vfadd_vv_f32m1(FROM, __riscv_vfmul_vv_f32m1(SLOPE, D, vl), vl)/__riscv_vfmacc_vv_f32m1(FROM, SLOPE, D, vl)/'
try "output rounded to nearest" hvopl3_rvv.inc 's/v_scatter_i16(write, out, ROW, v_trunc(r, vl), vl)/v_scatter_i16(write, out, ROW, __riscv_vfcvt_x_f_v_i32m1(r, vl), vl)/'
try "gain distributed (w from + w slope d)" hvopl3_rvv.inc 's/const VF v = __riscv_vfmul_vv_f32m1(w, __riscv_vfadd_vv_f32m1(FROM, __riscv_vfmul_vv_f32m1(SLOPE, D, vl), vl), vl);/const VF v = __riscv_vfadd_vv_f32m1(__riscv_vfmul_vv_f32m1(w, FROM, vl), __riscv_vfmul_vv_f32m1(__riscv_vfmul_vv_f32m1(w, SLOPE, vl), D, vl), vl);/'
try "waveforms 4, 5 not 0 on the second half" hvopl3_rvv.inc 's/const VB m_zlo = __riscv_vmor_mm_b32(__riscv_vmseq_vx_u32m1_b32(WS, 1, vl), m_twice, vl);/const VB m_zlo = __riscv_vmseq_vx_u32m1_b32(WS, 1, vl);/'
try "feedback history not rotated at 96 kHz" hvopl3_rvv.inc 's/if (r96) { a = F1; bb = F3; }/if (r96) { a = F1; bb = F2; }/'
try "modulation not rounded" hvopl3_rvv.inc 's/M = __riscv_vfmul_vf_f32m1(hq_mod ? v : __riscv_vfcvt_f_x_v_f32m1(v_trunc(v, vl), vl), k.r1024, vl);/M = __riscv_vfmul_vf_f32m1(v, k.r1024, vl);/'
try "a quiet voice keeps its feedback" hvopl3_rvv.inc 's/if (quiet) c.hq_fb0\[ch\] = c.hq_fb1\[ch\] = c.hq_fb2\[ch\] = c.hq_fb3\[ch\] = 0.0f;/if (0) c.hq_fb0[ch] = 0.0f;/'
try "lanes kept over an LFO step" hvopl3.cpp '/if (pm != c.pm) {/{n;d;}'
try "lanes kept over a register write" hvopl3.cpp '/^void write(Chip &c, uint32_t reg, uint8_t v) {/{n;d;}' t_ext
# v_trunc without rtz: its step, its sign, its |x| left out
try "no-rtz truncation: not stepped down" hvopl3_rvv.inc 's/return __riscv_vsub_vx_i32m1_mu(__riscv_vmflt_vv_f32m1_b32(x, __riscv_vfcvt_f_x_v_f32m1(n, vl), vl), n, n, 1, vl);/return n;/' "" -DHVOPL_NO_RTZ
try "no-rtz truncation: the sign not put back" hvopl3_rvv.inc 's/return __riscv_vrsub_vx_i32m1_mu(__riscv_vmflt_vf_f32m1_b32(x, 0.0f, vl), n, n, 0, vl);/return n;/' "" -DHVOPL_NO_RTZ
try "no-rtz truncation: a signed value truncated as if positive" hvopl3_rvv.inc 's/const VI n = v_trunc_pos(__riscv_vfsgnjx_vv_f32m1(x, x, vl), vl);/const VI n = v_trunc_pos(x, vl);/' "" -DHVOPL_NO_RTZ
if [ -z "$ONLY" ]; then   # the pool: every plant its own job, the report in plant order
  xargs -P ${PLANTS:-3} -I{} sh -c 'sh ./plant.sh "$0" --only {} | sed "s/^/{} /"' $TG < $T/list > $T/all || bad=1
  sort -n $T/all | cut -d' ' -f2-
  [ $(grep -c "^[0-9]* caught" $T/all) = $(wc -l < $T/list | tr -d ' ') ] || bad=1
fi
rm -rf $T; exit $bad
