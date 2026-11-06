#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
"""prof.py ELF PROF [NM]: run.sh PROF=1's counts by function (instructions, vector ones), largest first."""
import subprocess, sys, bisect, os
nm = sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser('~/opt/riscv64-elf-gcc15/bin/riscv64-unknown-elf-nm')
syms = []
for l in subprocess.run([nm, '-C', '--defined-only', '-n', sys.argv[1]], capture_output=True, text=True).stdout.splitlines():
    p = l.split(' ', 2)
    if len(p) == 3 and p[1] in 'tTwW': syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
tot = {}
for l in open(sys.argv[2]):
    pc, n, v = l.split(); pc = int(pc, 16); n = int(n)
    s = syms[bisect.bisect_right(addrs, pc) - 1][1]
    t = tot.setdefault(s, [0, 0]); t[0] += n; t[1] += n * int(v)
all_ = sum(t[0] for t in tot.values()) or 1
for s, (n, v) in sorted(tot.items(), key=lambda x: -x[1][0])[:25]:
    print(f'{100 * n / all_:5.1f}% {n:>13} vec {100 * v / max(n, 1):4.0f}%  {s[:110]}')
