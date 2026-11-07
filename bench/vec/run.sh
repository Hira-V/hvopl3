#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# run.sh TARGET[,TARGET..] [TRACE...]: each trace's chip-mode digests at 48 and 96 kHz (dry, buses) on
# a target under qemu-system against the host scalar build. Prints DIFF on any mismatch; exit 1 if any.
# Targets: x280 (RVV 1.0, VLEN 512), v128 (RVV 1.0, VLEN 128), t2 (RV32 Zve32x, VLEN 128: Tensix T2),
# baby (RV32IMF: the Tensix cores), rv64 (RV64GC, scalar). All link -nostdlib: nothing from a C library
# or libgcc. The C920 (RVV 0.7.1) has no emulator: build it with -march=rv64gc_xtheadvector and run it on a board.
# COUNT=1: instructions a frame instead (qemu -icount; a vector instruction counts 1), 48 kHz only;
# FLAGS= the flags of that count (default 0); with qemu's plugins, also the instructions executed in hvopl3's
# text and how many were vector (vcount.c); PROF=1: each instruction's count too, for prof.py. HQ=1: the digests of every high-quality flag set too.
# JOBS= how many qemu runs at once (default: the host's cores); a pool, so a long trace never holds up the rest.
#
cd "$(dirname "$0")"
ONE=; [ "$1" = --one ] && { ONE=1; shift; }   # --one TRACE TARGET: one run, for the pool (internal)
TARGETS=${1:-x280,v128,t2,baby,rv64}; shift
G=${GXX:-$HOME/opt/riscv64-elf-gcc15/bin/riscv64-unknown-elf-g++}
SRC=${SRC:-../../src}; TR=${TRACES:-../traces}; B=${BUILD:-../build/vec}; mkdir -p $B   # SRC=, BUILD=: another source tree, build folder
X=; [ -n "$HQ" ] && X=-DHV_HQ
BASE="-mcmodel=medany -ffp-contract=off -O2 -w -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffreestanding -DHV_BARE -I$SRC -I.."
[ -n "$COUNT" ] && BASE="$BASE -DHV_COUNT -DHV_FLAGS=${FLAGS:-0}"
BASE="$BASE $X $EXTRA"   # EXTRA=: more flags for the targets
cfg() { case $1 in
  x280) M="-march=rv64gcv -mabi=lp64d"; Q=qemu-system-riscv64; C="rv64,v=true,vlen=512,elen=64,vext_spec=v1.0";;
  v128) M="-march=rv64gcv -mabi=lp64d"; Q=qemu-system-riscv64; C="rv64,v=true,vlen=128,elen=64,vext_spec=v1.0";;
  rv64) M="-march=rv64gc -mabi=lp64d"; Q=qemu-system-riscv64; C="rv64,v=false";;
  t2)   M="-march=rv32imf_zve32x_zicsr -mabi=ilp32f"; Q=qemu-system-riscv32; C="rv32,c=false,v=true,vlen=128,elen=64,vext_spec=v1.0";;
  baby) M="-march=rv32imf_zicsr -mabi=ilp32f"; Q=qemu-system-riscv32; C="rv32,d=false,c=false,v=false";;
  *) echo "target?"; exit 1;;
esac; }
one() {
  t=$1; tg=$2; cfg $tg; n=$(basename $t .hvo2); d=$(cd $(dirname $t) && pwd)/$(basename $t)
  printf '.section .rodata\n.balign 4\n.globl trace_bin\n.globl trace_end\ntrace_bin:\n.incbin "%s"\ntrace_end:\n' $d > $B/$tg/$n.S
  $G $M -mcmodel=medany -nostdlib -nostartfiles -T virt.ld $B/$tg/crt.o $B/$tg/digest.o $B/$tg/hvopl3.o $B/$tg/$n.S -Wl,--no-warn-rwx-segments -o $B/$tg/$n.elf 2>$B/$tg/$n.err || { echo "$n $tg NOLINK $(head -2 $B/$tg/$n.err)"; return; }
  ic=; [ -n "$COUNT" ] && ic="-icount shift=0"
  if [ -n "$COUNT" ] && [ -f $B/vcount.dylib ]; then
    lo=$(${G%g++}nm $B/$tg/$n.elf | awk '$3=="__hv_lo"{print $1}'); hi=$(${G%g++}nm $B/$tg/$n.elf | awk '$3=="__hv_hi"{print $1}')
    pf=; [ -n "$PROF" ] && pf=",prof=$B/$tg/$n.prof"
    ic="$ic -d plugin -plugin $B/vcount.dylib,lo=0x$lo,hi=0x$hi$pf"
  fi
  q=$(python3 -I tqemu.py ${QEMU_SECS:-600} $Q -machine virt -m 256M -nographic -bios none -cpu $C $ic -kernel $B/$tg/$n.elf 2>&1 | tr -d '\r' | tr '\n' ' ')
  if [ -n "$COUNT" ]; then echo "$n $tg $q"; return; fi
  h=$($B/host $t | tr '\n' ' ')
  if [ "$h" = "$q" ]; then echo "$n $tg same $h"; else echo "$n $tg DIFF host: $h qemu: $q"; fi
}
if [ -n "$ONE" ]; then one $TARGETS $1 > $B/r.$(basename $TARGETS .hvo2).$1 2>/dev/null; exit 0; fi
TRACES=${*:-$(ls $TR/*.hvo2)}
c++ -std=gnu++20 -O2 -w -ffp-contract=off $X -I$SRC -I.. digest.cpp $SRC/hvopl3.cpp -o $B/host || exit 1
QI=$(dirname $(dirname $(command -v qemu-system-riscv64)))/include
[ -n "$COUNT" ] && [ -f $QI/qemu-plugin.h ] && cc -O2 -shared -fPIC -I$QI $(pkg-config --cflags glib-2.0 2>/dev/null) -undefined dynamic_lookup vcount.c -o $B/vcount.dylib
for tg in $(echo $TARGETS | tr , ' '); do
  cfg $tg; mkdir -p $B/$tg
  $G $BASE $M -c digest.cpp -o $B/$tg/digest.o && $G $BASE $M -c $SRC/hvopl3.cpp -o $B/$tg/hvopl3.o && $G $BASE $M -x assembler-with-cpp -c crt.S -o $B/$tg/crt.o || exit 1
done
rm -f $B/r.*
for t in $TRACES; do for tg in $(echo $TARGETS | tr , ' '); do echo "$t $tg"; done; done |
  xargs -n 2 -P ${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)} sh ./run.sh --one
cat $B/r.* | sort > $B/report.txt
cat $B/report.txt
! grep -q -E "DIFF|NOLINK" $B/report.txt
