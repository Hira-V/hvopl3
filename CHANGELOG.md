# Changelog

## 0.12.1 (2026-11-08)

- Documentation only: the README's section on `render_opl3`, prose wrapped at 100 columns, and
  this changelog. The code is unchanged from 0.12.0.

## 0.12.0 (2026-11-08)

- `render_opl3` (`tools/`, `make -C tools`): a Standard MIDI File through the MIDI driver to a WAV.
  It shares its host (`tools/render/`) and copies of Hira-V's UMP interface and MIDI file reader
  with [hvyxg](https://github.com/Hira-V/hvyxg)'s `render_yxg`.
- The chip's and the driver's samples are unchanged from 0.11.0.

## 0.11.0 (2026-11-06)

- HVO1 retired: HVO2 is the one trace format, and `play` reads it without a flag (`PLAY_HVO2` is
  gone). Old HVO1 files convert with `tools/hvo1to2.py`, to the same samples.
- The chip's samples are unchanged from 0.10.0.

## 0.10.0 (2026-11-06)

- The MIDI driver (`drv/`), the HVO2 trace format (up to 32 chips), `advance()` and `inaudible()`.

## 0.9.0 (2026-11-06)

- The first public release.
