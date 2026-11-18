# Plan: note swing, steady clock, faster rolls (1.5.0)

Agreed 2026-11-18. Base: `lionel/v1.4-fixes` (1.4.1), work on `lionel/note-swing`.

## Goal

The MIDI clock out and the clock outputs stay steady at any swing setting, while swung notes land
exactly where they do today. Faster and triplet rolls are built on the same foundation.

## How the stock timing works (for reference)

- `seq_tick()` runs once per audio block (64 samples at 44.1 kHz, 1.45 ms) and advances a
  96 ppq clock: 24 ticks per 16th step. Tick 0 calls `seq_nextStep()` (step positions, pattern
  changes, rolls); every tick calls `seq_processFlam()` (notes with flam and micro-shift); every
  4th tick sends a MIDI clock; the clock outputs tick as well.
- Swing stretches the clock: on tick 23 of every odd step the next tick interval is lengthened by
  shuffle/127 x 16 ticks and repaid on the following one. The 2nd and 4th 16th of each beat start
  late, but the MIDI clock and clock outputs swing too, and the ticks after the delay fire back to
  back (flams and shifts on swung steps get squeezed).
- Following an external clock, steps start on the external clock; swing postpones swung steps
  with `sync_delayedStepTimer`, and the next external step can cut off a swung step's last ticks.

## Decisions

Swing

1. Note positions are identical to stock: the 2nd and 4th 16th of each beat are delayed by
   shuffle/127 x 16 ticks.
2. Global config option `swg` ("Swing mode") next to `lim`, saved in `glo.cfg`: `Not` = note
   swing (default), `Clk` = stock clock swing. Older settings files get `Not`.
3. No per-track swing in this round, but all swing goes through one function,
   `swingOffsetTicks(track, step)`, so per-track swing later only changes that function, the
   storage (pattern block version 3) and the UI. No file format change now.
4. Leading and following an external clock use the same note swing; the postponing timer stays for
   `Clk` mode only. This also fixes the cut-off ticks of swung steps when following.
5. Exact time: swung hits are scheduled in system time (0.25 ms) and fire in the first audio block
   at or after it, like the stock swing. All 128 swing values stay distinct.
6. A swung step moves as a whole: its flam/arp hits and micro-shift move together. Swing belongs to
   the step a note comes from, not where it lands.
7. Overlaps, per track: the first hit of a newer step cancels the remaining hits of older steps;
   hits of one track in the same audio block are one trigger. Cross-track behaviour (hi-hat choke)
   is the sound engine's and unchanged; the shared choke flags (review S-B4) go away.
8. Live recording snaps to the swung grid in `Not` mode.

Rolls

9. Speeds, sorted: One, 1/1, 1/2, 1/4, 1/8, 8T, 16, 16T, 32, 32T, 64. The start-up default (the
   roll speed selected at power-on, `menu.c`, and the sequencer's matching value) stays "16".
   Nothing stores or transmits the roll speed as a number.
10. One to 16 (all hits on 16ths) swing as today; 8T, 16T, 32, 32T and 64 stay evenly spaced.
11. All rolls are locked to the pattern grid, as today (the step counter wraps every 6144 ticks,
    which every interval divides).
12. Triplets: rolls only in this round. Exact triplet notes (finer micro-shift or a triplet track
    mode) get their own plan; flam x3 already gives exact 32nd triplets.

Testing and delivery

13. A sequencer timing harness first: compiles the real `sequencer.c` and `clockSync.c`, drives
    `seq_tick()` per simulated audio block (internal and external clock) and records a timeline of
    voice triggers, MIDI notes and clocks out and clock-output ticks. Golden timelines are captured
    from 1.4.1 before any change. Checks: `Clk` mode reproduces them exactly; `Not` mode puts every
    hit in the same block as `Clk` (except flam spacing on swung steps and the cut rule); MIDI clock
    and clock outputs are even (within one block) at any swing, leading and following; roll grids
    and swing rules; the cut rule; recording on the swung grid; no cut-off ticks when following.
14. Commits on `lionel/note-swing`: image safety check; timing harness and golden timelines;
    exact-time hit scheduler (used by `Clk` mode first, golden timelines unchanged); note swing
    (`swg`, per-track scheduling and cut rule, external clock, recording); rolls; docs. Then a
    hardware check, merge, version 1.5.0.

## Safety: the bootloader

Nothing here touches the bootloader (0x08000000-0x08007FFF, write protected). A build check fails
the build if any firmware section lies below 0x08008000, if flash erase/program/option-byte code is
linked in, or if `UPDATE_BOOTLOADER` is not 0.

## Hardware check before merging

- MIDI clock out into a DAW or other gear at heavy swing: steady tempo.
- A swung pattern with flams: the flam spacing is intact.
- Every roll speed; recording a swung part; `swg` = `Clk` still feels like before.
