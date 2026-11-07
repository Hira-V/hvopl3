#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# drv.sh [TARGET,...]: the MIDI driver's test song (bench/drv_digest.cpp: tests/song.hpp with DMXOPL3, in six
# set-ups) on each target under qemu-system, against the host build. Targets as run.sh's (x280, v128, t2,
# baby, rv64); c920: built only (no emulator runs RVV 0.7.1). Prints DIFF on a mismatch; exit 1 if any.
#
cd "$(dirname "$0")"
TARGETS=${1:-x280,v128,t2,baby,rv64,c920}
G=${GXX:-$HOME/opt/riscv64-elf-gcc15/bin/riscv64-unknown-elf-g++}
B=${BUILD:-../build/vec-drv}; mkdir -p $B
BANK=$(cd ../../drv/banks && pwd)/DMXOPL3-GS.wopl
BASE="-mcmodel=medany -ffp-contract=off -O2 -w -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffreestanding -DHV_BARE -I../../src -I../../drv -I.."
c++ -std=gnu++20 -O2 -w -ffp-contract=off -I../../src -I../../drv ../drv_digest.cpp ../../src/hvopl3.cpp ../../drv/hvopl3_drv.cpp -o $B/host || exit 1
H=$($B/host $BANK | tr '\n' ' ')
printf '.section .rodata\n.balign 4\n.globl wopl_bin\n.globl wopl_end\nwopl_bin:\n.incbin "%s"\nwopl_end:\n' $BANK > $B/bank.S
one() {
  tg=$1
  case $tg in
    x280) M="-march=rv64gcv -mabi=lp64d"; Q=qemu-system-riscv64; C="rv64,v=true,vlen=512,elen=64,vext_spec=v1.0";;
    v128) M="-march=rv64gcv -mabi=lp64d"; Q=qemu-system-riscv64; C="rv64,v=true,vlen=128,elen=64,vext_spec=v1.0";;
    rv64) M="-march=rv64gc -mabi=lp64d"; Q=qemu-system-riscv64; C="rv64,v=false";;
    t2)   M="-march=rv32imf_zve32x_zicsr -mabi=ilp32f"; Q=qemu-system-riscv32; C="rv32,c=false,v=true,vlen=128,elen=64,vext_spec=v1.0";;
    baby) M="-march=rv32imf_zicsr -mabi=ilp32f"; Q=qemu-system-riscv32; C="rv32,d=false,c=false,v=false";;
    c920) M="-march=rv64gc_xtheadvector -mabi=lp64d"; Q=;;
    *) echo "$tg: target?"; return;;
  esac
  mkdir -p $B/$tg
  for s in ../drv_digest.cpp ../../src/hvopl3.cpp ../../drv/hvopl3_drv.cpp; do $G $BASE $M -c $s -o $B/$tg/$(basename $s .cpp).o || { echo "$tg NOBUILD $s"; return; }; done
  $G $BASE $M -x assembler-with-cpp -c crt.S -o $B/$tg/crt.o && $G $M -c $B/bank.S -o $B/$tg/bank.o || { echo "$tg NOBUILD"; return; }
  $G $M -mcmodel=medany -nostdlib -nostartfiles -T virt.ld $B/$tg/crt.o $B/$tg/drv_digest.o $B/$tg/hvopl3.o $B/$tg/hvopl3_drv.o $B/$tg/bank.o -Wl,--no-warn-rwx-segments -o $B/$tg/drv.elf 2>$B/$tg/link.err || { echo "$tg NOLINK $(head -3 $B/$tg/link.err)"; return; }
  [ -z "$Q" ] && { echo "$tg built (no emulator)"; return; }
  q=$(python3 -I tqemu.py ${QEMU_SECS:-1200} $Q -machine virt -m 256M -nographic -bios none -cpu $C -kernel $B/$tg/drv.elf 2>&1 | tr -d '\r' | tr '\n' ' ')
  if [ "$H" = "$q" ]; then echo "$tg same $q"; else echo "$tg DIFF host: $H qemu: $q"; fi
}
for tg in $(echo $TARGETS | tr , ' '); do one $tg > $B/r.$tg 2>&1 & done; wait
cat $(for tg in $(echo $TARGETS | tr , ' '); do echo $B/r.$tg; done) | tee $B/report.txt
! grep -q -E "DIFF|NOLINK|NOBUILD" $B/report.txt
