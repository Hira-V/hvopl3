# hvopl3 -- A new FM synth modeled on the Yamaha OPL3, with accelerated paths for Hira-V
# Copyright (C) 2026 Kevin H. Patterson
# SPDX-License-Identifier: BSD-3-Clause (see LICENSE.txt)
# Hira-V: https://hira-v.com
#
# The renderer (host). make -C tools -> tools/build/render_opl3: a Standard MIDI File through the MIDI
# driver into a WAV file, built on render/ (the host hvyxg's render_yxg shares, with copies of Hira-V's
# UMP interface and MIDI file reader in render/lib/).
#
CXX      ?= c++
BUILD    ?= build
CXXFLAGS := -O2 -g -std=gnu++20 -Wall -Wextra -I. -Irender -I../src -I../drv $(EXTRA_FLAGS)
KIT      := render/render_host.cpp render/render_host.hpp render/lib/midifile.cpp render/lib/midifile.hpp render/lib/midi.hpp
all: $(BUILD)/render_opl3
$(BUILD)/render_opl3: render_opl3.cpp $(KIT) ../tests/song.hpp ../src/hvopl3.cpp ../src/hvopl3.hpp ../src/hvopl3_tables.inc ../drv/hvopl3_drv.cpp ../drv/hvopl3_drv.hpp ../drv/hvopl3_drv_tables.inc
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -ffp-contract=off render_opl3.cpp render/render_host.cpp render/lib/midifile.cpp ../src/hvopl3.cpp ../drv/hvopl3_drv.cpp -o $@
clean:
	rm -rf $(BUILD)
.PHONY: clean all
