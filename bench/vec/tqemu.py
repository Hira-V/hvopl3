#!/usr/bin/env python3
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
"""tqemu.py SECONDS qemu-args...: runs qemu with a hard wall-clock limit (qemu ignores SIGALRM), prints its output."""
import subprocess, sys
try:
    r = subprocess.run(sys.argv[2:], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=float(sys.argv[1]))
    sys.stdout.write(r.stdout.decode(errors='replace'))
except subprocess.TimeoutExpired as e:
    sys.stdout.write((e.stdout or b'').decode(errors='replace')); print('TIMEOUT')
