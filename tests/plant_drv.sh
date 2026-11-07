#!/bin/sh
# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# plant_drv.sh: proves t_drv can fail. Each planted error in a copy of drv/hvopl3_drv.cpp, compiled
# directly (no make); every one must be caught. T_DRV_QUICK: the exhaustive hirav sums left out.
#
cd "$(dirname "$0")/.."; T=$(mktemp -d); bad=0
try() { # name, sed expression
  sed "$2" drv/hvopl3_drv.cpp > $T/d.cpp
  if cmp -s $T/d.cpp drv/hvopl3_drv.cpp; then echo "NOT APPLIED: $1"; bad=1; return; fi
  cp drv/hvopl3_drv_tables.inc $T/
  c++ -std=gnu++20 -O2 -w -ffp-contract=off -DT_DRV_QUICK -Isrc -Idrv -I$T tests/t_drv.cpp src/hvopl3.cpp $T/d.cpp -o $T/t || { echo "NO BUILD: $1"; bad=1; return; }
  if (cd tests && $T/t >$T/out 2>&1); then echo "MISSED: $1"; bad=1; else echo "caught: $1 ($(tail -1 $T/out); $(grep -m1 FAIL $T/out | cut -c1-70))"; fi
}
try "re-strike ignored" 's/if (d.cfg.restrike)$/if (false)/'
try "sostenuto takes every note" 's/if (d.slot\[x\].state == S_ON) d.slot\[x\].flags |= F_SOST;/if (d.slot[x].state != S_FREE) d.slot[x].flags |= F_SOST;/'
try "hirav truncated, not rounded" 's/+ (1ull << 31);/+ 0;/'
try "linear curve rounded down" 's|return (63 \* (16129 - p) + 8064) / 16129;|return (63 * (16129 - p)) / 16129 + 1;|'
try "F-number truncated" 's/uint32_t f = (m + (1u << (sh - 1))) >> sh;/uint32_t f = m >> sh;/'
try "detune off by one" 's/- 64) \* 128;/- 63) * 128;/'
try "a note of this period stolen" 's/if (held \&\& s.start == d.tick) continue;/(void)0;/'
try "no pedal preference" 's/key = int32_t(s.level) + ((s.flags \& F_OFF) ? 24 : 0) + int32_t(age > 16 ? 16 : age) - (c ? 32 : 0);/key = int32_t(s.level) + int32_t(age > 16 ? 16 : age) - (c ? 32 : 0);/'
try "WOPL operators in file order" 's/static const uint8_t FROM\[4\] = {1, 0, 3, 2};/static const uint8_t FROM[4] = {0, 1, 2, 3};/'
try "XG's melodic rule outside XG" 's/else if (c.percussion \&\& (d.xg || c.xg_kit))/else if (c.percussion)/'
try "vibrato half as deep" 's/(depth \* 2048 + 63) \/ 127/(depth * 1024 + 63) \/ 127/'
try "no level correction" 's/s.corr = P->gain;/s.corr = 0;/'
try "released voices never freed, silent or not" 's/if (!(chip.active >> ch \& 1)) make_free(d, g);/if (false) make_free(d, g);/; s/else if (inaudible(chip, ch)) {/else if (false) {/'
try "96 kHz runs counted at 48 kHz" 's/const uint32_t native = d.cfg.rate == RATE_96K ? 1u << 22 : 0;/const uint32_t native = 0;/'
try "no release cap" 's/if (!(s.flags \& F_CUT)) { key_off(d, g, true); s.flags |= F_CUT; }/(void)s;/'
try "bend twice its range" 's/\* range) >> 13);/* range) >> 12);/'
try "hard pan: the centre to one side" 's/return pan <= 42 ? 0x10 : pan >= 86 ? 0x20 : 0x30;/return pan <= 64 ? 0x10 : 0x20;/'
try "4-op carriers by the wrong connection" 's/constexpr uint8_t HEARD\[4\] = {0x8, 0x9, 0xA, 0xD};/constexpr uint8_t HEARD[4] = {0x8, 0xA, 0x9, 0xD};/'
try "a stream block ends anywhere" 's/if (d.cfg.stream \&\& d.rec_cap - d.rec_n < block_reserve(d, frames)) { stop = at; break; }/(void)0;/'
try "the control period at the block, not the frame" 's/const bool tick = tk < frames \&\& tk <= ev;/const bool tick = tk < frames \&\& (tk == 0 || i >= d.qn);/'
try "percussion kit from bank select" 's/find_bank(\*d.pool, true, 0, c.program)/find_bank(*d.pool, true, c.msb, c.program)/'
try "GS rhythm part numbering" 's/e.b = uint8_t(x == 0 ? 9 : x <= 9 ? x - 1 : x);/e.b = uint8_t(x);/'
try "shadow not advanced in stream mode" 's/\t\tadvance(c, n);/\t\t(void)c;/'
try "update: levels not applied again" '/if (d.pending \& P_LEVELS)$/{n;s/each_level(d, i);/(void)i;/;}'
try "update: always a full reset" 's/(flags \& ~LIVE) || (flags \&\& n.stream)) {/true) {/'
try "update: hq flags taken in stream mode" 's/(flags \& ~LIVE) || (flags \&\& n.stream)) {/(flags \& ~LIVE)) {/'
try "returning: never" 's/if (g < 0 \&\& d.cfg.returning \&\& /if (false \&\& /'
try "returning: a cut voice too" 's/|| s.patch != P || (s.flags \& F_CUT)) continue;/|| s.patch != P) continue;/'
try "returning: one pseudo half alone" 's/if (want == R_PSEUDO1 \&\& (h0 < 0 || h1 < 0)) return -1;/(void)0;/'
try "inaudible voices not freed" 's/else if (inaudible(chip, ch)) {/else if (false) {/'
try "inaudible voices freed uncut" 's/if ((d.slot\[g\].rr != 15 || !(d.slot\[g\].flags \& F_CUT))) key_off(d, g, true);/(void)0;/'
try "models: scaling always rounded down" 's/return rnd == 0 ? a \/ b : rnd == 1/return rnd != 9 ? a \/ b : rnd == 1/'
try "models: the adding models a step quiet" 's/default: { const uint32_t t = tl + M.kv\[v\] + M.kc\[c\];/default: { const uint32_t t = tl + M.kv[v] + M.kc[c] + 1;/'
try "models: the offset family without its offset" 's/const int32_t t = 63 - int32_t(((63 - tl) \* (k \& 255) + (k >> 8)) \/ 64);/const int32_t t = 63 - int32_t(((63 - tl) * (k \& 255)) \/ 64);/'
try "models: fractions of 63 rounded down" 's/av + ac : 63 - ((63 - av) \* (63 - ac) + 31) \/ 63;/av + ac : 63 - ((63 - av) * (63 - ac)) \/ 63;/'
try "models: factors multiplied, rounded down" 's/M.kc\[127\] ? M.kc\[127\] : 1, 2));/M.kc[127] ? M.kc[127] : 1, 0));/'
try "models: attenuations not added" 's/default: { const uint32_t t = tl + M.kv\[v\] + M.kc\[c\];/default: { const uint32_t t = tl + M.kv[v];/'
try "models: the product index of the volume alone" 's/c == 127 ? M.kv\[v\] : M.kc\[v \* c \/ 127\]/c == 127 ? M.kv[v] : M.kc[c]/'
try "models: APOGEE first operator as the second" 's/if (first \&\& M.op1 == 1) {/if (false) {/'
try "models: HMI_OLD first operator scaled" 's/if (first \&\& M.op1 == 2) return tl;/(void)0;/'
try "models: master volume left out" 's/return (vol \* expr \/ 127) \* master \/ 127; }/return vol * expr \/ 127; (void)master; }/'
try "GS drums: a melodic channel takes them" 's/if (!c.percussion || c.nrpn_lsb > 127) return;/if (c.nrpn_lsb > 127) return;/'
try "GS drums: a sounding voice not updated" 's/apply_level(d, x); apply_pan(d, x); apply_sends(d, x);/(void)x;/'
try "GS drums: pan not about the channel's" 's/? int32_t(r->pan) + c.pan - 64 : c.pan;/? int32_t(r->pan) : c.pan;/'
try "GS drums: level left out" 's/return r ? (c.expression \* r->level + 63) \/ 127 : c.expression;/return c.expression; (void)r;/'
try "GS drums: pitch left out" 's/(s.drum ? d.drums\[ch.drum_map\]\[note \& 127\].pitch : 0)/0/'
try "GS drums: one map for both" 's/c.drum_map = e.c == 2 ? 1 : 0; /c.drum_map = 0; /'
try "GS drums: kept over a reset" 's/for (auto \&r : map) { r.pitch = 0;/for (auto \&r : map) if (false) { r.pitch = 0;/'
try "GS drums: Reset All Controllers keeps the NRPN" 's/c.nrpn = 0; c.nrpn_msb = c.nrpn_lsb = 127;$/c.nrpn = c.nrpn;/'
try "models: GENERIC's attenuations scaled, not added" 's/const uint32_t a = M.comb == 4 ? av + ac :/const uint32_t a = M.comb == 9 ? av + ac :/'
try "models: NATIVE not a model (the default curve)" 's/return curve >= CURVE_DMX \&\& curve <= CURVE_NATIVE; }/return curve >= CURVE_DMX \&\& curve <= CURVE_GENERIC; }/'
rm -rf $T; exit $bad
