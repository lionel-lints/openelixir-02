# Sequencer timing test (host)

Runs the real sequencer (`Core/Src/Soundengine/Sequencer/sequencer.c` and `clockSync.c`) on the
PC and writes a timeline of everything that has a time: voice triggers (`TRIG track note vel`),
MIDI out (`CLK` for clocks, `MIDI status data1 data2` for the rest), clock output ticks
(`TICK onOff cnt`), received external clock pulses (`EXT n`), and in recording scenarios the
played hits (`HIT step`) and the recorded steps (`REC step`). Every line starts with the audio
block and the system tick it happened in. The scenario itself (tempo, swing, clock, the active
steps with flam and micro-shift, track lengths, events) is at the top, as `#` lines.

```sh
test/sequencer/run.sh             # build, run all scenarios, check the timing rules, compare with golden/
test/sequencer/run.sh --capture   # write golden/ (only on purpose: it is the regression reference)
```

Linux only, like `test/engine` (on a Mac: in Docker, building into the container, not into a mounted
macOS folder). Needs gcc and Python 3.

Three checks:
- `check.py`, the timing rules of `docs/PLAN-note-swing.md`, computed independently of the firmware
  from the scenario at the top of each timeline: (A) every note at grid + swing (2nd and 4th 16th) +
  micro-shift + flam/arp position, within one audio block, with the overlap rule (a newer step's
  first hit cuts older steps' pending repeats; the newest step sounds when hits of one track are due
  in the same fire pass) and the per-pattern swing a pattern change loads; following an external
  clock the steps start on the clock, steps that play early are queued one step ahead with the
  predicted start; (B) Not mode: MIDI clock and clock outputs on the straight grid and identical to
  the same scenario without swing; rolls on the global grid (a hit where the tick count since the
  start is a multiple of the speed's interval: 384, 192, 96, 48, 32, 24, 16, 12, 8, 6 ticks), 16
  and slower with their step's swing, faster ones straight, pressed mid-step from the next tick,
  released: pending hits removed; (C) Clk mode: the pulses of a swung step later by
  swing x (1 - tick/24), the notes identical to Not mode; (D) no queue overflow; (E) the scenarios
  without swing on the internal clock compared with stock.
- recording: the recorded steps must be the played steps.
- `golden/`: every timeline must match exactly (regression).

`golden-stock/` holds the same scenarios run on the stock sequencer (1.4.1 with the recording fix,
commit 76413d0), for reference: `check.py golden-stock golden-stock` shows which rules stock breaks
(every scenario with swing or an external clock fails, and the new roll speeds). Not used as an
expected result, except (E): the scenarios without swing on the internal clock and the existing
roll speeds are identical.

Time model, from the firmware: the system tick (TIM14) runs at 108 MHz / 13501 / 2 (0.25 ms), the
audio engine renders 64-sample blocks at 44140 Hz (1.45 ms) and calls `seq_tick()` before each
block; external clocks and recorded hits are handled after `seq_tick()` in the block that reaches
them, like received MIDI. Each scenario runs in its own process, so no state is shared.

Scenarios (`main.c`): straight 16ths; swing 0-127 at 150 bpm (and 64 at 93 bpm) with flams and
micro-shifts on straight and swung steps, also a late shift past the next step's first hit; a
15-step track; a pattern change; every roll speed (One, 1/1, 1/2, 1/4, 1/8, 8T, 16, 16T, 32, 32T,
64; 1/8, 8T, 16, 32 and 64 also with swing; 64 at 150 bpm; 32 following MIDI clock; 1/4 on a
15-step track); MIDI clock (steady,
jittered +-1 ms, a tempo ramp) and analog clock (steady, jittered); stop and start in the middle of
a bar; recording 3 ms early and late, 2 ms either side of the middle between steps (with the wrap
from the last step to the first) and right after start; names ending in `_clk`: swing mode Clk.

What the stock timelines show:
- Swing stretched the clock: the MIDI clock intervals ranged 5.8-50.7 ms at swing 64 and 150 bpm
  (16.7 ms without swing), and the flams of swung steps were squeezed.
- Following an external clock, notes came one tick late, and with swing notes were dropped (the next
  external step cut off the last ticks of a swung step).
- Recording put hits played slightly early one step too early (fixed in 76413d0).
