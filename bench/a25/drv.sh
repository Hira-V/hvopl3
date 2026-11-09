#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# drv.sh -- the MIDI driver on the A25, under the repo's rv32emu (the Tang's flags, as digest.sh): the test
# song's digests (bench/drv_digest.cpp) against the host build, then the instructions a 48 kHz frame spent
# in the driver alone and in the chips' render (its calls counted apart: HVOPL3_DRV_CHIP_RENDER). EXTRA=:
# more flags. EVENTS=FILE (tools/build/render_opl3 SONG.mid OUT.wav --dump FILE): also count that song's first
# 8 seconds. Prints DIFF on a mismatch; exit 1 if any.
#
cd "$(dirname "$0")"
REPO=${HIRAV:-$(d=$(pwd); while [ "$d" != / ] && [ ! -x "$d/emu/rv32emu" ]; do d=$(dirname "$d"); done; [ "$d" = / ] && d=$HOME/devel/hira-v; echo "$d")}   # Hira-V: HIRAV=, else the nearest folder above with emu/rv32emu, else ~/devel/hira-v
ANDES=${ANDES:-$HOME/opt/andes-rv32imafdc}
CX=$ANDES/bin/riscv32-unknown-elf-g++
ISA=rv32i2p1_m2p0_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0_zca1p0_zcd1p0_zcf1p0_xandes5p0_xandescodense1p0_xandesperf1p0
FL="-mcpu=a25 -march=$ISA -mabi=ilp32d -mcmodel=medany -mext-dsp -O2 -w -fno-exceptions -fno-rtti -fno-threadsafe-statics -std=gnu++20 -DHVOPL_TOUCH_FIRST -ffp-contract=off -ffreestanding -DHV_BARE -DHV_RV32EMU -I../../src -I../../drv -I.. $EXTRA"
B=${BUILD:-build/drv}; mkdir -p $B
BANK=$(cd ../../drv/banks && pwd)/DMXOPL3-GS.wopl
printf '.section .rodata\n.balign 4\n.globl wopl_bin\n.globl wopl_end\nwopl_bin:\n.incbin "%s"\nwopl_end:\n' $BANK > $B/bank.S
c++ -std=gnu++20 -O2 -w -ffp-contract=off -I../../src -I../../drv ../drv_digest.cpp ../../src/hvopl3.cpp ../../drv/hvopl3_drv.cpp -o $B/host || exit 1
build() { # name, extra flags
  for s in ../drv_digest.cpp ../../src/hvopl3.cpp ../../drv/hvopl3_drv.cpp; do $CX $FL $2 -c $s -o $B/$1.$(basename $s .cpp).o || exit 1; done
  $CX $FL -nostdlib -nostartfiles -T link.ld -Wl,--no-relax $REPO/emu/tests/crt0.S $B/bank.S $B/$1.drv_digest.o $B/$1.hvopl3.o $B/$1.hvopl3_drv.o -o $B/$1.elf || exit 1
}
build digest ""
build count "-DHV_COUNT -DHVOPL3_DRV_CHIP_RENDER=bench_chip_render"
h=$($B/host $BANK | tr '\n' ' ')
e=$($REPO/emu/rv32emu --quiet --max-seconds ${EMU_SECS:-3000} $B/digest.elf 2>&1 | tr -d '\r' | grep -E '^(1chip|4chips|stream|96k|hq|hard|events) ' | tr '\n' ' ')
if if [ -n "$EVENTS" ]; then
  printf '.section .rodata\n.balign 4\n.globl events_bin\n.globl events_end\nevents_bin:\n.incbin "%s"\nevents_end:\n' $(cd $(dirname $EVENTS) && pwd)/$(basename $EVENTS) > $B/events.S
  $CX $FL -DHV_COUNT -DHV_EVENTS -DHVOPL3_DRV_CHIP_RENDER=bench_chip_render -c ../drv_digest.cpp -o $B/ev.drv_digest.o || exit 1
  $CX $FL -nostdlib -nostartfiles -T link.ld -Wl,--no-relax $REPO/emu/tests/crt0.S $B/bank.S $B/events.S $B/ev.drv_digest.o $B/count.hvopl3.o $B/count.hvopl3_drv.o -o $B/ev.elf || exit 1
  echo "$(basename $EVENTS):"; $REPO/emu/rv32emu --quiet --max-seconds ${EMU_SECS:-3000} $B/ev.elf 2>&1 | tr -d '\r' | grep insns
fi
[ "$h" = "$e" ]; then echo "a25 same $e"; else echo "a25 DIFF host: $h a25: $e"; fi
$REPO/emu/rv32emu --quiet --max-seconds ${EMU_SECS:-3000} $B/count.elf 2>&1 | tr -d '\r' | grep insns
if [ -n "$EVENTS" ]; then
  printf '.section .rodata\n.balign 4\n.globl events_bin\n.globl events_end\nevents_bin:\n.incbin "%s"\nevents_end:\n' $(cd $(dirname $EVENTS) && pwd)/$(basename $EVENTS) > $B/events.S
  $CX $FL -DHV_COUNT -DHV_EVENTS -DHVOPL3_DRV_CHIP_RENDER=bench_chip_render -c ../drv_digest.cpp -o $B/ev.drv_digest.o || exit 1
  $CX $FL -nostdlib -nostartfiles -T link.ld -Wl,--no-relax $REPO/emu/tests/crt0.S $B/bank.S $B/events.S $B/ev.drv_digest.o $B/count.hvopl3.o $B/count.hvopl3_drv.o -o $B/ev.elf || exit 1
  echo "$(basename $EVENTS):"; $REPO/emu/rv32emu --quiet --max-seconds ${EMU_SECS:-3000} $B/ev.elf 2>&1 | tr -d '\r' | grep insns
fi
[ "$h" = "$e" ]
