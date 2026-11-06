# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
CXX ?= c++
CXXFLAGS := -std=gnu++20 -O2 -g -Wall -Wextra -I../src
all: run
build/t_ext: t_ext.cpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc
	@mkdir -p build
	$(CXX) $(CXXFLAGS) t_ext.cpp ../src/hvopl3.cpp -o $@
run: build/t_ext
	build/t_ext
.PHONY: all run
