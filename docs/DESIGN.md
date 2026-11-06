# hvopl3: design

Written before the first line of the synthesizer; kept current.

## The shape: three stages per block

1. **Plan** (scalar, every target): time, the LFO, envelope events; which voices sound, and with
   which kernel. Cost follows what happens, not how long it lasts.
2. **Synthesize** (kernels; scalar, or RVV across samples, or lanes across voices): one voice's
   segment into a row of a mono **matrix** (voices x frames).
3. **Mix** (one kernel): that matrix times a coefficient matrix (voices x 6: dry L/R, rev L/R,
   cho L/R, with gain, pan, sends and the C0 bits folded in), each output the exact sum rounded once
   (dry from Q12, then clipped; buses from Q14), then the dry clip. A matrix product with one rounding
   at the end: the same bits however it is grouped, which is what a matrix unit does.

Stage 2 and 3 meet only in the matrix. The A25 makes one row and mixes it at once (a one-row
matrix: best for its cache); RVV makes several rows; a Tensix tile could hand the whole matrix to
the matrix unit. No "made-apart" path: every voice goes the same way.

## Time

Two clocks: output samples (48/96 kHz) and chip ticks (14318180/288 Hz). A Q32 ratio relates them,
`tick position = tick + frac`; n samples advance it in closed form, and a tick maps back to a sample
by one multiply. Envelopes, LFOs and noise run on **ticks**, as the chip's do; only the mapping knows
the output rate. No simulated tables, no rate tables, no per-rate anything but two constants.

A hidden third ratio, exactly 1 (`hvopl3_test.hpp`, never offered as an API rate), runs the same
engine at the chip's own rate. Resampled to 48 kHz it can be compared sample by sample with a
chip-rate recording, which a 48 kHz run cannot: that is how chip details are decided from data.

## Events, not per-sample envelopes

The chip's attenuation is constant between its envelope steps, and each operator's next step is a
known tick. So a voice renders in **segments** between its next event: within one, every attenuation
is a constant in a register. The kernels have no envelope code, no branches per sample, and a
straight vector form. A slow decay costs one small event every few dozen samples.

The LFOs are pure functions of the tick counter: no state to advance through silence. Blocks split
at LFO steps only when a sounding voice uses AM or VIB.

## Silence is exact, and costs nothing

The exponent shift makes an operator's output exactly 0 from an attenuation of 0xC00 up, whatever
its phase: so a voice whose output operators are all there is skipped **without changing a sample**.
A silent modulator drops out the same way (a carrier-only kernel). The active voices are a bitmask;
an idle chip does one test per call. Phases of a skipped voice advance in closed form.

## Log domain

A wave table (10-bit phase -> log attenuation + sign) plus an exponent table and a shift, as the chip
does it: no multiply, exact by construction, every step a single lane-wise vector op with no
branches: gather, add, gather, variable shift, conditional negate.

## Data layout: structure of arrays

Operator and channel state are separate arrays indexed by operator (2 x channel + n) and channel:
lanes across voices load by index or stride, no shuffling; on the A25 it is the same cost as a
struct. Elements are at most 32 bits (T2's unit has no 64-bit elements), and everything in a kernel
fits 16-bit or 32-bit lanes. Table indices are 16-bit, with 16-bit elements (the C920 requires the
index width to equal the element width).

## RVV as built

`hvopl3_rvv.inc`: across samples for carriers and voices without feedback (16-bit lanes, phases 32-bit);
**across voices** for FM voices with feedback (`k_lanes`): one sample of up to a vector's worth of voices,
the chain in each lane, each lane's own wave offset, attenuation and shift, a strided store into the voices x
frames matrix. A lockstep chunk ends at the earliest event of its voices; a voice that stops fitting
finishes alone. Four chips are not lanes of one call yet: each chip has its own 18.

**High quality across voices** (`hq_lanes_pass`, RVV with float: the X280 and the C920; not
Zve32x): every voice but the rhythm pair a 32-bit float lane, seg_hq's
arithmetic operation for operation (no fused multiply-add, C's truncating conversions (`v_trunc`: rtz on RVV 1.0, a
corrected rounding conversion on RVV 0.7.1), the polynomials in its order, each
select a merge of what the scalar code computes on that side), so the samples are the scalar path's bits. An
operator at a time over a sub-block of 16 samples (`hq_chunk`): only op 1's feedback is a chain in time; ops
2-4 read the previous operator's outputs of the same samples (`h_v`) and add to the voices' sums (`h_sum`).
Waveforms are masks fixed per lane (0-5 one sine of x, its half or twice it, then negate/zero masks).
A chunk ends at any voice's event; a voice's lane is drawn again (gain lines, quiet or heard) only at its own
events, which are where seg_hq's segments start, and kept from one block to the next until a register write,
a key, a flag or an LFO step (`h_ok`). A quiet voice stays in its lane (its phases run there) with its row
writes masked off. The rhythm pair stays scalar (one voice; it matters only in s_rhythm).

## What goes where

| | Plan | Synthesize | Mix |
|---|---|---|---|
| A25 | scalar | scalar, registers (DSP where it pays) | scalar / DSP |
| X280, C920 | scalar | RVV: across samples where the chain allows; feedback across voices; high quality across voices | RVV |
| Tensix cores | scalar | scalar; T2: RVV (Zve32x, gathers work) | scalar; T2 RVV |
| Tensix matrix/vector units | | possible by polynomial, see below | **yes**, see below |

## Tensix matrix unit (a study)

MVMUL is Dst[8x16] += SrcB[8x16] x SrcA[16x16]: exactly coefficients (6 rows used of 8) times 16
voices x 16 samples. Its integer path has 10-bit sign-magnitude samples, 8-bit coefficients, INT32
Dst: int16 samples and our coefficients need slices and shift-adds to stay bit-exact, so it is more
natural in the float path (high quality). Hence: **mix blocks in multiples of 16 frames, voices in
groups of 16, coefficients as a plain array of rows**, and the mono matrix contiguous.

The baby RV32 cores can move data from main memory and the tile's cache through these units, so a tile program can feed them directly. The SFPU cannot gather from a table in
memory, but **a table is only one way to get a value**: the log-sine and exponent can be polynomials
on a core that cannot read a table, so synthesis on the SFPU (32 lanes) stays
an option. Chip-mode bit-exactness would need the polynomial to round to the ROM's integer values,
or that path to be a high-quality one. Keep the kernels' arithmetic plain enough to allow it: wave
lookup and exponent are separate, named steps.

## Tables

`src/hvopl3_tables.inc`, generated by `tools/gen_tables.py` and checked in (no libm dependence at
build): log-sine and exponent by the published formulas of the chip's ROMs, pan and gain. The eight
1024-entry wave tables are `constexpr`, built from the log-sine: read-only, no relocations.

## Control path

`write()` updates derived values at once; the cost is per register write, O(1), never per voice
per sample. The state is one flat caller-owned struct.
