# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# The bench (host). make -j16 -> build/play, build/native (the renderer is tools/: make -C tools). EXTRA_SRC=, EXTRA_FLAGS=: more chips for
# play (each file defines make_extra), built into BUILD= (default build).
#
CXX      ?= c++
BUILD    ?= build
CXXFLAGS := -O2 -g -std=gnu++20 -Wall -Wextra -I. -I../src $(EXTRA_FLAGS)
SRC_HV   := $(wildcard ../src/*.cpp)
all: $(BUILD)/play $(BUILD)/native
$(BUILD)/play: ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc play.cpp hvo2.hpp chip.hpp adapter_hvopl3.cpp $(SRC_HV) $(EXTRA_SRC)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -w play.cpp adapter_hvopl3.cpp $(SRC_HV) $(EXTRA_SRC) -o $@
$(BUILD)/native: native.cpp hvo2.hpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_test.hpp ../src/hvopl3_tables.inc
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) native.cpp ../src/hvopl3.cpp -o $@
clean:
	rm -rf build
.PHONY: clean all
