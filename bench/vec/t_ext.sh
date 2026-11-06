#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# t_ext.sh TARGET[,TARGET..] -- tests/t_ext.cpp (the extensions, the output contract, the samples' independence
# of how a run is cut into calls, in every mode) built for a vector target (x280, v128: run.sh's) and run bare
# metal under qemu-system, newlib for printf and libm. SRC=: another source tree. Exit 1 on a failure.
#
cd "$(dirname "$0")"
G=${GXX:-$HOME/opt/riscv64-elf-gcc15/bin/riscv64-unknown-elf-g++}
SRC=${SRC:-../../src}; B=${BUILD:-../build/vec}; mkdir -p $B; bad=0
for tg in $(echo ${1:-x280,v128} | tr , ' '); do
  case $tg in
    x280) C="rv64,v=true,vlen=512,elen=64,vext_spec=v1.0";;
    v128) C="rv64,v=true,vlen=128,elen=64,vext_spec=v1.0";;
    *) echo "target?"; exit 1;;
  esac
  $G -march=rv64gcv -mabi=lp64d -mcmodel=medany -O2 -w -std=gnu++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics -ffp-contract=off \
    -I$SRC -nostartfiles -T virt.ld -Wl,--no-warn-rwx-segments -x assembler-with-cpp crt.S -x c++ sysq.cpp ../../tests/t_ext.cpp $SRC/hvopl3.cpp -lm -o $B/t_ext_$tg.elf || exit 1
  python3 -I tqemu.py ${QEMU_SECS:-900} qemu-system-riscv64 -machine virt -m 256M -nographic -bios none -cpu $C -kernel $B/t_ext_$tg.elf 2>&1 | tr -d '\r' > $B/t_ext_$tg.txt
  echo "$tg: $(grep -E 'FAIL|t_ext:|TIMEOUT' $B/t_ext_$tg.txt)"
  grep -q "t_ext: [0-9]* checks, 0 failed" $B/t_ext_$tg.txt || bad=1
done
exit $bad
