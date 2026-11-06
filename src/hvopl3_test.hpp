// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// hvopl3_test.hpp -- for the bench only, not an API: the same engine at the chip's own rate (one tick a
// sample), to compare with data recorded at that rate (docs/DESIGN.md).
//
#pragma once
#include "hvopl3.hpp"
namespace hvopl3 {
void init_chip_rate(Chip &, uint32_t flags = 0);
// A flag for the tests: the mix's slow path always (it and the fast path give the same bits).
constexpr uint32_t MIX_SLOW = 1u << 30;
}
