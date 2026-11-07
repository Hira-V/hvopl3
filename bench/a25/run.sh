#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# run.sh CHIP [TRACE...] -- the A25 instructions a 48 kHz frame of CHIP (hvopl, hvopl_nodsp, hvopl96, hvoplhq, hvopl96hq) over
# each trace (default: all), under the repo's rv32emu, built with the Tang's flags (-mcpu=a25
# -mext-dsp -O2 -ffp-contract=off: the build digest.sh checks). Writes bench/results/CHIP.a25.txt. Needs the Andes
# GCC (ANDES=) and emu/rv32emu built. EXTRA=: more flags (a later -ffp-contract wins); TAG=: a name for that
# variant (build/CHIP.TAG, results CHIP.TAG.a25*.txt). Another chip: CHIPSRC= its sources (with a bench
# adapter), CHIPMK= the call that makes it, CHIPINC= its include flags.
#
set -e
cd "$(dirname "$0")"
CHIP=$1; shift
REPO=${HIRAV:-$(d=$(pwd); while [ "$d" != / ] && [ ! -x "$d/emu/rv32emu" ]; do d=$(dirname "$d"); done; echo "$d")}   # Hira-V: HIRAV=, or the nearest folder above with emu/rv32emu
ANDES=${ANDES:-$HOME/opt/andes-rv32imafdc}
CX=$ANDES/bin/riscv32-unknown-elf-g++
ISA=rv32i2p1_m2p0_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zca1p0_zcd1p0_zcf1p0_xandes5p0_xandescodense1p0_xandesperf1p0
FL="-mcpu=a25 -march=$ISA -mabi=ilp32d -mcmodel=medany -mext-dsp -O2 -w -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=gnu++20 -DHVOPL_TOUCH_FIRST -ffp-contract=off"
case $CHIP in
  hvopl) SRC="../adapter_hvopl3.cpp $(ls ../../src/*.cpp 2>/dev/null)"; MK='make_hvopl3(48000)';;
  hvopl_nodsp) SRC="../adapter_hvopl3.cpp $(ls ../../src/*.cpp 2>/dev/null)"; MK='make_hvopl3(48000)'; FL="$FL -DHVOPL_NO_DSP";;   # the scalar path
  hvopl96) SRC="../adapter_hvopl3.cpp $(ls ../../src/*.cpp 2>/dev/null)"; MK='make_hvopl3(96000)';;
  hvoplhq) SRC="../adapter_hvopl3.cpp $(ls ../../src/*.cpp 2>/dev/null)"; MK='make_hvopl3(48000,7)';;
  hvopl96hq) SRC="../adapter_hvopl3.cpp $(ls ../../src/*.cpp 2>/dev/null)"; MK='make_hvopl3(96000,7)';;
  *) [ -n "$CHIPSRC" ] || { echo "run.sh: chip?"; exit 1; }; SRC=$CHIPSRC; MK=$CHIPMK;;
esac
FL="$FL $EXTRA"; NAME=$CHIP${TAG:+.$TAG}
B=build/$NAME; mkdir -p $B ../results
OBJS=
for s in count.cpp $SRC; do o=$B/$(basename $s .cpp).o; $CX $FL "-DCHIP=$MK" -I.. $CHIPINC -I../../src -c $s -o $o; OBJS="$OBJS $o"; done
R=../results/$NAME.a25.txt; [ $# -gt 0 ] && R=../results/$NAME.a25.some.txt; : > $R
TRACES=${*:-$(ls ../traces/*.hvo1)}
for t in $TRACES; do
  n=$(basename $t .hvo1)
  printf '.section .rodata\n.balign 4\n.globl trace_bin\n.globl trace_end\ntrace_bin:\n.incbin "%s"\ntrace_end:\n' $(cd $(dirname $t) && pwd)/$(basename $t) > $B/$n.S
  $CX $FL -nostartfiles -T link.ld -Wl,--no-relax $REPO/emu/tests/crt0.S $B/$n.S $OBJS -o $B/$n.elf
  echo "$n $($REPO/emu/rv32emu --quiet --max-seconds 1800 $B/$n.elf 2>&1 | grep 'insns')" | tee -a $R
done
