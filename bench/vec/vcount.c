// hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
// Hira-V: https://hira-v.com
//
// vcount: a qemu TCG plugin counting the instructions executed in [lo, hi) (hvopl3's text), all of them and
// the vector ones (OP-V, and LOAD-FP/STORE-FP with a vector width). Args: lo=0x..,hi=0x...; prof=FILE also
// writes each instruction's count ("pc count vector", hex pc) for a profile by symbol.
//
#include <qemu-plugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
static uint64_t lo, hi;
struct counts { uint64_t all, vec; };
static struct qemu_plugin_scoreboard *sb;
static qemu_plugin_u64 all, vec;
static struct qemu_plugin_scoreboard *pb;   // prof: a count per halfword of [lo, hi)
static const char *prof;
static uint8_t *isvec;
static int is_vector(uint32_t w) {
	const uint32_t op = w & 0x7F, width = (w >> 12) & 7;
	if (op == 0x57) return 1;
	if (op == 0x07 || op == 0x27) return width == 0 || width >= 5;
	return 0;
}
static void trans(struct qemu_plugin_tb *tb, void *u) {
	(void)u;
	for (size_t i = 0, n = qemu_plugin_tb_n_insns(tb); i < n; ++i) {
		struct qemu_plugin_insn *in = qemu_plugin_tb_get_insn(tb, i);
		const uint64_t pc = qemu_plugin_insn_vaddr(in);
		if (pc < lo || pc >= hi) continue;
		uint32_t w = 0;
		qemu_plugin_insn_data(in, &w, sizeof w);
		qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, all, 1);
		const int v = (w & 3) == 3 && is_vector(w);
		if (v) qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, vec, 1);
		if (pb) {
			const qemu_plugin_u64 e = {pb, (size_t)(pc - lo) / 2 * 8};
			isvec[(pc - lo) / 2] = (uint8_t)(1 + v);
			qemu_plugin_register_vcpu_insn_exec_inline_per_vcpu(in, QEMU_PLUGIN_INLINE_ADD_U64, e, 1);
		}
	}
}
static void done(void *p) {
	(void)p;
	char b[128];
	snprintf(b, sizeof b, "hvopl3-text %" PRIu64 " vector %" PRIu64 "\n", qemu_plugin_u64_sum(all), qemu_plugin_u64_sum(vec));
	qemu_plugin_outs(b);
	if (!pb) return;
	FILE *f = fopen(prof, "w");
	if (!f) return;
	for (uint64_t i = 0; i < (hi - lo) / 2; ++i) {
		if (!isvec[i]) continue;
		const qemu_plugin_u64 e = {pb, (size_t)i * 8};
		const uint64_t n = qemu_plugin_u64_get(e, 0);
		if (n) fprintf(f, "%" PRIx64 " %" PRIu64 " %d\n", lo + 2 * i, n, isvec[i] - 1);
	}
	fclose(f);
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info, int argc, char **argv) {
	(void)info;
	for (int i = 0; i < argc; ++i) {
		if (!strncmp(argv[i], "lo=", 3)) lo = strtoull(argv[i] + 3, 0, 0);
		if (!strncmp(argv[i], "hi=", 3)) hi = strtoull(argv[i] + 3, 0, 0);
		if (!strncmp(argv[i], "prof=", 5)) prof = argv[i] + 5;
	}
	sb = qemu_plugin_scoreboard_new(sizeof(struct counts));
	all = qemu_plugin_scoreboard_u64_in_struct(sb, struct counts, all);
	vec = qemu_plugin_scoreboard_u64_in_struct(sb, struct counts, vec);
	if (prof && hi > lo) {
		pb = qemu_plugin_scoreboard_new((size_t)(hi - lo) / 2 * 8);
		isvec = calloc((size_t)(hi - lo) / 2, 1);
	}
	qemu_plugin_register_vcpu_tb_trans_cb(id, trans, 0);
	qemu_plugin_register_atexit_cb(id, done, 0);
	return 0;
}
