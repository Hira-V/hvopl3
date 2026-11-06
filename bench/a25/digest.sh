#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# digest.sh [TRACE...] -- the samples, not just the counts: vec/digest.cpp built with the A25's flags
# (run.sh's, plus -ffp-contract=off) and run under the repo's rv32emu, so every Andes DSP instruction
# the chip uses is executed by the emulator the counts come from. Each trace's digests (48 kHz, 96 kHz,
# high quality, the dry output added to, unclipped: dry and buses) against the host scalar build. EXTRA= adds flags (EXTRA=-DHVOPL_NO_DSP:
# the A25's scalar path). Prints DIFF on a mismatch; exit 1 if any.
#
cd "$(dirname "$0")"
REPO=$(cd ../../.. && pwd)
ANDES=${ANDES:-$HOME/opt/andes-rv32imafdc}
CX=$ANDES/bin/riscv32-unknown-elf-g++
ISA=rv32i2p1_m2p0_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zca1p0_zcd1p0_zcf1p0_xandes5p0_xandescodense1p0_xandesperf1p0
FL="-mcpu=a25 -march=$ISA -mabi=ilp32d -mcmodel=medany -mext-dsp -O2 -w -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=gnu++20 -DHVOPL_TOUCH_FIRST -ffp-contract=off -ffreestanding -DHV_BARE -DHV_RV32EMU -DHV_MORE $EXTRA"
SRC=${SRC:-../../src}; TR=${TRACES:-../traces}; B=${BUILD:-build/digest}; mkdir -p $B   # SRC=, BUILD=: another source tree, build folder
c++ -std=gnu++20 -O2 -w -ffp-contract=off -DHV_MORE -I$SRC -I.. ../vec/digest.cpp $SRC/hvopl3.cpp -o $B/host || exit 1
$CX $FL -I$SRC -I.. -c ../vec/digest.cpp -o $B/digest.o && $CX $FL -I$SRC -I.. -c $SRC/hvopl3.cpp -o $B/hvopl3.o || exit 1
one() {
  t=$1; n=$(basename $t .hvo1); d=$(cd $(dirname $t) && pwd)/$(basename $t)
  printf '.section .rodata\n.balign 4\n.globl trace_bin\n.globl trace_end\ntrace_bin:\n.incbin "%s"\ntrace_end:\n' $d > $B/$n.S
  $CX $FL -nostdlib -nostartfiles -T link.ld -Wl,--no-relax ../../../emu/tests/crt0.S $B/$n.S $B/digest.o $B/hvopl3.o -o $B/$n.elf || { echo "$n NOLINK"; return; }
  e=$($REPO/emu/rv32emu --quiet --max-seconds ${EMU_SECS:-3000} $B/$n.elf 2>&1 | grep -E '^(48k|96k|hq|add|noclip|noclip.add|slow) ' | tr -d '\r' | tr '\n' ' ')
  h=$($B/host $t | tr '\n' ' ')
  if [ "$h" = "$e" ]; then echo "$n same $h"; else echo "$n DIFF host: $h a25: $e"; fi
}
rm -f $B/r.*; i=0
for t in ${*:-$(ls $TR/*.hvo1)}; do one $t > $B/r.$i 2>&1 & i=$((i+1)); [ $((i%14)) = 0 ] && wait; done; wait
cat $B/r.* | sort > $B/report.txt
cat $B/report.txt
! grep -q -E "DIFF|NOLINK" $B/report.txt
