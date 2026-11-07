# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
CXX ?= c++
CXXFLAGS := -std=gnu++20 -O2 -g -Wall -Wextra -I../src
DRV := ../drv/hvopl3_drv.cpp ../drv/hvopl3_drv.hpp ../drv/hvopl3_drv_tables.inc ../drv/hvopl3_drv_models.inc models_measured.inc models_combined.inc
all: run
build/t_ext: t_ext.cpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc
	@mkdir -p build
	$(CXX) $(CXXFLAGS) t_ext.cpp ../src/hvopl3.cpp -o $@
build/t_drv: t_drv.cpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc $(DRV)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffp-contract=off -I../drv t_drv.cpp ../src/hvopl3.cpp ../drv/hvopl3_drv.cpp -o $@
# the same, under the address and undefined-behaviour sanitizers (the WOPL fuzz's overreads)
build/t_drv_asan: t_drv.cpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc $(DRV)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all -DT_DRV_QUICK -I../drv t_drv.cpp ../src/hvopl3.cpp ../drv/hvopl3_drv.cpp -o $@
# built with a maximum of 4 chips (HVOPL3_DRV_MAX_CHIPS): the option, and the clamp
build/t_drv4: t_drv.cpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc $(DRV)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffp-contract=off -DT_DRV_QUICK -DHVOPL3_DRV_MAX_CHIPS=4 -I../drv t_drv.cpp ../src/hvopl3.cpp ../drv/hvopl3_drv.cpp -o $@
run: build/t_ext build/t_drv build/t_drv_asan build/t_drv4
	build/t_ext
	build/t_drv
	build/t_drv_asan
	build/t_drv4
.PHONY: all run
