#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# t_ext.sh -- tests/t_ext.cpp (the extensions and the output contract) built with the A25's flags and run
# under the repo's rv32emu: the Andes DSP mix's every output case, null buses included, on the emulator
# the counts come from. EXTRA= adds flags (EXTRA=-DHVOPL_NO_DSP: the scalar path). Exit 1 on a failure.
#
cd "$(dirname "$0")"
REPO=${HIRAV:-$(d=$(pwd); while [ "$d" != / ] && [ ! -x "$d/emu/rv32emu" ]; do d=$(dirname "$d"); done; [ "$d" = / ] && d=$HOME/devel/hira-v; echo "$d")}   # Hira-V: HIRAV=, else the nearest folder above with emu/rv32emu, else ~/devel/hira-v
ANDES=${ANDES:-$HOME/opt/andes-rv32imafdc}
CX=$ANDES/bin/riscv32-unknown-elf-g++
ISA=rv32i2p1_m2p0_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zca1p0_zcd1p0_zcf1p0_xandes5p0_xandescodense1p0_xandesperf1p0
FL="-mcpu=a25 -march=$ISA -mabi=ilp32d -mcmodel=medany -mext-dsp -O2 -w -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=gnu++20 -DHVOPL_TOUCH_FIRST -ffp-contract=off $EXTRA"
B=build/t_ext; mkdir -p $B
$CX $FL -I../../src -nostartfiles -T link.ld -Wl,--no-relax $REPO/emu/tests/crt0.S sys.cpp ../../tests/t_ext.cpp ../../src/hvopl3.cpp -lm -o $B/t_ext.elf || exit 1
$REPO/emu/rv32emu --quiet --max-seconds 600 $B/t_ext.elf 2>&1 | tr -d '\r' | tee $B/out.txt | grep -E "FAIL|t_ext:"
grep -q "t_ext: [0-9]* checks, 0 failed" $B/out.txt
